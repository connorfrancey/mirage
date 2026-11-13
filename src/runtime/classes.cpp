#include "runtime/classes.h"

#include "game/game.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>

#include <windows.h>

#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

namespace mirage::classes
{
	namespace
	{
		// UObject::StaticLoadObject(UClass* Class, UObject* InOuter, const TCHAR* Name, const TCHAR* Filename,
		//                           DWORD LoadFlags, UPackageMap* Sandbox, UBOOL bAllowObjectReconciliation)
		using StaticLoadObjectFn = Classes::UObject*(__cdecl*)(Classes::UClass*, Classes::UObject*, const wchar_t*, const wchar_t*,
															   unsigned long, void*, int);
		StaticLoadObjectFn g_load = nullptr;
		void** g_cacheVar = nullptr;	// &GPackageFileCache (an FPackageFileCache*)
		std::once_flag g_found;			// The patterns are looked for once (any thread: Prepare does it on a worker)
		// FPackageFileCache::CachePackage(const TCHAR* InPathName, UBOOL InOverrideDupe, UBOOL WarnIfExists): virtual 1
		using CachePackageFn = int(__thiscall*)(void* self, const wchar_t* path, int overrideDupe, int warnIfExists);

		void Look()
		{
			// Its prologue: SEH frame, a 0x68-byte stack frame, the security cookie, `mov eax, [ebp+10h]` (Name)
			g_load = reinterpret_cast<StaticLoadObjectFn>(game::FindPattern(
				"\x55\x8B\xEC\x6A\xFF\x68\x00\x00\x00\x00\x64\xA1\x00\x00\x00\x00\x50\x83\xEC\x68\x53\x56\x57\xA1\x00\x00\x00\x00"
				"\x33\xC5\x50\x8D\x45\xF4\x64\xA3\x00\x00\x00\x00\x89\x65\xF0\x8B\x45\x10\x33\xFF",
				"xxxxxx????xxxxxxxxxxxxxx????xxxxxxxxxxxxxxxxxxxx"));
			// GPackageFileCache: `mov ecx, [GPackageFileCache]` before its FindPackageFile call in GetPackageLinker
			if (auto* at = static_cast<std::uint8_t*>(game::FindPattern(
					"\x8B\x0D\x00\x00\x00\x00\x8B\x11\x8B\x52\x08\x53\x8D\x45\xAC\x50\x8B\x45\xC4\x50\x57\xFF\xD2",
					"xx????xxxxxxxxxxxxxxxxx")))
				g_cacheVar = *reinterpret_cast<void***>(at + 2);
			if (g_load && g_cacheVar)
				spdlog::info("Classes: StaticLoadObject at {}, GPackageFileCache at {}", reinterpret_cast<void*>(g_load), static_cast<void*>(g_cacheVar));
			else
				spdlog::error("Classes: StaticLoadObject or GPackageFileCache wasn't found");
			if (!g_cacheVar) g_load = nullptr;
		}
		StaticLoadObjectFn Find()
		{
			std::call_once(g_found, Look);
			return g_load;
		}

