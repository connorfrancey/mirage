// The engine's symbol table (loader/src/runtime/symbols.h): the built-in symbols.toml reads, and every entry is one
// MirageHook's scanner takes (a pattern it parses, not starting with a wildcard; resolve steps the runtime knows).
// Finding them needs the game: the in-game self-test "Mirage engine symbols" does that.
#include "runtime/symbols.h"

#include <MirageHook/utilities/Address.hpp>   // Before Signature.hpp, which uses it without including it
#include <MirageHook/scanner/Signature.hpp>
#include <gtest/gtest.h>

#include <set>
#include <sstream>

TEST(Symbols, TheBuiltInTableIsValid)
{
	std::string error;
	const auto entries = mirage::symbols::Parse(mirage::symbols::BuiltIn(), &error);
	ASSERT_TRUE(error.empty()) << error;
	ASSERT_GE(entries.size(), 10u);
	std::set<std::string> names;
	for (const auto& e : entries) {
		EXPECT_TRUE(names.insert(e.name).second) << e.name << " twice";
		EXPECT_TRUE(e.kind == "code" || e.kind == "data") << e.name << ": kind " << e.kind;
		EXPECT_FALSE(e.description.empty()) << e.name << " has no description";
		auto sig = MirageHook::Signature::parse(e.name, e.pattern);
		EXPECT_TRUE(sig.has_value()) << e.name << ": " << (sig ? std::string() : sig.error().message());
		for (const std::string& step : e.resolve) {
			std::istringstream in(step);
			std::string word;
			in >> word;
			EXPECT_TRUE(word == "offset" || word == "deref" || word == "rel32") << e.name << ": step " << step;
		}
	}
	for (const char* needed : {"GNames", "GObjects", "LevelLoad", "Tick", "ProcessEvent", "StaticLoadObject"})
		EXPECT_TRUE(names.count(needed)) << needed << " is missing";
}

TEST(Symbols, ProvidedNamesAreFound)
{
	mirage::symbols::Provide("TestSymbol", 0x401000, "code", "a test");
	EXPECT_EQ(mirage::symbols::Find("TestSymbol"), 0x401000u);
	EXPECT_EQ(mirage::symbols::NameOf(0x401000), "TestSymbol");
	EXPECT_EQ(mirage::symbols::Find("NoSuchSymbol"), 0u);
	std::string error;
	EXPECT_TRUE(mirage::symbols::Parse("[Broken\nkind = ", &error).empty());
	EXPECT_FALSE(error.empty());
}
