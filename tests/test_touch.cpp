// Touch sets and their overlaps (runtime/touch): two mods written to a temporary folder, read without running them.
#include "runtime/manifest.h"
#include "runtime/touch.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace mirage;

namespace
{
	void Write(const fs::path& p, const std::string& text)
	{
		fs::create_directories(p.parent_path());
		std::ofstream(p, std::ios::binary) << text;
	}

	struct Folder
	{
		fs::path root = fs::temp_directory_path() / ("mirage_touch_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
		Folder() { fs::remove_all(root); }
		~Folder() { fs::remove_all(root); }
	};

	mods::Manifest Mod(const fs::path& dir, const std::string& id, const std::string& extra = "")
	{
		fs::create_directories(dir / "content");
		Write(dir / "mod.toml", "id = \"" + id + "\"\nversion = \"1.0.0\"\ncontent = [\"content\"]\n" + extra);
		const mods::Parsed p = mods::Load(dir);
		EXPECT_TRUE(p.manifest.has_value()) << (p.errors.empty() ? "" : p.errors.front());
		return p.manifest ? *p.manifest : mods::Manifest{};
	}

	bool Has(const touch::Set& s, touch::Kind k, const std::string& what)
	{
		for (const auto& i : s.items)
			if (i.kind == k && i.what == what) return true;
		return false;
	}
}

TEST(Touch, ReadsWhatAModTouches)
{
	Folder f;
	const fs::path a = f.root / "aaa";
	Write(a / "content" / "defaults.toml",
		  "[\"TdGame.Default__TdPawn\"]\nMaxStepHeight = 60.0\ninstances = true\n[\"TdPawn TdGame.Default__TdPlayerPawn\"]\n"
		  "[\"TdGame.Default__TdPlayerPawn\".Offset]\nX = 1.0\n");
	Write(a / "content" / "textures" / "Pkg.Group.T_Thing.png", "x");
	Write(a / "content" / "textures" / "readme.txt", "not a texture");
	Write(a / "kismet" / "Edge_p.toml", "[[set]]\nop = \"SeqEvent_Touch_3\"\n[set.properties]\nMaxTriggerCount = 0\n"
										 "[[unlink]]\nfrom = \"SeqEvent_Touch_3\"\nto = \"SeqAct_Toggle_2\"\n");
	Write(a / "main.lua", "local FUNCTION = \"TdGame.TdMove_Jump.StartMove\"\nlocal s = mirage.script.open(FUNCTION)\n"
						  "mirage.hook(\"Engine.Actor.Touch\", function() end)\nmirage.inject(name_from_somewhere, \"head\")\n");
	const mods::Manifest m = Mod(a, "aaa", "[actions.dash]\nkey = \"Q\"\n");
	const touch::Set s = touch::Read(m);
	using K = touch::Kind;
	EXPECT_TRUE(Has(s, K::Property, "TdGame.Default__TdPawn.MaxStepHeight"));
	EXPECT_FALSE(Has(s, K::Property, "TdGame.Default__TdPawn.instances"));     // Where it applies, not a property
	EXPECT_TRUE(Has(s, K::Property, "TdGame.Default__TdPlayerPawn.Offset.X")); // A struct's member, by its full name
	EXPECT_TRUE(Has(s, K::Texture, "Pkg.Group.T_Thing"));
	EXPECT_TRUE(Has(s, K::KismetSet, "edge_p: SeqEvent_Touch_3.MaxTriggerCount"));
	EXPECT_TRUE(Has(s, K::KismetUnlink, "edge_p: SeqEvent_Touch_3 -> SeqAct_Toggle_2"));
	EXPECT_TRUE(Has(s, K::Patch, "TdGame.TdMove_Jump.StartMove"));            // Through the constant
	EXPECT_TRUE(Has(s, K::Hook, "Engine.Actor.Touch"));
	EXPECT_TRUE(Has(s, K::ActionKey, "q"));
	int injections = 0;
	for (const auto& i : s.items) injections += i.kind == K::Inject;
	EXPECT_EQ(injections, 0);           // A name computed at run time isn't guessed
}

TEST(Touch, OverlapsBetweenMods)
{
	Folder f;
	const fs::path a = f.root / "aaa", b = f.root / "bbb", c = f.root / "ccc";
	Write(a / "content" / "defaults.toml", "[\"TdGame.Default__TdPawn\"]\nMaxStepHeight = 60.0\n");
	Write(b / "content" / "defaults.toml", "[\"TdGame.Default__TdPawn\"]\nmaxstepheight = 90.0\nGroundSpeed = 1.0\n");
	Write(a / "content" / "textures" / "Pkg.T_A.png", "x");
	Write(b / "content" / "textures" / "Pkg.T_A.dds", "x");
	Write(a / "main.lua", "mirage.script.open(\"TdGame.TdMove_Jump.StartMove\")\nmirage.hook(\"Engine.Actor.Touch\")\n");
	Write(b / "main.lua", "mirage.inject(\"TdGame.TdMove_Jump.StartMove\", \"head\")\nmirage.hook(\"Engine.Actor.Touch\")\n");
	Write(c / "main.cs", "class C : Mod { void F() { Inject(\"TdGame.TdMove_Jump.StartMove\", \"head\", c => {}); } }\n");
	const std::vector<touch::Set> sets = {touch::Read(Mod(a, "aaa", "[actions.jump]\nkey = \"SpaceBar\"\n")),
										  touch::Read(Mod(b, "bbb", "[actions.leap]\nkey = \"spacebar\"\n")),
										  touch::Read(Mod(c, "ccc"))};
	const auto overlaps = touch::Overlaps(sets);
	int conflicts = 0, notes = 0;
	bool property = false, texture = false, patchInject = false, key = false;
	for (const auto& o : overlaps) {
		(o.conflict ? conflicts : notes)++;
		property |= o.kind == touch::Kind::Property && o.mods == std::vector<std::string>{"aaa", "bbb"} && o.text.find("bbb's value stays") != std::string::npos;
		texture |= o.kind == touch::Kind::Texture;
		patchInject |= o.kind == touch::Kind::Inject && o.mods.size() == 3;
		key |= o.kind == touch::Kind::ActionKey && !o.conflict;
	}
	EXPECT_TRUE(property);      // The same property, names compared without case
	EXPECT_TRUE(texture);       // The same texture, from a PNG and a DDS
	EXPECT_TRUE(patchInject);   // A patch and two injections: a note
	EXPECT_TRUE(key);
	EXPECT_EQ(conflicts, 2);    // Two hooks and two injections alone aren't reported
	EXPECT_EQ(notes, 2);
	ASSERT_FALSE(overlaps.empty());
	EXPECT_TRUE(overlaps.front().conflict);     // Conflicts first
}
