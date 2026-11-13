#pragma once
// Classes from UnrealScript source (Mirage, step 12): a mod's `.uc` file built into a script package by UnrealLib's
// class package builder, then loaded by the engine like any cooked class (runtime/classes.h).
//
// The package goes to <memenu>\classes\, outside the mod's folder (the hot reload watcher would see it), named after the
// class and a hash of the source: "MirageBouncePad_1a2b3c4d". The engine keeps a loaded package for the session, so the
// same source loads the class it already has, and an edited source is a new package with a new class of the same name
// (hot reload). A package already built for the same source isn't built again.
//
// A class can also be described instead of written (DefineClass, for Lua and C# mods): its variables, defaults and
// functions; the runtime writes the `.uc` (an override takes the parent's declaration, with an empty body the mod's
// handler fills through a script injection) and builds it the same way.
//
// `native("Name")` in a mod's source is that mod's registered native (step 11): its index in the engine's native table.
//
// Building reads the game's script packages (Core, Engine, GameFramework, IpDrv, TdGame) with UnrealLib once; they stay
// loaded for later builds until Release().

#include <filesystem>
#include <string>
#include <vector>

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

	// `uc` built into a package; `owner` (a mod's id) resolves its native("Name") declarations. Any thread (it doesn't
	// touch the engine); one build at a time. False and `error` (the compiler's errors, "MirageBouncePad.uc line 12:
	// ...") if it doesn't build.
	bool BuildSource(const std::filesystem::path& uc, BuiltClass& out, std::string* error, const std::string& owner = "");

	// BuildSource, then the class loaded from the package. Game thread. nullptr and `error` if either fails.
	Classes::UClass* LoadSource(const std::filesystem::path& uc, std::string* error, BuiltClass* built = nullptr,
								const std::string& owner = "");

	// A class described rather than written
	struct ClassSpec
	{
		std::string name, extends;
		bool placeable = false;
		std::vector<std::string> vars;			// "float Strength", "var() int Count" (the `var` is optional)
		std::vector<std::string> functions;		// "Touch": an override, the parent's declaration with an empty body;
												// "function int Count()": a new one (also with an empty body)
		std::vector<std::string> defaults;		// "Strength=1500.0"
		std::string extra;						// More UnrealScript, as written (states, structs, functions with bodies)
	};
	// The `.uc` source for `spec`. False and `error` if a function to override isn't in the parent classes.
	bool GenerateSource(const ClassSpec& spec, std::string& out, std::string* error);
	// GenerateSource, written to <memenu>\classes\generated\<owner>\<Name>.uc, built and loaded. Game thread.
	Classes::UClass* DefineClass(const ClassSpec& spec, const std::string& owner, std::string* error, BuiltClass* built = nullptr);

	// The declaration of `function` as `parent` (or a class above it) declares it: "event Touch(Actor Other, ...)".
	// Empty and `error` if there is none.
	std::string ParentDeclaration(const std::string& parent, const std::string& function, std::string* error);

	// The game's packages read for building, released (memory)
	void Release();
}
