#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The engine's functions and globals by name (step 2 of the plan's next ten steps; the first part of step 32).
//
// Most come from runtime/symbols.toml, built into the DLL: a pattern MirageHook's scanner looks for in the game's code
// at start (each must match exactly once; more than one is reported, and the first is used, as the old scans did),
// then resolve steps from the match (an offset, a dereference, a call's target). The rest are names the runtime's own
// code works out (found relative to those, or from the engine's objects) and registers with Provide. Mods find either
// kind by name (symbol_find, detour_symbol), and so do the runtime's hooks: one place says where the engine is.
namespace mirage::symbols
{
	struct Info
	{
		std::string name;
		std::string kind;			// "code" or "data"
		std::string description;
		std::uintptr_t address = 0;	// 0: not found
		int matches = 0;			// Pattern entries: how many places matched; provided ones: 1
		int expected = 1;			// How many the table says it matches (symbols.toml's `matches`)
		bool provided = false;		// Registered by the runtime's code, not from symbols.toml
		std::string problem;		// Why it isn't found, or what is wrong with it
	};

	// Looks for every entry of symbols.toml (once; again only if the table is reloaded). Any thread, before the first
	// Find of a pattern entry: game::Init calls it first thing. Returns how many were found.
	int Resolve();

	// A name the runtime's code found: replaces an earlier address of the same name.
	void Provide(std::string_view name, std::uintptr_t address, std::string_view kind = "code", std::string_view description = {});

	// The address of `name` (case-sensitive), 0 if it isn't known or wasn't found. Any thread.
	std::uintptr_t Find(std::string_view name);
	template <class T>
	T* Find(std::string_view name) { return reinterpret_cast<T*>(Find(name)); }

	std::vector<Info> List();			// Every symbol, in name order
	std::string NameOf(std::uintptr_t address);		// The symbol at exactly that address ("" if none)

	// The built-in table's text (symbols.toml), parsed into entries; for the unit tests and the developer tools
	struct Entry
	{
		std::string name, kind, pattern, description;
		std::vector<std::string> resolve;
		int matches = 1;
	};
	std::vector<Entry> Parse(std::string_view toml, std::string* error = nullptr);
	std::string_view BuiltIn();
}
