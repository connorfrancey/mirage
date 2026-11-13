#include "runtime/layers.h"

#include "game/game.h"
#include "runtime/class_source.h"
#include "runtime/classes.h"
#include "runtime/mods.h"
#include "runtime/reflect.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>
#include <toml.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <future>
#include <map>

namespace fs = std::filesystem;

namespace mirage::layers
{
	namespace
	{
		struct Spawned
		{
			ActorInfo info;
			int objectIndex = -1;		// Its GObjects slot: the actor is trusted only while the slot still holds it
		};
		struct ModLayer
		{
			int reloads = -1;			// The mod's reload count when spawned (a hot reload spawns again)
			std::vector<Spawned> actors;
			bool waiting = false;		// A class of it is being compiled (on a worker): spawned when it is done
		};

		// .uc classes compiled off the game thread (the first build reads the game's packages: seconds): by file
		struct Build
		{
			std::future<std::pair<classes::BuiltClass, std::string>> result;
			bool done = false;
			classes::BuiltClass built;
			std::string error;
			fs::file_time_type stamp;	// The file's when it was built: a change builds it again
			Classes::UClass* cls = nullptr;	// Loaded (kept while its GObjects slot still holds it)
			int slot = -1;
		};
		std::map<std::string, Build> g_builds;

		std::map<std::string, ModLayer> g_layers;	// By mod id: the current level's
		std::vector<mods::Info> g_mods;				// The mods as last checked (layers waiting on a build use it)
		std::string g_map;							// The level they were spawned in
		bool g_levelChanged = true;					// A level finished loading since (MapName is read then only)
		void* g_pawn = nullptr;						// The player's pawn then (a new one: a checkpoint restart)
		int g_generation = 0;
		unsigned g_ticks = 0;

