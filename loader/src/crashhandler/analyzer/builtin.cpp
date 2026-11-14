// The built-in analyzers (crashhandler/analyzer/analyzer.h): the causes that a crash's own facts point to.
#include "crashhandler/analyzer/analyzer.h"

#include <cstdio>
#include <string>

namespace crashhandler
{
	namespace
	{
		constexpr std::uint32_t kAccessViolation = 0xC0000005, kStackOverflow = 0xC00000FD, kHeapCorruption = 0xC0000374,
								kStackBufferOverrun = 0xC0000409, kCppException = 0xE06D7363, kBreakpoint = 0x80000003;

		std::string Hex(std::uint32_t v)
		{
			char b[16];
			std::snprintf(b, sizeof b, "0x%X", v);
			return b;
		}

		// A field of a null object: the address read or written is small
		std::optional<Finding> NullPointer(const Analysis& a)
		{
			if (a.code != kAccessViolation || !a.accessKnown || a.accessAddress >= 0x10000) return std::nullopt;
			if (a.accessKind == "executing")
				return Finding{"A call through a null function pointer",
							   "the code jumped to " + Hex(a.accessAddress) + ": a function pointer or a vtable entry was empty"};
			return Finding{"A null pointer was " + std::string(a.accessKind == "writing" ? "written through" : "read"),
						   "the address " + Hex(a.accessAddress) + " is a field " + Hex(a.accessAddress) +
							   " bytes into an object that doesn't exist (a pointer that was null: an actor destroyed, a "
							   "lookup that found nothing)"};
		}

		std::optional<Finding> ExecutedData(const Analysis& a)
		{
			if (a.code != kAccessViolation || !a.accessKnown || a.accessKind != "executing" || a.accessAddress < 0x10000)
				return std::nullopt;
			return Finding{"Code ran where there is no code",
						   "the instruction pointer went to " + Hex(a.accessAddress) +
							   ": a corrupted return address or function pointer, or a call into a module that was unloaded"};
		}

		std::optional<Finding> StackOverflow(const Analysis& a)
		{
			if (a.code != kStackOverflow) return std::nullopt;
			return Finding{"Stack overflow", "a function called itself without end, or something kept a very large "
											 "local; the call stack shows the repeating functions"};
		}

		std::optional<Finding> HeapDamage(const Analysis& a)
		{
			if (a.code == kHeapCorruption)
				return Finding{"Heap corruption", "Windows found the heap damaged: memory written past its end or freed twice, "
												  "earlier than this crash (the code that did it is usually not on this stack)"};
			if (a.code == kStackBufferOverrun)
				return Finding{"A buffer overrun was caught", "code wrote past the end of a buffer on the stack (/GS), or "
															  "called fail-fast"};
			if (a.inAllocator)
				return Finding{"The crash is in the engine's allocator",
							   "the engine freed or resized memory it doesn't own, or memory that was freed already: often "
							   "something wrote past an array, or a mod replaced engine memory (bytecode, an array) with its "
							   "own and the engine later freed it"};
			return std::nullopt;
		}

		std::optional<Finding> UncaughtCpp(const Analysis& a)
		{
			if (a.code != kCppException) return std::nullopt;
			return Finding{"A C++ exception nobody caught", "a native mod or a library threw and nothing caught it; the "
															"call stack shows where it was thrown"};
		}

		std::optional<Finding> Breakpoint(const Analysis& a)
		{
			if (a.code != kBreakpoint) return std::nullopt;
			return Finding{"A breakpoint", "an int3 instruction ran (a debug check, or __debugbreak); with no debugger "
										   "attached the game stops here. Continuing skips it"};
		}

		std::optional<Finding> ModCode(const Analysis& a)
		{
			if (!a.where.mod.empty())
				return Finding{"The crash is in the mod " + a.where.mod,
							   "its own code faulted; turn it off in the mod manager (or start without it) and tell its author"};
			for (const Place& f : a.frames)
				if (!f.mod.empty())
					return Finding{"The mod " + f.mod + " is on the call stack",
								   "the game's code faulted while a call from it was running: it may have passed something "
								   "the game didn't expect"};
			return std::nullopt;
		}

		std::optional<Finding> ChangedBytecode(const Analysis& a)
		{
			for (const ScriptFrame& f : a.script)
				if (!f.changedBy.empty())
					return Finding{"UnrealScript changed by " + f.changedBy + " was running",
								   f.function + " runs bytecode that " + f.changedBy +
									   " patched or injected into; the change may not fit what the function expects"};
			return std::nullopt;
		}

		std::optional<Finding> AddressSpace(const Analysis& a)
		{
			if (a.largestFreeBlock == 0 || a.largestFreeBlock >= 32ull * 1024 * 1024) return std::nullopt;
			char mb[32];
			std::snprintf(mb, sizeof mb, "%.1f MB", static_cast<double>(a.largestFreeBlock) / (1024.0 * 1024.0));
			return Finding{"The game is out of address space",
						   std::string("the largest free block is ") + mb +
							   ": the 32-bit game can't find room for big allocations (textures, levels). Fewer or smaller "
							   "mods, or a Large Address Aware game, give it more room"};
		}

		std::optional<Finding> MirageItself(const Analysis& a)
		{
			if (a.where.module != "Mirage.dll") return std::nullopt;
			return Finding{"The crash is in Mirage itself",
						   "a bug in the mod loader; please report it with this report and the minidump next to it"};
		}

		// The built-in analyzers first, whichever file registers first (static initialization order is per file)
		std::vector<Analyzer>& Registry()
		{
			static std::vector<Analyzer> analyzers{&NullPointer, &ExecutedData, &StackOverflow, &HeapDamage, &UncaughtCpp,
												   &Breakpoint, &ModCode, &ChangedBytecode, &AddressSpace, &MirageItself};
			return analyzers;
		}
	}

	bool RegisterAnalyzer(Analyzer analyzer)
	{
		if (analyzer) Registry().push_back(analyzer);
		return true;
	}

	std::vector<Finding> RunAnalyzers(const Analysis& a)
	{
		std::vector<Finding> out;
		for (Analyzer f : Registry())
			if (auto finding = f(a)) out.push_back(std::move(*finding));
		return out;
	}
}
