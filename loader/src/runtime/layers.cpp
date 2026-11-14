#include "runtime/layers.h"

#include "game/game.h"
#include "paths.h"
#ifdef GetObject		// windows.h (through paths.h) would rename call::Params::GetObject
#undef GetObject
#endif
#include "runtime/call.h"
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
#include <fstream>
#include <map>
#include <utility>

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
			fs::file_time_type stamp{};	// Its file's when spawned (the in-game editor saves it: spawned again)
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

		// A mod's sublevel in the current level: kept through restarts (the level's own sublevels are), unloaded with the
		// mod or when its layer no longer lists it
		struct Stream
		{
			SublevelInfo info;
			int slot = -1;				// The streaming object's GObjects slot
		};
		std::map<std::string, std::vector<Stream>> g_streams;	// By mod id

		std::map<std::string, ModLayer> g_layers;	// By mod id: the current level's
		std::vector<mods::Info> g_mods;				// The mods as last checked (layers waiting on a build use it)
		std::string g_map;							// The level they were spawned in
		bool g_levelChanged = true;					// A level finished loading since (MapName is read then only)
		void* g_pawn = nullptr;						// The player's pawn then (a new one: a checkpoint restart)
		int g_generation = 0;
		bool g_check = false;						// Look at the mods' layers on the next tick (the editor saved one)
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

		// A .uc file's class, compiled on a worker (step 12) and loaded. `pending`: still compiling (ask again later).
		Classes::UClass* BuiltClassOf(const fs::path& file, const std::string& owner, std::string* why, bool* pending)
		{
			{
				const std::string key = owner + "|" + file.string();
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
					b.result = std::async(std::launch::async, [file, owner] {
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
		}

		// A class: a mod's .uc file (compiled on a worker, step 12) or a class path, loaded if its package isn't yet.
		// `pending`: the .uc is still compiling (nullptr now, ask again on a later tick).
		Classes::UClass* ResolveClass(const mods::Info& mod, const std::string& name, std::string* why, bool* pending)
		{
			if (name.size() > 3 && Lower(name.substr(name.size() - 3)) == ".uc")
				return BuiltClassOf(mod.dir / fs::path(std::u8string(name.begin(), name.end())), mod.id, why, pending);
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

		// ---- Sublevels ---------------------------------------------------------------------------------------------
		// What the layers need from UnrealScript, built like a mod's class (step 12) the first time a layer streams a
		// sublevel: a new LevelStreaming object appended to the world's list, and a name made from a string
		constexpr const char* kHelperSource =
			"// Written by the menu (runtime/layers.cpp): level streaming for mods' level layers\r\n"
			"class MirageWorld extends Object;\r\n"
			"\r\n"
			"static function LevelStreaming AddStreamingLevel(WorldInfo W)\r\n"
			"{\r\n"
			"    local LevelStreamingKismet L;\r\n"
			"\r\n"
			"    L = new(W) class'LevelStreamingKismet';\r\n"
			"    W.StreamingLevels.AddItem(L);\r\n"
			"    return L;\r\n"
			"}\r\n"
			"\r\n"
			"static function name MakeName(string S)\r\n"
			"{\r\n"
			"    return name(S);\r\n"
			"}\r\n";

		Classes::UClass* Helper(std::string* why, bool* pending)
		{
			static fs::path file;
			if (file.empty()) {
				file = paths::Dir() / "classes" / "generated" / "mirage" / "MirageWorld.uc";
				std::error_code ec;
				fs::create_directories(file.parent_path(), ec);
				std::ifstream in(file, std::ios::binary);
				const std::string old((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
				in.close();
				if (old != kHelperSource) std::ofstream(file, std::ios::binary) << kHelperSource;
			}
			return BuiltClassOf(file, "mirage", why, pending);
		}

		Classes::UObject* DefaultObject(Classes::UClass* cls)
		{
			constexpr std::uint64_t kClassDefaultObject = 0x200;
			auto& objects = Classes::UObject::GetGlobalObjects();
			for (int i = static_cast<int>(objects.Num()) - 1; i >= 0; --i)
				if (auto* o = objects.GetByIndex(i);
					o && o->Class == cls && (*reinterpret_cast<std::uint64_t*>(reinterpret_cast<char*>(o) + 8) & kClassDefaultObject))
					return o;
			return nullptr;
		}

		bool Alive(const Stream& s)
		{
			if (!s.info.streaming || s.slot < 0) return false;
			auto& objects = Classes::UObject::GetGlobalObjects();
			return s.slot < static_cast<int>(objects.Num()) && objects.GetByIndex(s.slot) == s.info.streaming;
		}

		// The engine unloads it and takes it off the world's list
		void Unload(Stream& s)
		{
			if (!Alive(s)) return;
			Classes::ULevelStreaming* l = s.info.streaming;
			l->bShouldBeLoaded = 0;
			l->bShouldBeVisible = 0;
			l->bIsRequestingUnloadAndRemoval = 1;
			spdlog::info("[mod {}] sublevel {} unloaded", s.info.mod, s.info.package);
		}
		void UnloadStreams(const std::string& modId)
		{
			const auto it = g_streams.find(modId);
			if (it == g_streams.end()) return;
			for (Stream& s : it->second) Unload(s);
			g_streams.erase(it);
		}

		// A new streaming object for `package` (its file in the engine's package cache already), loading blocking
		Classes::ULevelStreaming* AddStreaming(Classes::UObject* helper, Classes::AWorldInfo* world, const std::string& package,
											   std::string& problem)
		{
			// The package's name as an FName (the engine makes it: the name table may not have it yet)
			call::Params named(helper, "MakeName");
			const std::wstring wide(package.begin(), package.end());
			const Classes::FString text(wide.c_str());
			Classes::FName fname{};
			named.SetRaw("S", &text, sizeof text);
			if (!call::Call(helper, named, &problem) || !named.GetRaw("ReturnValue", &fname, sizeof fname)) {
				if (problem.empty()) problem = "MakeName failed";
				return nullptr;
			}
			call::Params add(helper, "AddStreamingLevel");
			add.Set("W", static_cast<Classes::UObject*>(world));
			if (!call::Call(helper, add, &problem)) return nullptr;
			auto* l = static_cast<Classes::ULevelStreaming*>(add.GetObject("ReturnValue"));
			if (!l) {
				problem = "AddStreamingLevel returned nothing";
				return nullptr;
			}
			l->PackageName = fname;
			l->bShouldBeLoaded = 1;
			l->bShouldBeVisible = 1;
			l->bShouldBlockOnLoad = 1;		// Loaded in the next tick with the game waiting: the same on every run
			return l;
		}

		// The layer's [[sublevel]] entries streamed in: those already there kept, those gone from the layer unloaded.
		// False: the helper class is still compiling (the layer waits for it).
		bool StreamSublevels(const mods::Info& mod, const toml::value& root)
		{
			std::vector<std::pair<std::string, fs::path>> wanted;		// Package name, file
			if (root.contains("sublevel"))
				for (const toml::value& entry : toml::find<toml::array>(root, "sublevel")) {
					const std::string rel = toml::find_or<std::string>(entry, "package", "");
					if (rel.empty()) continue;
					const fs::path file = mod.dir / fs::path(std::u8string(rel.begin(), rel.end()));
					wanted.emplace_back(file.stem().string(), file);
				}
			const auto listed = [&](const std::string& package) {
				return std::any_of(wanted.begin(), wanted.end(), [&](const auto& w) { return Lower(w.first) == Lower(package); });
			};
			std::vector<Stream>& streams = g_streams[mod.id];
			for (auto it = streams.begin(); it != streams.end();) {
				if (listed(it->info.package) && (Alive(*it) || !it->info.problem.empty())) {
					++it;
				} else {
					Unload(*it);
					it = streams.erase(it);
				}
			}
			if (wanted.empty()) return true;
			std::string why;
			bool pending = false;
			Classes::UClass* helperClass = Helper(&why, &pending);
			if (pending) return false;
			Classes::AWorldInfo* world = game::World();
			for (const auto& [package, file] : wanted) {
				if (std::any_of(streams.begin(), streams.end(), [&](const Stream& s) { return Lower(s.info.package) == Lower(package); }))
					continue;
				Stream s;
				s.info.mod = mod.id;
				s.info.package = package;
				std::string problem;
				Classes::UObject* helper = helperClass ? DefaultObject(helperClass) : nullptr;
				if (!helperClass) problem = "the level streaming helper didn't build: " + why;
				else if (!helper) problem = "the level streaming helper has no default object";
				else if (!world) problem = "no world";
				else if (classes::CachePackage(file, &problem)) {
					s.info.streaming = AddStreaming(helper, world, package, problem);
					s.slot = s.info.streaming ? SlotOf(s.info.streaming) : -1;
				}
				s.info.problem = problem;
				if (problem.empty()) spdlog::info("[mod {}] sublevel {} streamed into {}", mod.id, package, g_map);
				else spdlog::warn("[mod {}] levels/{}.toml, sublevel {}: {}", mod.id, Lower(g_map), package, problem);
				streams.push_back(std::move(s));
			}
			return true;
		}

		// DrawScale the way the engine sets it (the components follow)
		void SetScale(Classes::AActor* a, float scale)
		{
			call::Params p(a, "SetDrawScale");
			if (p.Valid() && p.Set("NewScale", scale) && call::Call(a, p)) return;
			a->DrawScale = scale;
		}

		void SpawnLayer(const mods::Info& mod, ModLayer& layer)
		{
			layer.reloads = mod.reloads;
			layer.waiting = false;
			const fs::path file = mod.dir / "levels" / (Lower(g_map) + ".toml");
			std::error_code ec;
			layer.stamp = fs::last_write_time(file, ec);
			if (!fs::is_regular_file(file, ec)) {
				UnloadStreams(mod.id);		// It had one before a hot reload
				return;
			}
			toml::value root;
			try {
				root = toml::parse(file);
			} catch (const std::exception& e) {
				spdlog::error("[mod {}] {}: {}", mod.id, file.filename().string(), e.what());
				return;
			}
			if (!StreamSublevels(mod, root)) {	// The streaming helper is compiling: the whole layer waits for it
				layer.waiting = true;
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
					if (entry.contains("scale")) {
						const toml::value& v = toml::find(entry, "scale");
						if (v.is_floating() || v.is_integer()) SetScale(s.info.actor, Number(v));
						else s.info.problem = "scale needs a number";
					}
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
			if (restart) DestroyAll();		// Sublevels stay, like the level's own
			else {
				g_layers.clear();
				g_streams.clear();			// Gone with the old world
			}
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
		if (g_ticks++ % 60 != 0 && !std::exchange(g_check, false)) return;
		g_mods = mods::List();
		const std::vector<mods::Info>& list = g_mods;

		for (auto it = g_layers.begin(); it != g_layers.end();) {
			const auto m = std::find_if(list.begin(), list.end(), [&](const mods::Info& i) { return i.id == it->first; });
			if (m == list.end() || !m->loaded) {
				UnloadStreams(it->first);
				Destroy(it->second);
				it = g_layers.erase(it);
			} else {
				++it;
			}
		}
		for (const mods::Info& m : list) {
			if (!m.loaded) continue;
			auto it = g_layers.find(m.id);
			std::error_code ec;
			const auto stamp = fs::last_write_time(m.dir / "levels" / (Lower(g_map) + ".toml"), ec);
			if (it != g_layers.end() && it->second.reloads == m.reloads && it->second.stamp == stamp) continue;
			if (it != g_layers.end()) Destroy(it->second);
			const auto start = std::chrono::steady_clock::now();
			SpawnLayer(m, g_layers[m.id]);
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			if (ms > 50.0) spdlog::info("[mod {}] its level layer took {:.0f} ms of the game thread", m.id, ms);
		}
	}

	void Remove(const std::string& modId)
	{
		UnloadStreams(modId);
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

	std::vector<SublevelInfo> Sublevels()
	{
		std::vector<SublevelInfo> out;
		for (auto& [id, streams] : g_streams)
			for (const Stream& s : streams) {
				SublevelInfo i = s.info;
				if (!Alive(s)) {
					i.streaming = nullptr;
				} else {
					i.loaded = i.streaming->LoadedLevel != nullptr;
					i.visible = i.streaming->bIsVisible != 0;
				}
				out.push_back(std::move(i));
			}
		return out;
	}

	// ---- The in-game editor -------------------------------------------------------------------------------------------
	namespace
	{
		fs::path LayerFile(const std::string& modId)
		{
			for (const mods::Info& m : mods::List())
				if (m.id == modId) return m.dir / "levels" / (Lower(g_map) + ".toml");
			return {};
		}

		// A value as the editor shows it, and back (text that isn't TOML is a string)
		std::string ValueText(const toml::value& v)
		{
			std::string text = toml::format(v);
			while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
			return text;
		}
		toml::value TextValue(const std::string& text)
		{
			const auto parsed = toml::try_parse_str("v = " + text);
			if (parsed.is_ok() && parsed.unwrap().contains("v")) return parsed.unwrap().at("v");
			return toml::value(text);
		}
	}

	std::string CurrentMap() { return Lower(g_map); }

	bool Read(const std::string& modId, std::vector<EditActor>& out, std::string* error)
	{
		out.clear();
		const fs::path file = LayerFile(modId);
		std::error_code ec;
		if (file.empty() || !fs::is_regular_file(file, ec)) return true;
		try {
			const toml::value root = toml::parse(file);
			if (!root.contains("actor")) return true;
			for (const toml::value& entry : toml::find<toml::array>(root, "actor")) {
				EditActor a;
				a.name = toml::find_or<std::string>(entry, "name", "");
				a.cls = toml::find_or<std::string>(entry, "class", "");
				if (entry.contains("location")) {
					const auto& v = toml::find<toml::array>(entry, "location");
					for (std::size_t i = 0; i < 3 && i < v.size(); ++i) a.location[i] = Number(v[i]);
				}
				if (entry.contains("rotation")) {
					const auto& v = toml::find<toml::array>(entry, "rotation");
					for (std::size_t i = 0; i < 3 && i < v.size(); ++i) a.rotation[i] = static_cast<int>(Number(v[i]));
				}
				if (entry.contains("scale")) a.scale = Number(toml::find(entry, "scale"));
				if (entry.contains("properties"))
					for (const auto& [key, value] : toml::find<toml::table>(entry, "properties")) a.properties.emplace_back(key, ValueText(value));
				out.push_back(std::move(a));
			}
		} catch (const std::exception& e) {
			if (error) *error = e.what();
			return false;
		}
		return true;
	}

	bool Write(const std::string& modId, const std::vector<EditActor>& actors, std::string* error)
	{
		const fs::path file = LayerFile(modId);
		if (file.empty() || g_map.empty()) {
			if (error) *error = file.empty() ? "no mod " + modId : "no level";
			return false;
		}
		std::error_code ec;
		toml::value root = toml::table{};
		if (fs::is_regular_file(file, ec)) {
			try {
				root = toml::parse(file);
			} catch (const std::exception& e) {
				if (error) *error = std::string("the layer doesn't parse: ") + e.what();
				return false;
			}
		}
		toml::array list;
		for (const EditActor& a : actors) {
			toml::value entry = toml::table{};
			if (!a.name.empty()) entry["name"] = a.name;
			entry["class"] = a.cls;
			entry["location"] = toml::array{a.location[0], a.location[1], a.location[2]};
			if (a.rotation[0] || a.rotation[1] || a.rotation[2])
				entry["rotation"] = toml::array{a.rotation[0], a.rotation[1], a.rotation[2]};
			if (a.scale != 1.f) entry["scale"] = a.scale;
			if (!a.properties.empty()) {
				toml::value props = toml::table{};
				for (const auto& [key, text] : a.properties) props[key] = TextValue(text);
				entry["properties"] = props;
			}
			list.push_back(std::move(entry));
		}
		root["actor"] = toml::value(std::move(list));
		root["actor"].as_array_fmt().fmt = toml::array_format::array_of_tables;
		fs::create_directories(file.parent_path(), ec);
		std::ofstream out(file, std::ios::binary | std::ios::trunc);
		out << "# A level layer (Mirage step 13): written by the in-game Level Editor\n" << toml::format(root);
		if (!out) {
			if (error) *error = "couldn't write " + file.string();
			return false;
		}
		out.close();
		g_check = true;
		spdlog::info("[mod {}] levels/{}.toml saved: {} actor(s)", modId, Lower(g_map), actors.size());
		return true;
	}

	void Place(Classes::AActor* actor, const float location[3], const int rotation[3], float scale)
	{
		if (!actor) return;
		call::Params at(actor, "SetLocation");
		const Classes::FVector v{location[0], location[1], location[2]};
		if (!(at.Valid() && at.SetRaw("NewLocation", &v, sizeof v) && call::Call(actor, at))) actor->Location = v;
		call::Params turn(actor, "SetRotation");
		const Classes::FRotator r{rotation[0], rotation[1], rotation[2]};
		if (!(turn.Valid() && turn.SetRaw("NewRotation", &r, sizeof r) && call::Call(actor, turn))) actor->Rotation = r;
		if (actor->DrawScale != scale) SetScale(actor, scale);
	}

	Classes::AActor* Find(const std::string& modId, const std::string& name)
	{
		const auto it = g_layers.find(modId);
		if (it == g_layers.end() || name.empty()) return nullptr;
		for (const Spawned& s : it->second.actors)
			if (s.info.name == name && Alive(s)) return s.info.actor;
		return nullptr;
	}
}