		std::string Lower(std::string s)
		{
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		bool Alive(const Spawned& s)
		{
			if (!s.info.actor || s.objectIndex < 0) return false;
			auto& objects = Classes::UObject::GetGlobalObjects();
			return s.objectIndex < static_cast<int>(objects.Num()) && objects.GetByIndex(s.objectIndex) == s.info.actor &&
				   !s.info.actor->bDeleteMe;
		}
		int SlotOf(Classes::UObject* o)
		{
			auto& objects = Classes::UObject::GetGlobalObjects();
			for (int i = static_cast<int>(objects.Num()) - 1; i >= 0; --i)	// A new actor is near the end
				if (objects.GetByIndex(i) == o) return i;
			return -1;
		}

		void Destroy(ModLayer& layer)
		{
			for (Spawned& s : layer.actors)
				if (Alive(s)) s.info.actor->Destroy();
			layer.actors.clear();
		}

		// A package of that name is loaded (compared by name index: no strings)
		bool PackageLoaded(const std::string& package)
		{
			const int index = Classes::UObject::FindNameIndex(package);
			if (index < 0) return false;
			static Classes::UClass* packageClass = Classes::UObject::FindClass("Class Core.Package");
			auto& objects = Classes::UObject::GetGlobalObjects();
			for (int i = 0; i < static_cast<int>(objects.Num()); ++i)
				if (auto* o = objects.GetByIndex(i); o && o->Name.Index == index && o->Class == packageClass && !o->Outer) return true;
			return false;
		}

		// A class: a mod's .uc file (compiled on a worker, step 12) or a class path, loaded if its package isn't yet.
		// `pending`: the .uc is still compiling (nullptr now, ask again on a later tick).
		Classes::UClass* ResolveClass(const mods::Info& mod, const std::string& name, std::string* why, bool* pending)
		{
			if (name.size() > 3 && Lower(name.substr(name.size() - 3)) == ".uc") {
				const fs::path file = mod.dir / fs::path(std::u8string(name.begin(), name.end()));
				const std::string key = mod.id + "|" + file.string();
				std::error_code ec;
				const fs::file_time_type stamp = fs::last_write_time(file, ec);
				auto it = g_builds.find(key);
				if (it != g_builds.end() && it->second.done && it->second.stamp != stamp) {
					g_builds.erase(it);
					it = g_builds.end();
				}
				if (it == g_builds.end()) {
					Build b;
					b.stamp = stamp;
					b.result = std::async(std::launch::async, [file, owner = mod.id] {
						std::pair<classes::BuiltClass, std::string> r;
						if (!classes::BuildSource(file, r.first, &r.second, owner) && r.second.empty()) r.second = "it didn't build";
						return r;
					});
					it = g_builds.emplace(key, std::move(b)).first;
				}
				Build& b = it->second;
				if (!b.done) {
					if (b.result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
						*pending = true;
						return nullptr;
					}
					auto r = b.result.get();
					b.built = r.first;
					b.error = r.second;
					b.done = true;
				}
				// Built (kept until the file changes): loaded, quick after the first time (the engine has the package)
				if (!b.error.empty()) {
					if (why) *why = b.error;
					return nullptr;
				}
				auto& objects = Classes::UObject::GetGlobalObjects();
				if (b.cls && b.slot >= 0 && b.slot < static_cast<int>(objects.Num()) && objects.GetByIndex(b.slot) == b.cls) return b.cls;
				b.cls = classes::LoadClassFrom(b.built.package, b.built.className, why);
				b.slot = b.cls ? SlotOf(b.cls) : -1;
				return b.cls;
			}
			// Cheap checks first: a lookup of something missing compares every object's full name, and asking the engine
			// to load it searches the disk (seconds, on the game thread)
			const std::string package = name.substr(0, name.find('.'));
			const std::string className = name.substr(name.rfind('.') + 1);
			if (Classes::UObject::FindNameIndex(className) < 0) {	// Not even a name the engine has
				if (why) *why = "no class " + name;
				return nullptr;
			}
			if (auto* c = Classes::UObject::FindClass("Class " + name)) return c;
			if (package != name && PackageLoaded(package)) {		// Its package is loaded: the class isn't in it
				if (why) *why = "no class " + name;
				return nullptr;
			}
			if (auto* c = static_cast<Classes::UClass*>(classes::LoadByName(name, Classes::UClass::StaticClass()))) return c;
			if (why) *why = "no class " + name;
			return nullptr;
		}

		float Number(const toml::value& v) { return v.is_floating() ? static_cast<float>(v.as_floating()) : static_cast<float>(v.as_integer()); }

		// A property by path, from a TOML value: numbers, booleans, names (strings), objects ("Package.Object"), vectors
		// and rotators ([x, y, z])
		bool SetProperty(Classes::AActor* a, const std::string& path, const toml::value& v, std::string* why)
		{
			using layout::Kind;
			if (v.is_array()) {
				const auto& arr = v.as_array();
				static const char* xyz[] = {".X", ".Y", ".Z"};
				static const char* pyr[] = {".Pitch", ".Yaw", ".Roll"};
				const reflect::Value first = reflect::Resolve(a->Class, a, path + ".X");
				const char** parts = first ? xyz : pyr;
				for (std::size_t i = 0; i < arr.size() && i < 3; ++i)
					if (!SetProperty(a, path + parts[i], arr[i], why)) return false;
				return true;
			}
			const reflect::Value p = reflect::Resolve(a->Class, a, path);
			if (!p) {
				if (why) *why = path + ": " + p.error;
				return false;
			}
			bool ok = false;
			switch (p.member->kind) {
			case Kind::Float: ok = (v.is_floating() || v.is_integer()) && reflect::SetFloat(p, Number(v)); break;
			case Kind::Int:
			case Kind::Byte: ok = v.is_integer() && reflect::SetInt(p, static_cast<std::int32_t>(v.as_integer())); break;
			case Kind::Bool: ok = v.is_boolean() && reflect::SetInt(p, v.as_boolean() ? 1 : 0); break;
			case Kind::Name: ok = v.is_string() && reflect::SetText(p, v.as_string()); break;
			case Kind::Object:
			case Kind::Class:
			case Kind::Component: {
				if (!v.is_string()) break;
				const std::string text = v.as_string();
				Classes::UObject* o = nullptr;
				if (Lower(text) != "none") {
					o = p.member->kind == Kind::Class ? static_cast<Classes::UObject*>(Classes::UObject::FindClass("Class " + text))
													  : classes::LoadByName(text, nullptr);
					if (!o) {
						if (why) *why = path + ": no object " + text;
						return false;
					}
				}
				ok = reflect::SetObj(p, o);
				break;
			}
			default: break;
			}
			if (!ok && why) *why = path + ": a value of the wrong kind (or a kind layers can't set yet)";
			return ok;
		}

		void SpawnLayer(const mods::Info& mod, ModLayer& layer)
		{
			layer.reloads = mod.reloads;
			layer.waiting = false;
			const fs::path file = mod.dir / "levels" / (Lower(g_map) + ".toml");
			std::error_code ec;
			if (!fs::is_regular_file(file, ec)) return;
			toml::value root;
			try {
				root = toml::parse(file);
			} catch (const std::exception& e) {
				spdlog::error("[mod {}] {}: {}", mod.id, file.filename().string(), e.what());
				return;
			}
			if (!root.contains("actor")) return;
			Classes::AWorldInfo* world = game::World();
			if (!world) return;
			int index = 0;
			for (const toml::value& entry : toml::find<toml::array>(root, "actor")) {
				++index;
				Spawned s;
				s.info.mod = mod.id;
				s.info.name = toml::find_or<std::string>(entry, "name", "");
				s.info.cls = toml::find_or<std::string>(entry, "class", "");
				const std::string label = s.info.name.empty() ? "actor " + std::to_string(index) : s.info.name;
				std::string why;
				bool pending = false;
				Classes::UClass* cls = s.info.cls.empty() ? nullptr : ResolveClass(mod, s.info.cls, &why, &pending);
				if (pending) {		// Compiling: the whole layer waits for it (spawned on a later tick, in order)
					for (Spawned& done : layer.actors)
						if (Alive(done)) done.info.actor->Destroy();
					layer.actors.clear();
					layer.waiting = true;
					return;
				}
				if (!cls) {
					s.info.problem = s.info.cls.empty() ? "no class" : why;
				} else {
					Classes::FVector at{};
					Classes::FRotator rot{};
					try {
						const auto loc = toml::find<std::vector<toml::value>>(entry, "location");
						if (loc.size() == 3) at = {Number(loc[0]), Number(loc[1]), Number(loc[2])};
						else s.info.problem = "location needs [x, y, z]";
						if (entry.contains("rotation")) {
							const auto r = toml::find<std::vector<toml::value>>(entry, "rotation");
							if (r.size() == 3)
								rot = {static_cast<int>(Number(r[0])), static_cast<int>(Number(r[1])), static_cast<int>(Number(r[2]))};
						}
					} catch (const std::exception& e) {
						s.info.problem = std::string("location: ") + e.what();
					}
					if (s.info.problem.empty()) {
						s.info.actor = world->Spawn(cls, nullptr, Classes::FName{}, at, rot, nullptr, true);
						if (!s.info.actor) s.info.problem = "the engine didn't spawn it (an abstract, static or bNoDelete class?)";
					}
				}
				if (s.info.actor) {
					s.objectIndex = SlotOf(s.info.actor);
					if (entry.contains("properties"))
						for (const auto& [key, value] : toml::find<toml::table>(entry, "properties")) {
							std::string bad;
							if (!SetProperty(s.info.actor, key, value, &bad)) s.info.problem += (s.info.problem.empty() ? "" : "; ") + bad;
						}
				}
				if (!s.info.problem.empty())
					spdlog::warn("[mod {}] levels/{}.toml, {}: {}", mod.id, Lower(g_map), label, s.info.problem);
				layer.actors.push_back(std::move(s));
			}
			int spawned = 0;
			for (const Spawned& s : layer.actors) spawned += s.info.actor ? 1 : 0;
			spdlog::info("[mod {}] levels/{}.toml: {} of {} actor(s) spawned", mod.id, Lower(g_map), spawned, layer.actors.size());
		}

		void DestroyAll()
		{
			for (auto& [id, layer] : g_layers) Destroy(layer);
			g_layers.clear();
		}
	}

