#pragma once
// The sampling heap profiler: what is live in the menu's heap, by tag and by call stack, and how long blocks live.
// It is also the leak detector.
//
//   Sampled  (default) one allocation in about every `interval` bytes allocated is recorded with its call stack, tag,
//            size and time; each stands for `interval` bytes, so the totals are unbiased estimates. Cost: one
//            thread-local subtraction per allocation, one atomic load per free (a filter in front of the table).
//   Full     every allocation is recorded (exact, slow: a stack capture each). For a leak check of one operation.
//   Off      nothing new is recorded (what is recorded stays until freed).
//
// Leak checks:  const auto gen = Generation();  ...work...  Report(gen) lists what the work left behind, by stack.
// Leak suspects: Report() groups the live blocks by stack, with the age of the oldest; the manager watches each
// stack's live bytes over time (a stack that only grows is a leak suspect).
//
// The table is fixed size and lives outside the heap (VirtualAlloc, about 3 MB of address space, reserved on first
// use): when it is full new samples are dropped and counted, never a failed allocation. Nothing here allocates while
// its lock is held, and allocations the sampler itself makes are never sampled.
#include "runtime/mem/heap.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mirage::mem::sampler
{
	enum class Mode { Off, Sampled, Full };

	void SetMode(Mode mode, std::size_t intervalBytes = 256 * 1024);
	Mode CurrentMode();
	std::size_t Interval();

	// operator new / delete (runtime/memory.cpp): cheap unless this allocation is sampled
	void OnAlloc(void* p, std::size_t bytes, TagId tag) noexcept;
	void OnFree(void* p) noexcept;

	std::uint32_t Generation();			// Samples taken so far: a mark for "since"
	void Clear();						// Forgets every record (they are then neither reported nor matched on free)

	struct Site
	{
		std::uint32_t stack = 0;
		std::vector<std::uintptr_t> frames;
		TagId tag = 0;					// Of its first live sample
		std::uint64_t liveBytes = 0;	// Estimated
		std::uint64_t liveBlocks = 0;	// Estimated
		std::uint32_t samples = 0;		// Live samples
		std::uint64_t oldestAgeMs = 0;
	};

	struct Report
	{
		Mode mode = Mode::Off;
		std::size_t interval = 0;
		std::uint64_t samples = 0, dropped = 0;			// Taken so far, dropped (table full)
		std::size_t tableUsed = 0, tableCapacity = 0;
		std::size_t stacksUsed = 0, stacksCapacity = 0;
		std::uint64_t liveBytes = 0, liveBlocks = 0;	// Estimated, of the samples taken since `since`
		std::uint64_t liveByTag[kMaxTags] = {};
		std::uint64_t longLivedBytes = 0;				// Live for over a minute (estimated)
		// Bytes freed by how long they lived (estimated): < 16 ms (within a frame), < 1 s, < 1 min, < 10 min, longer
		std::uint64_t lifetime[5] = {};
		std::vector<Site> sites;						// Most live bytes first
	};
	// What is live of the samples taken since generation `since` (0: all), the `top` biggest stacks; `thread`: only
	// what that thread allocated (0: every thread)
	Report Snapshot(std::uint32_t since = 0, std::size_t top = 20, std::uint32_t thread = 0);

	// "Mirage.dll+0x1234" for each frame (module + offset, like the crash log; no symbol files needed)
	std::string Describe(std::uintptr_t address);
}
