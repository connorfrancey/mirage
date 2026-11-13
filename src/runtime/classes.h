#pragma once
// Classes from mods (Mirage, step 12): a script package (.u) a mod ships or the runtime builds (UnrealLib's class
// package builder) is loaded by the engine itself, from any file: UObject::StaticLoadObject with a filename resolves
// the package from that path (UObject::GetPackageLinker), so the engine links its classes, builds their default objects
// and keeps them like any cooked class. The file is added to the engine's package cache first
// (FPackageFileCache::CachePackage, as the engine adds its own), then loaded by name. No copy into the game's folders.

#include <filesystem>
#include <string>
#include <string_view>

namespace Classes
{
	class UObject;
	class UClass;
}

namespace mirage::classes
{
	// StaticLoadObject found in the executable (by pattern). False and `why` if not.
	bool Ready(std::string* why = nullptr);

	// `object` ("Package.Name") of class `cls` from the package file `file` (its name is the file's stem). Game thread.
	// nullptr and `error` when it isn't there.
	Classes::UObject* LoadFrom(const std::filesystem::path& file, std::string_view object, Classes::UClass* cls,
							   std::string* error = nullptr);
	Classes::UClass* LoadClassFrom(const std::filesystem::path& file, std::string_view className, std::string* error = nullptr);
}
