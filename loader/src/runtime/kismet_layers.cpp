#include "runtime/kismet_layers.h"

#include "game/game.h"
#include "game/uobject.h"
#include "runtime/kismet.h"
#include "runtime/layer_values.h"
#include "runtime/mods.h"
#include "runtime/reflect.h"
#include "sdk/SDK.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <toml.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>

namespace fs = std::filesystem;

namespace mirage::kismet_layers
{
	namespace
	{
		// An object the layer touched: trusted while its GObjects slot still holds it (never dereferenced before)
		struct Ref
		{
			Classes::UObject* object = nullptr;
			std::size_t slot = 0;
			bool Alive() const { return game::AliveAt(object, slot); }
		};
		Ref RefOf(Classes::UObject* o) { return o ? Ref{o, game::IndexOf(o)} : Ref{}; }

		struct Applied
		{
			int reloads = -1;				// The mod's reload count when applied (a hot reload applies it again)
			fs::file_time_type stamp{};		// Its file's then (an edited file applies it again)
			bool waiting = false;			// A class of it is compiling: applied on a later tick
			std::vector<std::function<void()>> undo;	// Newest last
			std::vector<Entry> entries;
		};
		std::map<std::string, Applied> g_applied;	// By mod id: the current level's
		std::string g_map;
		bool g_levelChanged = true;
		unsigned g_ticks = 0;

		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		void Undo(Applied& a)
		{
			for (auto it = a.undo.rbegin(); it != a.undo.rend(); ++it) (*it)();
			a.undo.clear();
			a.entries.clear();
		}

		fs::path FileOf(const mods::Info& mod) { return mod.dir / "kismet" / (Lower(g_map) + ".toml"); }

		// What one application of a layer works with: the levels' sequence objects and the layer's own ops by name
		struct Context
		{
			const mods::Info& mod;
			Applied& applied;
			std::vector<Classes::USequenceObject*> objects;
			std::map<std::string, Classes::USequenceOp*> own;	// By lowercase name

			// An op (or sequence) of the level by name or path ending, or the layer's own; nullptr and `why` if there
			// is none or more than one
			Classes::USequenceObject* Find(const std::string& ref, std::string* why) const
			{
				if (ref.empty()) {
					*why = "no op named";
					return nullptr;
				}
				if (const auto it = own.find(Lower(ref)); it != own.end()) return it->second;
				const std::string want = Lower(ref);
				const bool byPath = ref.find('.') != std::string::npos;
				std::vector<Classes::USequenceObject*> found;
				for (Classes::USequenceObject* o : objects) {
					if (!byPath) {
						if (Lower(o->GetName()) == want) found.push_back(o);
						continue;
					}
					const std::string path = Lower(o->GetPathName());
					if (path == want || (path.size() > want.size() && path.ends_with(want) && path[path.size() - want.size() - 1] == '.'))
						found.push_back(o);
				}
				if (found.size() > 1) {		// The map's own level first (every level has a Main_Sequence), then its sublevels
					std::vector<Classes::USequenceObject*> inMap;
					for (Classes::USequenceObject* o : found)
						if (Lower(kismet::LevelOf(o)) == Lower(g_map)) inMap.push_back(o);
					if (inMap.size() == 1) return inMap.front();
				}
				if (found.size() == 1) return found.front();
				*why = found.empty() ? "nothing named " + ref + " in the level's Kismet"
									 : fmt::format("{} objects named {}: give more of its path (\"Sub_Sequence.{}\")", found.size(), ref, ref);
				return nullptr;
			}
			Classes::USequenceOp* FindOp(const std::string& ref, std::string* why) const
			{
				Classes::USequenceObject* o = Find(ref, why);
				if (o && !o->IsA(Classes::USequenceOp::StaticClass())) {
					*why = ref + " isn't an op (a variable?)";
					return nullptr;
				}
				return static_cast<Classes::USequenceOp*>(o);
			}
			// The level's root sequence (its package is the map's)
			Classes::USequence* Root() const
			{
				for (Classes::USequenceObject* o : objects)
					if (o->IsA(Classes::USequence::StaticClass()) && !o->ParentSequence && Lower(kismet::LevelOf(o)) == Lower(g_map))
						return static_cast<Classes::USequence*>(o);
				return nullptr;
			}

