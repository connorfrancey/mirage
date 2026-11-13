#include "runtime/classes.h"

#include "game/game.h"
#include "sdk/SDK.hpp"

#include <spdlog/spdlog.h>

#include <windows.h>

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
		bool g_tried = false;
		// FPackageFileCache::CachePackage(const TCHAR* InPathName, UBOOL InOverrideDupe, UBOOL WarnIfExists): virtual 1
		using CachePackageFn = int(__thiscall*)(void* self, const wchar_t* path, int overrideDupe, int warnIfExists);

		StaticLoadObjectFn Find()
		{
			if (g_tried) return g_load;
			g_tried = true;
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

	Classes::UClass* LoadClassFrom(const std::filesystem::path& file, std::string_view className, std::string* error)
	{
		const std::string object = file.stem().string() + "." + std::string(className);
		return static_cast<Classes::UClass*>(LoadFrom(file, object, Classes::UClass::StaticClass(), error));
	}
}