	void OnLevelLoaded() { g_levelChanged = true; }

	void Tick()
	{
		void* pawn = game::Pawn();
		if (!pawn) return;
		const bool newLevel = g_levelChanged;									// A level loaded (even the same one again)
		const bool restart = !newLevel && g_pawn && pawn != g_pawn;			// A new pawn in the same level
		if (newLevel || restart) {
			// A loaded level took the old actors with it; at a restart ours are destroyed for a fresh start
			if (restart) DestroyAll();
			else g_layers.clear();
			if (newLevel) g_map = game::MapName();
			g_levelChanged = false;
			++g_generation;
			g_ticks = 0;
		}
		g_pawn = pawn;
		if (g_map.empty()) return;
		// Layers waiting for a class to compile: tried again every tick until it is there
		for (auto& [id, layer] : g_layers)
			if (layer.waiting)
				for (const mods::Info& m : g_mods)
					if (m.id == id && m.loaded) {
						const auto start = std::chrono::steady_clock::now();
						SpawnLayer(m, layer);
						const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
						if (ms > 50.0) spdlog::info("[mod {}] its level layer took {:.0f} ms of the game thread", m.id, ms);
					}

		// Mods: loaded (a layer to spawn), reloaded (again), unloaded (gone). Checked on a level change or restart and
		// about every second (hot reloads, mods loading in the world phase)
		if (g_ticks++ % 60 != 0) return;
		g_mods = mods::List();
		const std::vector<mods::Info>& list = g_mods;

		for (auto it = g_layers.begin(); it != g_layers.end();) {
			const auto m = std::find_if(list.begin(), list.end(), [&](const mods::Info& i) { return i.id == it->first; });
			if (m == list.end() || !m->loaded) {
				Destroy(it->second);
				it = g_layers.erase(it);
			} else {
				++it;
			}
		}
		for (const mods::Info& m : list) {
			if (!m.loaded) continue;
			auto it = g_layers.find(m.id);
			if (it != g_layers.end() && it->second.reloads == m.reloads) continue;
			if (it != g_layers.end()) Destroy(it->second);
			const auto start = std::chrono::steady_clock::now();
			SpawnLayer(m, g_layers[m.id]);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			if (ms > 50.0) spdlog::info("[mod {}] its level layer took {:.0f} ms of the game thread", m.id, ms);
		}
	}

	void Remove(const std::string& modId)
	{
		const auto it = g_layers.find(modId);
		if (it == g_layers.end()) return;
		Destroy(it->second);
		g_layers.erase(it);
	}

	std::vector<ActorInfo> List()
	{
		std::vector<ActorInfo> out;
		for (auto& [id, layer] : g_layers)
			for (Spawned& s : layer.actors) {
				ActorInfo i = s.info;
				if (!Alive(s)) i.actor = nullptr;
				out.push_back(std::move(i));
			}
		return out;
	}

	int Generation() { return g_generation; }
}
