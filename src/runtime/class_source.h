#pragma once
// Classes from UnrealScript source (Mirage, step 12): a mod's `.uc` file built into a script package by UnrealLib's
// class package builder, then loaded by the engine like any cooked class (runtime/classes.h).
//
// The package goes to <memenu>\classes\, outside the mod's folder (the hot reload watcher would see it), named after the
// class and a hash of the source: "MirageBouncePad_1a2b3c4d". The engine keeps a loaded package for the session, so the
// same source loads the class it already has, and an edited source is a new package with a new class of the same name
// (hot reload). A package already built for the same source isn't built again.
//
// Building reads the game's script packages (Core, Engine, GameFramework, IpDrv, TdGame) with UnrealLib once; they stay
// loaded for later builds until Release().

#include <filesystem>
#include <string>

namespace Classes
{
	class UClass;
}

namespace mirage::classes
{
	struct BuiltClass
	{
		std::filesystem::path package;		// The package file
		std::string className;				// The class it holds (the file's stem)
		bool cached = false;				// It was there already: not built again
		double seconds = 0.0;				// Building it (with reading the game's packages the first time)
	};

	// `uc` built into a package. Any thread (it doesn't touch the engine); one build at a time. False and `error` (the
	// compiler's errors, "MirageBouncePad.uc line 12: ...") if it doesn't build.
	bool BuildSource(const std::filesystem::path& uc, BuiltClass& out, std::string* error);

	// BuildSource, then the class loaded from the package. Game thread. nullptr and `error` if either fails.
	Classes::UClass* LoadSource(const std::filesystem::path& uc, std::string* error, BuiltClass* built = nullptr);

	// The game's packages read for building, released (memory)
	void Release();
}
