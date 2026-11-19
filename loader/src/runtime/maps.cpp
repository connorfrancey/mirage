#include "runtime/maps.h"
#include "runtime/symbols.h"

#include "game/game.h"
#include "game/levels.h"
#include "game/uobject.h"
#include "runtime/classes.h"
#include "runtime/mods.h"
#include "sdk/SDK.hpp"

#include "core/hooking.h"
#include <spdlog/spdlog.h>
#include <toml.hpp>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace fs = std::filesystem;

namespace mirage::maps
{
	namespace
	{
		// ---- The engine's pieces (found by pattern; addresses from the Steam MirrorsEdge.exe in comments) ------------
		// UUIDataStore_GameResource::InitializeListElementProviders (0x00cd13d0, __fastcall this): empties
		// ListElementProviders, then for each ElementProviderTypes entry asks GConfig for the "<Name> <ProviderClass>"
		// sections and finds (by name) or makes a provider for each. Running it again rebuilds the same list.
		using InitProvidersFn = void(__fastcall*)(void* store);
		InitProvidersFn g_initProviders = nullptr;
		// FConfigCacheIni::GetPerObjectConfigSections(const TCHAR* Filename, const FString& SearchClass,
		//                                             TArray<FString>& out_SectionNames, INT MaxResults): vtable +0x2C
		using SectionsFn = int(__thiscall*)(void* config, const wchar_t* file, const void* searchClass, void* out, int max);
		SectionsFn g_sections = nullptr;	// The original
		// GMalloc: the engine's allocator (FMalloc: +4 Malloc(size, align), +8 Realloc(p, size, align), +0xC Free(p)).
		// Strings and arrays the engine will free or grow must come from it.
		void** g_mallocVar = nullptr;

		struct RawArray
		{
			void* data;
			std::int32_t count, max;
		};
		static_assert(sizeof(RawArray) == 12);

		void* Malloc(std::uint32_t size)
		{
			void* m = *g_mallocVar;
			using Fn = void*(__thiscall*)(void*, std::uint32_t, std::uint32_t);
			return reinterpret_cast<Fn>((*reinterpret_cast<void***>(m))[1])(m, size, 8);
		}
		void* Realloc(void* p, std::uint32_t size)
		{
			void* m = *g_mallocVar;
			using Fn = void*(__thiscall*)(void*, void*, std::uint32_t, std::uint32_t);
			return reinterpret_cast<Fn>((*reinterpret_cast<void***>(m))[2])(m, p, size, 8);
		}
		void Free(void* p)
		{
			if (!p) return;
			void* m = *g_mallocVar;
			using Fn = void(__thiscall*)(void*, void*);
			reinterpret_cast<Fn>((*reinterpret_cast<void***>(m))[3])(m, p);
		}

