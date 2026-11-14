// The crash handler's parts that need no game (loader/src/crashhandler/): text recognized in memory, the disassembly
// around a crash, the analyzers' findings and the report's text.
#include "crashhandler/analysis.h"
#include "crashhandler/analyzer/analyzer.h"
#include "crashhandler/memory_text.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

using namespace crashhandler;

namespace
{
	bool HasFinding(const std::vector<Finding>& findings, const std::string& part)
	{
		return std::any_of(findings.begin(), findings.end(), [&](const Finding& f) { return f.title.find(part) != std::string::npos; });
	}

	std::optional<Finding> AlwaysSays(const Analysis&) { return Finding{"Registered analyzer", "it ran"}; }
}

TEST(CrashHandler, TextAtRecognizesNarrowAndWideStrings)
{
	const char narrow[] = "Edge_p\0junk";
	EXPECT_EQ(TextAt(reinterpret_cast<const std::uint8_t*>(narrow), sizeof narrow), "\"Edge_p\"");

	const wchar_t wide[] = L"TdPlayerPawn";
	EXPECT_EQ(TextAt(reinterpret_cast<const std::uint8_t*>(wide), sizeof wide), "L\"TdPlayerPawn\"");

	const std::uint8_t pointer[] = {0x10, 0x20, 0x8F, 0x01, 0, 0, 0, 0};
	EXPECT_EQ(TextAt(pointer, sizeof pointer), "");

	const char shortText[] = "abc";		// Under 4 characters: not taken for a string
	EXPECT_EQ(TextAt(reinterpret_cast<const std::uint8_t*>(shortText), sizeof shortText), "");

	const std::string longText(200, 'x');
	const std::string cut = TextAt(reinterpret_cast<const std::uint8_t*>(longText.c_str()), longText.size() + 1, 10);
	EXPECT_EQ(cut, "\"xxxxxxxxxx...\"");
}

TEST(CrashHandler, DisassemblyLandsOnTheFaultingInstruction)
{
	// push ebp; mov ebp, esp; mov eax, [ebp+8]; mov dword ptr [eax], 0 (the fault); pop ebp; ret
	const std::uint8_t code[] = {0x55, 0x8B, 0xEC, 0x8B, 0x45, 0x08, 0xC7, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5D, 0xC3};
	const auto lines = Disassemble(code, sizeof code, 0x401000, 6, 8, 2);
	ASSERT_FALSE(lines.empty());
	const auto current = std::find_if(lines.begin(), lines.end(), [](const DisasmLine& l) { return l.current; });
	ASSERT_NE(current, lines.end());
	EXPECT_EQ(current->address, 0x401006u);
	EXPECT_NE(current->text.find("mov"), std::string::npos);
	EXPECT_EQ(current->bytes, "C7 00 00 00 00 00");
	EXPECT_EQ(lines.front().address, 0x401000u);		// Decoded from the start: every line before it is whole
	EXPECT_EQ(std::count_if(lines.begin(), lines.end(), [](const DisasmLine& l) { return l.current; }), 1);
}

TEST(CrashHandler, DisassemblyNamesCallTargets)
{
	// call +0x10 (to 0x401015)
	const std::uint8_t code[] = {0xE8, 0x10, 0x00, 0x00, 0x00, 0x90};
	const auto lines = Disassemble(code, sizeof code, 0x401000, 0, 0, 1, [](std::uint32_t target) {
		return target == 0x401015 ? std::string("Foo") : std::string();
	});
	ASSERT_FALSE(lines.empty());
	EXPECT_NE(lines[0].text.find("; Foo"), std::string::npos) << lines[0].text;
}

TEST(CrashHandler, AnalyzersNameTheLikelyCause)
{
	Analysis a;
	a.code = 0xC0000005;
	a.accessKnown = true;
	a.accessKind = "reading";
	a.accessAddress = 0x9550;
	a.where.module = "MirrorsEdge.exe";
	EXPECT_TRUE(HasFinding(RunAnalyzers(a), "null pointer"));

	a.accessAddress = 0x12345678;
	a.inAllocator = true;
	auto findings = RunAnalyzers(a);
	EXPECT_FALSE(HasFinding(findings, "null pointer"));
	EXPECT_TRUE(HasFinding(findings, "allocator"));

	Analysis m;
	m.code = 0xC0000005;
	m.where.module = "hook-native.dll";
	m.where.mod = "hook-native";
	EXPECT_TRUE(HasFinding(RunAnalyzers(m), "in the mod hook-native"));

	Analysis s;
	s.code = 0xC0000005;
	s.script.push_back({"Function TdGame.TdMove_Jump.StartMove", "TdMove_Jump_0", 0x10, "higher-jumps"});
	EXPECT_TRUE(HasFinding(RunAnalyzers(s), "changed by higher-jumps"));

	Analysis o;
	o.code = 0xC0000017;
	o.largestFreeBlock = 4 * 1024 * 1024;
	EXPECT_TRUE(HasFinding(RunAnalyzers(o), "address space"));
	o.largestFreeBlock = 512ull * 1024 * 1024;
	EXPECT_FALSE(HasFinding(RunAnalyzers(o), "address space"));
}

TEST(CrashHandler, RegisteredAnalyzersRunAfterTheBuiltInOnes)
{
	RegisterAnalyzer(&AlwaysSays);
	Analysis a;
	a.code = 0x80000003;	// A breakpoint
	const auto findings = RunAnalyzers(a);
	ASSERT_GE(findings.size(), 2u);
	EXPECT_EQ(findings.front().title, "A breakpoint");
	EXPECT_EQ(findings.back().title, "Registered analyzer");
}

TEST(CrashHandler, ReportTextHasEverySection)
{
	Analysis a;
	a.threadName = "the game thread";
	a.thread = 42;
	a.findings.push_back({"A null pointer was read", "details"});
	Place p;
	p.address = 0x10001234;
	p.module = "Mirage.dll";
	p.offset = 0x1234;
	p.symbol = "mirage::Foo+0x10";
	p.source = "foo.cpp:12";
	a.frames.push_back(p);
	a.script.push_back({"Function Engine.Actor.Tick", "TdPlayerPawn_0 (TdPlayerPawn)", 0x1A, ""});
	a.registers.push_back({"eax", 0x1234, "\"Edge_p\""});
	a.disasm.push_back({0x401006, "C7 00", "mov dword ptr [eax], 0", true});
	a.stack.push_back({"esp+0x000", 5, "TdPlayerPawn_0 (TdPlayerPawn)"});
	a.log.push_back("12:00:00 hello");
	const std::string t = ToText(a);
	for (const char* part : {"Possible causes:", "A null pointer was read", "Thread: the game thread (42)", "Call stack:",
							 "Mirage.dll+0x1234  mirage::Foo+0x10  (foo.cpp:12)", "UnrealScript call stack:",
							 "Function Engine.Actor.Tick +0x1A  on TdPlayerPawn_0", "eax=00001234  \"Edge_p\"", "=> 00401006",
							 "Stack memory:", "The log before the crash:", "12:00:00 hello"})
		EXPECT_NE(t.find(part), std::string::npos) << part << "\n" << t;
	EXPECT_EQ(p.Short(), "Mirage.dll+0x1234");
	Place none;
	none.address = 0x1234;
	EXPECT_EQ(none.Short(), "0x00001234");
}
