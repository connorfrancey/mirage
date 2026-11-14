#pragma once
#include <cstdint>
#include <string>
#include <vector>

// What the crash handler worked out about one crash: plain data (no game or Windows types), so the analyzers, the
// report text and the crash window share it, and the unit tests can build one by hand.
//
// It is filled on the crash reporter's thread after the report's essentials are on disk (runtime/crash_reporter.cpp):
// the heap is used, and every part that reads the game's memory is guarded, so a part that faults is left out.
namespace crashhandler
{
	// Where an address is: "Mirage.dll+0x1234", and what is known about it
	struct Place
	{
		std::uint32_t address = 0;
		std::string module;				// "Mirage.dll", "" when it isn't in a module
		std::uint32_t offset = 0;		// From the module's base
		std::string symbol;				// "mirage::crash::WriteReport+0x1A", "Engine.Actor.execTrace (native)+0x40", or ""
		std::string source;				// "crash_reporter.cpp:212" when the module has line information
		std::string mod;				// The mod whose DLL it is ("" if none)

		std::string Short() const;		// "Mirage.dll+0x1234" or "0x00401234"
		std::string Full() const;		// Short, then the symbol, the source and the mod when known
	};

	// A register or a stack slot: its value, and what the value looks like (an object, a string, code, a pointer)
	struct Slot
	{
		std::string label;				// "eax", "esp+0x10"
		std::uint32_t value = 0;
		std::string note;				// "TdPlayerPawn_0 (TdPlayerPawn)", "\"Edge_p\"", "Engine.dll+0x10 ...", ""
	};

	// A frame of the UnrealScript call stack: the function or state running and the object it runs on
	struct ScriptFrame
	{
		std::string function;			// "Function TdGame.TdPawn.Tick"
		std::string object;				// "TdPlayerPawn_0"
		std::uint32_t offset = 0;		// Into the function's bytecode
		std::string changedBy;			// The mod whose patch or injection the bytecode is ("" if the game's own)
	};

	struct DisasmLine
	{
		std::uint32_t address = 0;
		std::string bytes;				// "8B 45 08"
		std::string text;				// "mov eax, [ebp+0x08]", targets followed by "  ; Mirage.dll+0x10 Foo"
		bool current = false;			// The faulting instruction
	};

	// A likely cause, from an analyzer (crashhandler/analyzer/)
	struct Finding
	{
		std::string title;				// "A null pointer was read"
		std::string detail;				// What it means and what to try
	};

	struct Analysis
	{
		std::string title;				// "the game crashed", "self-test (not a real crash)"
		bool fatal = false;
		std::uint32_t code = 0;			// The exception code
		std::string what;				// "access violation"
		Place where;					// The faulting instruction
		bool accessKnown = false;		// An access violation's operation and address
		std::string accessKind;			// "reading", "writing", "executing"
		std::uint32_t accessAddress = 0;
		std::string whose;				// "the game", "Mirage", "the mod hook-native", "a module outside the game"
		std::uint32_t thread = 0;
		std::string threadName;			// "the game thread", "the render thread", "a thread of the mod x", "another thread"

		std::vector<Slot> registers;	// eax..eip, eflags
		std::vector<Place> frames;		// The native call stack, innermost first
		bool framesFromScan = false;	// From code addresses found on the stack (no frame pointers): some may be stale
		std::vector<ScriptFrame> script;	// The UnrealScript call stack, innermost first
		std::vector<Slot> stack;		// Stack memory from esp
		std::vector<DisasmLine> disasm;	// Around the faulting instruction
		std::vector<Finding> findings;
		std::vector<std::string> mods;	// "id version (kind)" of the loaded mods
		std::vector<std::string> log;	// The last lines of the log before the crash

		// Facts the analyzers use
		std::uint64_t largestFreeBlock = 0;	// Address space: the largest free block, bytes (0 = unknown)
		bool inAllocator = false;		// The fault is in the engine's allocator (GMalloc's Malloc / Realloc / Free)
	};

	// The sections the report gets after its essentials: findings, the call stacks with names, annotated registers
	// and stack, the disassembly, the log's last lines
	std::string ToText(const Analysis& a);
}