			void Add(std::string what, std::string problem = {})
			{
				if (!problem.empty()) spdlog::warn("[mod {}] kismet/{}.toml, {}: {}", mod.id, Lower(g_map), what, problem);
				applied.entries.push_back({mod.id, std::move(what), std::move(problem)});
			}

			// Properties from a TOML table, each one's old bytes kept to put back
			std::string SetProperties(Classes::UObject* o, const toml::value& table)
			{
				std::string problems;
				for (const auto& [key, value] : table.as_table()) {
					const reflect::Value v = reflect::Resolve(o->Class, o, key);
					if (!v) {
						problems += (problems.empty() ? "" : "; ") + key + ": " + v.error;
						continue;
					}
					std::vector<std::uint8_t> old(v.address, v.address + v.member->elementSize);
					std::string why;
					if (!layers::SetValue(o, key, value, &why)) {
						problems += (problems.empty() ? "" : "; ") + why;
						continue;
					}
					applied.undo.push_back([r = RefOf(o), at = v.address, old = std::move(old)] {
						if (r.Alive()) std::memcpy(at, old.data(), old.size());
					});
				}
				return problems;
			}
		};

		std::string Text(const toml::value& entry, const char* key)
		{
			if (!entry.contains(key)) return {};
			const toml::value& v = toml::find(entry, key);
			if (v.is_integer()) return std::to_string(v.as_integer());
			return v.is_string() ? v.as_string() : std::string();
		}

		// Applies `mod`'s layer for the current level; false: a class is still compiling (everything applied so far
		// undone: the whole layer goes in, in order, on a later tick)
		bool Apply(const mods::Info& mod, Applied& applied)
		{
			applied.reloads = mod.reloads;
			applied.waiting = false;
			const fs::path file = FileOf(mod);
			std::error_code ec;
			applied.stamp = fs::last_write_time(file, ec);
			if (!fs::is_regular_file(file, ec)) return true;
			toml::value root;
			try {
				root = toml::parse(file);
			} catch (const std::exception& e) {
				spdlog::error("[mod {}] kismet/{}: {}", mod.id, file.filename().string(), e.what());
				return true;
			}
			Context c{mod, applied, kismet::LevelObjects(), {}};
			const auto list = [&](const char* key) {
				return root.contains(key) && root.at(key).is_array() ? root.at(key).as_array() : toml::array{};
			};

			for (const toml::value& entry : list("op")) {
				const std::string name = Text(entry, "name"), clsName = Text(entry, "class");
				const std::string what = fmt::format("op {} ({})", name, clsName);
				std::string why;
				bool pending = false;
				Classes::UClass* cls = layers::ClassFor(mod, clsName, &why, &pending);
				if (pending) {
					Undo(applied);
					applied.waiting = true;
					return false;
				}
				if (!cls) {
					c.Add(what, why);
					continue;
				}
				Classes::USequence* seq = nullptr;
				if (const std::string ref = Text(entry, "sequence"); !ref.empty()) {
					Classes::USequenceObject* s = c.Find(ref, &why);
					if (s && !s->IsA(Classes::USequence::StaticClass())) why = ref + " isn't a sequence";
					else seq = static_cast<Classes::USequence*>(s);
				} else if (!(seq = c.Root())) {
					why = "the level has no root sequence";
				}
				if (!seq) {
					c.Add(what, why);
					continue;
				}
				Classes::USequenceOp* op = kismet::Construct(seq, cls, &why);
				if (!op) {
					c.Add(what, why);
					continue;
				}
				applied.undo.push_back([s = RefOf(seq), o = RefOf(op)] {
					if (s.Alive() && o.Alive())
						kismet::RemoveOp(static_cast<Classes::USequence*>(s.object), static_cast<Classes::USequenceObject*>(o.object));
				});
				if (!name.empty()) c.own[Lower(name)] = op;
				std::string problems;
				if (entry.contains("properties")) problems = c.SetProperties(op, toml::find(entry, "properties"));
				c.Add(what + " = " + op->GetName(), problems);
			}

			const auto links = [&](const char* key, bool add) {
				for (const toml::value& entry : list(key)) {
					const std::string fromRef = Text(entry, "from"), toRef = Text(entry, "to");
					const std::string outRef = Text(entry, "output"), inRef = Text(entry, "input");
					std::string what = fmt::format("{} {}[{}] -> {}[{}]", key, fromRef, outRef, toRef, inRef);
					std::string why;
					Classes::USequenceOp* from = c.FindOp(fromRef, &why);
					Classes::USequenceOp* to = from ? c.FindOp(toRef, &why) : nullptr;
					if (!from || !to) {
						c.Add(what, why);
						continue;
					}
					const int out = kismet::OutputIndex(from, outRef.c_str());
					const int in = kismet::InputIndex(to, inRef.c_str());
					if (out < 0 || in < 0) {
						c.Add(what, out < 0 ? fromRef + " has no output " + outRef : toRef + " has no input " + inRef);
						continue;
					}
					if (add) {
						const kismet::LinkResult r = kismet::AddLink(from, out, to, in, -1, &why);
						if (r == kismet::LinkResult::Failed) {
							c.Add(what, why);
							continue;
						}
						if (r == kismet::LinkResult::Added)
							applied.undo.push_back([f = RefOf(from), t = RefOf(to), out, in] {
								if (f.Alive() && t.Alive())
									kismet::RemoveLink(static_cast<Classes::USequenceOp*>(f.object), out, static_cast<Classes::USequenceOp*>(t.object), in);
							});
					} else {
						const int at = kismet::RemoveLink(from, out, to, in);
						if (at < 0) {
							c.Add(what, "there is no such link");
							continue;
						}
						applied.undo.push_back([f = RefOf(from), t = RefOf(to), out, in, at] {
							if (f.Alive() && t.Alive())
								kismet::AddLink(static_cast<Classes::USequenceOp*>(f.object), out, static_cast<Classes::USequenceOp*>(t.object), in, at, nullptr);
						});
					}
					c.Add(what);
				}
			};
			links("unlink", false);
			links("link", true);

			for (const toml::value& entry : list("set")) {
				const std::string ref = Text(entry, "op");
				std::string why;
				Classes::USequenceObject* o = c.Find(ref, &why);
				if (o && !entry.contains("properties")) why = "no [set.properties]";
				if (!o || !why.empty()) {
					c.Add("set " + ref, why);
					continue;
				}
				c.Add("set " + ref, c.SetProperties(o, toml::find(entry, "properties")));
			}
			int skipped = 0;
			for (const Entry& e : applied.entries) skipped += !e.problem.empty();
			spdlog::info("[mod {}] Kismet layer for {}: {} entr{} applied, {} skipped", mod.id, g_map,
						 applied.entries.size() - skipped, applied.entries.size() - skipped == 1 ? "y" : "ies", skipped);
			return true;
		}
	}