		std::wstring Widen(const std::string& s)
		{
			if (s.empty()) return {};
			const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
			std::wstring w(static_cast<std::size_t>(n), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
			return w;
		}

		// An FString of the engine's memory holding `text` ({nullptr, 0, 0} for an empty one, as the engine has them)
		RawArray EngineString(const std::wstring& text)
		{
			if (text.empty()) return {nullptr, 0, 0};
			const auto n = static_cast<std::int32_t>(text.size() + 1);
			auto* data = static_cast<wchar_t*>(Malloc(static_cast<std::uint32_t>(n * sizeof(wchar_t))));
			std::memcpy(data, text.c_str(), static_cast<std::size_t>(n) * sizeof(wchar_t));
			return {data, n, n};
		}
		// Replaces the FString at `at` (its old memory freed)
		void SetString(void* at, const std::string& text)
		{
			auto* s = static_cast<RawArray*>(at);
			Free(s->data);
			*s = EngineString(Widen(text));
		}

		std::string Lower(std::string s)
		{
			for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		// ---- The mods' maps ------------------------------------------------------------------------------------------
		constexpr const wchar_t* kProviderClass = L"UIDataProvider_TdMaps";
		std::mutex g_mutex;
		std::vector<MapInfo> g_maps;					// Guarded by g_mutex (the hook runs on the game thread, List anywhere)
		std::vector<std::wstring> g_providerSections;	// "Mirage_<Package> UIDataProvider_TdMaps", one per listed map
		std::string g_signature;						// The mods as last registered (ids, loaded, reloads)
		unsigned g_ticks = 0;

		std::string ProviderName(const std::string& package) { return "Mirage_" + package; }

		// The config's answer, with the mods' sections added for the level select's provider class
		int __fastcall SectionsHook(void* config, void*, const wchar_t* file, const void* searchClass, void* out, int max)
		{
			const int found = g_sections(config, file, searchClass, out, max);
			const auto* cls = static_cast<const RawArray*>(searchClass);
			if (!cls->data || cls->count < 1 || _wcsicmp(static_cast<const wchar_t*>(cls->data), kProviderClass) != 0) return found;
			std::lock_guard lock(g_mutex);
			if (g_providerSections.empty()) return found;
			auto* list = static_cast<RawArray*>(out);
			for (const std::wstring& section : g_providerSections) {
				if (max > 0 && list->count >= max) break;
				if (list->count >= list->max) {
					const std::int32_t grown = std::max(list->max * 2, list->count + 4);
					list->data = Realloc(list->data, static_cast<std::uint32_t>(grown * sizeof(RawArray)));
					list->max = grown;
				}
				static_cast<RawArray*>(list->data)[list->count++] = EngineString(section);
			}
			return 1;
		}

		MapInfo Read(const mods::Info& mod, const fs::path& file)
		{
			MapInfo m;
			m.mod = mod.id;
			m.file = file;
			m.package = file.stem().string();
			m.title = m.package;
			m.gameMode = "TdGame.TdSPStoryGame";
			const fs::path details = fs::path(file).replace_extension(".toml");
			std::error_code ec;
			if (!fs::is_regular_file(details, ec)) return m;
			try {
				const toml::value root = toml::parse(details);
				m.title = toml::find_or<std::string>(root, "title", m.title);
				m.description = toml::find_or<std::string>(root, "description", "");
				m.gameMode = toml::find_or<std::string>(root, "game", m.gameMode);
				if (root.contains("checkpoint"))
					for (const toml::value& c : toml::find<toml::array>(root, "checkpoint")) {
						const std::string name = toml::find_or<std::string>(c, "name", "");
						if (!name.empty()) m.checkpoints.emplace_back(name, toml::find_or<std::string>(c, "title", name));
					}
			} catch (const std::exception& e) {
				m.problem = details.filename().string() + ": " + e.what();
			}
			return m;
		}

		// Its provider filled in from the map's details (the store made it with the class's defaults: the config has no
		// section of that name)
		bool Fill(Classes::UUIDataProvider_TdMaps* p, const MapInfo& m)
		{
			SetString(&p->Filename, m.package);
			SetString(&p->MapName, m.title);
			SetString(&p->Description, m.description);
			SetString(&p->GameMode, m.gameMode);
			SetString(&p->LevelImageMarkup, "");
			SetString(&p->LevelEvent, "");
			p->bIsTutorialMap = 0;
			p->Filter = 1;			// As the game's maps have it
			// The checkpoints: an array of the engine's memory, each entry four FStrings
			auto* list = reinterpret_cast<RawArray*>(&p->Checkpoints);
			for (std::int32_t i = 0; i < list->count; ++i)
				for (int f = 0; f < 4; ++f) Free(reinterpret_cast<RawArray*>(static_cast<char*>(list->data) + i * 0x30)[f].data);
			Free(list->data);
			*list = {nullptr, 0, 0};
			if (!m.checkpoints.empty()) {
				const auto n = static_cast<std::int32_t>(m.checkpoints.size());
				list->data = Malloc(static_cast<std::uint32_t>(n * 0x30));
				std::memset(list->data, 0, static_cast<std::size_t>(n) * 0x30);
				list->count = list->max = n;
				for (std::int32_t i = 0; i < n; ++i) {
					auto* cp = reinterpret_cast<Classes::FTdMapCheckpoint*>(static_cast<char*>(list->data) + i * 0x30);
					SetString(&cp->CheckpointFriendlyName, m.checkpoints[static_cast<std::size_t>(i)].second);
					SetString(&cp->CheckpointName, m.checkpoints[static_cast<std::size_t>(i)].first);
				}
			}
			return true;
		}

		bool IsDefault(Classes::UObject* o) { return (*reinterpret_cast<std::uint64_t*>(reinterpret_cast<char*>(o) + 8) & 0x200) != 0; }

		// The game's data stores rebuild their lists (with the mods' sections now), then the mods' providers are filled
		void Relist(std::vector<MapInfo>& maps)
		{
			if (!g_initProviders) return;
			auto& objects = Classes::UObject::GetGlobalObjects();
			int stores = 0;
			for (int i = 0; i < static_cast<int>(objects.Num()); ++i) {
				Classes::UObject* o = objects.GetByIndex(i);
				if (!o || IsDefault(o) || !game::IsA(o, Classes::UUIDataStore_TdGameData::StaticClass())) continue;
				g_initProviders(o);
				++stores;
			}
			for (int i = 0; i < static_cast<int>(objects.Num()); ++i) {
				Classes::UObject* o = objects.GetByIndex(i);
				if (!o || IsDefault(o) || !game::IsA(o, Classes::UUIDataProvider_TdMaps::StaticClass())) continue;
				const std::string name = o->GetName();
				for (MapInfo& m : maps)
					if (m.problem.empty() && name == ProviderName(m.package))
						m.listed = Fill(static_cast<Classes::UUIDataProvider_TdMaps*>(o), m);
			}
			int listed = 0;
			for (const MapInfo& m : maps) listed += m.listed ? 1 : 0;
			spdlog::info("Maps: {} mod map(s), {} in the level select ({} data store(s) relisted)", maps.size(), listed, stores);
		}

		void Register(const std::vector<mods::Info>& list)
		{
			std::vector<MapInfo> maps;
			for (const mods::Info& mod : list) {
				if (!mod.loaded) continue;
				std::error_code ec;
				// The packages a mod's maps use (a map's own content package): known to the engine by name, not maps
				for (const auto& e : fs::directory_iterator(mod.dir / "packages", ec)) {
					const std::string ext = Lower(e.path().extension().string());
					std::string problem;
					if (e.is_regular_file() && (ext == ".upk" || ext == ".u") && !classes::CachePackage(e.path(), &problem))
						spdlog::warn("[mod {}] packages/{}: {}", mod.id, e.path().filename().string(), problem);
				}
				ec.clear();
				const fs::path dir = mod.dir / "maps";
				if (!fs::is_directory(dir, ec)) continue;
				std::vector<fs::path> files;
				for (const auto& e : fs::directory_iterator(dir, ec)) {
					const std::string ext = Lower(e.path().extension().string());
					if (e.is_regular_file() && (ext == ".me1" || ext == ".upk")) files.push_back(e.path());
				}
				std::sort(files.begin(), files.end());
				for (const fs::path& file : files) {
					MapInfo m = Read(mod, file);
					const bool taken = std::any_of(maps.begin(), maps.end(), [&](const MapInfo& o) { return Lower(o.package) == Lower(m.package); });
					if (taken) m.problem = "another mod has a map named " + m.package;
					else if (m.problem.empty()) classes::CachePackage(file, &m.problem);
					if (!m.problem.empty()) spdlog::warn("[mod {}] maps/{}: {}", mod.id, file.filename().string(), m.problem);
					maps.push_back(std::move(m));
				}
			}
			{
				std::lock_guard lock(g_mutex);
				g_providerSections.clear();
				for (const MapInfo& m : maps)
					if (m.problem.empty()) g_providerSections.push_back(Widen(ProviderName(m.package)) + L" " + kProviderClass);
			}
			bool had;
			{
				std::lock_guard lock(g_mutex);
				had = !g_maps.empty();
			}
			if (had || !maps.empty()) Relist(maps);		// Nothing to add or take away otherwise
			{
				std::lock_guard lock(g_mutex);
				g_maps = maps;
			}
			levels::Refresh();		// The menu's Levels list reads the providers
		}
	}

	void** GMallocVariable() { return g_mallocVar; }

	void* EngineRealloc(void* p, std::size_t bytes)
	{
		if (!g_mallocVar || !*g_mallocVar) return nullptr;
		void* m = *g_mallocVar;
		using Fn = void*(__thiscall*)(void*, void*, std::uint32_t, std::uint32_t);
		return reinterpret_cast<Fn>((*reinterpret_cast<void***>(m))[2])(m, p, static_cast<std::uint32_t>(bytes), 8);
	}

	bool Install()
	{
		// InitializeListElementProviders: its prologue up to `lea ecx, [ebp+88h]; push ebx; call Empty`
		auto* init = symbols::Find<std::uint8_t>("InitializeListElementProviders");		// runtime/symbols.toml
		if (!init) {
			spdlog::error("Maps: the data store's InitializeListElementProviders wasn't found: mod maps won't be in the level select");
			return false;
		}
		// In it: `mov ecx, [GConfig]; mov edx, [ecx]; push 400h` before the GetPerObjectConfigSections call, and
		// `mov ecx, [GMalloc]; cmp ecx, ebx; mov esi, eax; jnz` freeing the strings after it
		auto* config = static_cast<std::uint8_t*>(game::FindPatternIn(init, 0x200, "\x8B\x0D\x00\x00\x00\x00\x8B\x11\x68\x00\x04\x00\x00",
																		  "xx????xxxxxxx"));
		auto* malloc = static_cast<std::uint8_t*>(game::FindPatternIn(init, 0x200, "\x8B\x0D\x00\x00\x00\x00\x3B\xCB\x8B\xF0\x75",
																		  "xx????xxxxx"));
		if (!config || !malloc) {
			spdlog::error("Maps: GConfig or GMalloc wasn't found in InitializeListElementProviders");
			return false;
		}
		void** configVar = *reinterpret_cast<void***>(config + 2);
		g_mallocVar = *reinterpret_cast<void***>(malloc + 2);
		symbols::Provide("GConfig", reinterpret_cast<std::uintptr_t>(configVar), "data", "The engine's config cache (FConfigCacheIni*)");
		symbols::Provide("GMalloc", reinterpret_cast<std::uintptr_t>(g_mallocVar), "data", "The engine's allocator (FMalloc*)");
		if (!*configVar || !*g_mallocVar) {
			spdlog::error("Maps: GConfig or GMalloc isn't set yet");
			return false;
		}
		void* target = (*reinterpret_cast<void***>(*configVar))[0x2C / 4];
		if (const hooking::Status st = hooking::Install(target, &SectionsHook, &g_sections, "GetPerObjectConfigSections"); !st) {
			spdlog::error("Maps: could not hook GetPerObjectConfigSections: {}", st.error);
			return false;
		}
		g_initProviders = reinterpret_cast<InitProvidersFn>(init);
		spdlog::info("Maps: InitializeListElementProviders at {}, GetPerObjectConfigSections at {} hooked", static_cast<void*>(init), target);
		return true;
	}

	void Tick()
	{
		if (g_ticks++ % 60 != 0) return;		// About a second: mods loading in later phases, hot reloads
		const std::vector<mods::Info> list = mods::List();
		std::string signature;
		for (const mods::Info& m : list)
			if (m.loaded) signature += m.id + ":" + std::to_string(m.reloads) + ";";
		if (signature == g_signature) return;
		g_signature = signature;
		Register(list);
	}

	std::vector<MapInfo> List()
	{
		std::lock_guard lock(g_mutex);
		return g_maps;
	}
}