		// The calls themselves, guarded (no C++ objects here: __try can't unwind them)
		bool CacheGuarded(const wchar_t* path)
		{
			__try
			{
				void* cache = *g_cacheVar;
				if (!cache) return false;
				auto cachePackage = reinterpret_cast<CachePackageFn>((*reinterpret_cast<void***>(cache))[1]);
				return cachePackage(cache, path, 1, 0) != 0;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}
		Classes::UObject* Guarded(StaticLoadObjectFn load, Classes::UClass* cls, const wchar_t* name)
		{
			__try
			{
				return load(cls, nullptr, name, nullptr, 0, nullptr, 1);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
		}
	}

	void Prepare()
	{
		std::thread([] { Find(); }).detach();
	}

	bool Ready(std::string* why)
	{
		if (Find()) return true;
		if (why) *why = "UObject::StaticLoadObject wasn't found in the executable";
		return false;
	}

	Classes::UObject* LoadFrom(const std::filesystem::path& file, std::string_view object, Classes::UClass* cls, std::string* error)
	{
		StaticLoadObjectFn load = Find();
		if (!load)
		{
			if (error) *error = "UObject::StaticLoadObject wasn't found";
			return nullptr;
		}
		std::error_code ec;
		if (!std::filesystem::is_regular_file(file, ec))
		{
			if (error) *error = "no file " + file.string();
			return nullptr;
		}
		const std::wstring name(object.begin(), object.end());
		const std::wstring path = std::filesystem::absolute(file, ec).wstring();
		// The package file joins the engine's package cache (its name: the file's stem), then loads like any package
		if (!CacheGuarded(path.c_str()))
		{
			if (error) *error = "the engine's package cache refused " + file.string();
			return nullptr;
		}
		Classes::UObject* result = Guarded(load, cls, name.c_str());
		if (!result && error) *error = std::string(object) + " didn't load from " + file.string() + " (the game's log says why)";
		return result;
	}

	bool CachePackage(const std::filesystem::path& file, std::string* error)
	{
		if (!Find())
		{
			if (error) *error = "GPackageFileCache wasn't found";
			return false;
		}
		std::error_code ec;
		if (!std::filesystem::is_regular_file(file, ec))
		{
			if (error) *error = "no file " + file.string();
			return false;
		}
		const std::wstring path = std::filesystem::absolute(file, ec).wstring();
		if (CacheGuarded(path.c_str())) return true;
		if (error) *error = "the engine's package cache refused " + file.string();
		return false;
	}

	Classes::UObject* LoadByName(std::string_view path, Classes::UClass* cls)
	{
		StaticLoadObjectFn load = Find();
		if (!load) return nullptr;
		const std::wstring name(path.begin(), path.end());
		return Guarded(load, cls ? cls : Classes::UObject::StaticClass(), name.c_str());
	}

	Classes::UClass* LoadClassFrom(const std::filesystem::path& file, std::string_view className, std::string* error)
	{
		const std::string object = file.stem().string() + "." + std::string(className);
		return static_cast<Classes::UClass*>(LoadFrom(file, object, Classes::UClass::StaticClass(), error));
	}

	namespace
	{
		constexpr std::size_t kClassFlagsOffset = 0xE4;		// UClass's first member, after UState (SDK: UClass at 0xE4)
		constexpr std::uint32_t kClassAbstract = 0x1;
		constexpr std::uint64_t kRootSet = 0x4000;			// RF_RootSet
		constexpr std::uint64_t kNotAnInstance = 0x200 | 0x400;	// RF_ClassDefaultObject | RF_ArchetypeObject
		std::uint32_t& Flags(Classes::UClass* cls) { return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<char*>(cls) + kClassFlagsOffset); }
		std::uint64_t& ObjectFlags(Classes::UObject* o) { return *reinterpret_cast<std::uint64_t*>(reinterpret_cast<char*>(o) + 0x8); }
		std::vector<Classes::UClass*> g_retired;		// Made abstract by Retire (Revive undoes only these)
	}

	std::uint32_t ClassFlags(const Classes::UClass* cls)
	{
		return cls ? *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const char*>(cls) + kClassFlagsOffset) : 0;
	}

	int Retire(Classes::UClass* cls)
	{
		if (!cls) return 0;
		int destroyed = 0;
		auto& objects = Classes::UObject::GetGlobalObjects();
		std::vector<Classes::AActor*> actors;
		for (int i = 0; i < static_cast<int>(objects.Num()); ++i) {
			Classes::UObject* o = objects.GetByIndex(i);
			if (!o || o == cls || (ObjectFlags(o) & kNotAnInstance) || !o->IsA(cls)) continue;
			if (o->IsA(Classes::AActor::StaticClass())) actors.push_back(static_cast<Classes::AActor*>(o));
		}
		for (Classes::AActor* a : actors)
			if (a->Destroy()) ++destroyed;
		if (!(Flags(cls) & kClassAbstract)) {
			Flags(cls) |= kClassAbstract;
			g_retired.push_back(cls);
		}
		spdlog::info("Classes: {} retired ({} actors destroyed)", cls->GetPathName(), destroyed);
		return destroyed;
	}

	void Revive(Classes::UClass* cls)
	{
		const auto it = std::find(g_retired.begin(), g_retired.end(), cls);
		if (it == g_retired.end()) return;
		Flags(cls) &= ~kClassAbstract;
		g_retired.erase(it);
	}

	void Keep(Classes::UObject* object, bool keep)
	{
		if (!object) return;
		if (keep) ObjectFlags(object) |= kRootSet;
		else ObjectFlags(object) &= ~kRootSet;
	}
}