	void OnLevelLoaded() { g_levelChanged = true; }

	void Tick()
	{
		if (game::IsLoading() || !game::Pawn()) return;
		if (g_levelChanged) {
			g_applied.clear();			// The old level's sequences went with it: nothing to undo
			g_map = game::MapName();
			g_levelChanged = false;
			g_ticks = 0;
		}
		if (g_map.empty()) return;
		bool waiting = false;
		for (auto& [id, a] : g_applied) waiting |= a.waiting;
		if (g_ticks++ % 60 != 0 && !waiting) return;	// About every second (and every tick while a class compiles)
		const std::vector<mods::Info> list = mods::List();
		for (auto it = g_applied.begin(); it != g_applied.end();) {
			const auto m = std::find_if(list.begin(), list.end(), [&](const mods::Info& i) { return i.id == it->first; });
			if (m == list.end() || !m->loaded) {
				Undo(it->second);
				it = g_applied.erase(it);
			} else {
				++it;
			}
		}
		for (const mods::Info& m : list) {
			if (!m.loaded) continue;
			auto it = g_applied.find(m.id);
			std::error_code ec;
			const auto stamp = fs::last_write_time(FileOf(m), ec);
			if (it != g_applied.end() && !it->second.waiting && it->second.reloads == m.reloads && it->second.stamp == stamp) continue;
			Applied& a = g_applied[m.id];
			Undo(a);
			Apply(m, a);
		}
	}

	void Remove(const std::string& modId)
	{
		const auto it = g_applied.find(modId);
		if (it == g_applied.end()) return;
		Undo(it->second);
		g_applied.erase(it);
	}

	std::vector<Entry> List()
	{
		std::vector<Entry> out;
		for (const auto& [id, a] : g_applied) out.insert(out.end(), a.entries.begin(), a.entries.end());
		return out;
	}
}
