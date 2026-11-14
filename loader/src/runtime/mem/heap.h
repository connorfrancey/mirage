#pragma once
// The menu's heap as operator new / delete see it (runtime/memory.cpp calls OnAllocated / OnFreeing): counters by size
// class and by tag, sampled timing of the allocator, and large allocations caught as they happen. Everything here is
// lock-free, never allocates, and costs a few relaxed atomic adds per allocation. The game's own allocations never
// pass through it (this DLL's operator new is only the menu's code and its libraries).
//
// Tags say what part of the menu memory is for: a TagScope on the stack tags every allocation the thread makes under it.
//
//   static const auto kTag = mirage::mem::RegisterTag("UnrealLib");
//   mirage::mem::TagScope tag(kTag);
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mirage::mem
{
	using TagId = std::uint8_t;
	constexpr int kMaxTags = 32;			// Tag 0 is "untagged"; a registration past the last gets 0
	constexpr int kSizeClasses = 24;		// Powers of two: <= 16 B, <= 32 B, ..., the last one everything larger

	// `name` must outlive the menu (a string literal). The same name gets the same id. Any thread.
	TagId RegisterTag(const char* name);
	const char* TagName(TagId id);
	int TagCount();
	TagId CurrentTag();

	class TagScope
	{
	public:
		explicit TagScope(TagId tag);
		~TagScope();
		TagScope(const TagScope&) = delete;
		TagScope& operator=(const TagScope&) = delete;
	private:
		TagId previous_;
	};

	struct HeapCounters
	{
		std::int64_t live = 0;
		std::uint64_t allocations = 0, frees = 0;
		std::uint64_t bytesAllocated = 0, bytesFreed = 0;
		std::uint64_t sizeClass[kSizeClasses] = {};		// Allocations by size class, so far
		std::int64_t peakLive = 0;
	};
	HeapCounters ReadHeap();
	std::int64_t LiveHeapBytes();			// Just the live bytes (cheap)
	std::uint64_t HeapAllocationCount();
	int SizeClassOf(std::size_t bytes);
	std::size_t SizeClassLimit(int sizeClass);			// Largest size in the class (the last: SIZE_MAX)

	struct TagCounters
	{
		const char* name = nullptr;
		std::uint64_t allocations = 0, bytes = 0;		// So far (what is live by tag comes from the sampler)
	};
	std::vector<TagCounters> ReadTags();

	// The allocator's own speed, timed on one call in kTimingEvery per thread (the CRT heap is shared with system code:
	// a slow malloc means contention or a fragmented heap)
	constexpr std::uint32_t kTimingEvery = 1024;
	struct Timing
	{
		std::uint64_t samples = 0;
		double averageNs = 0.0, maxNs = 0.0;
		std::uint64_t buckets[12] = {};		// < 64 ns, < 128, ..., >= 64 us
	};
	Timing ReadTiming(bool allocations);	// true: malloc, false: free

	// Allocations at or over the threshold, kept until drained (a small ring; the oldest are dropped when it is full)
	struct LargeAllocation
	{
		std::size_t bytes = 0;
		TagId tag = 0;
		std::uint64_t timeMs = 0;
		std::uintptr_t frames[10] = {};
		int depth = 0;
	};
	void SetLargeThreshold(std::size_t bytes);		// 0 = off
	int DrainLarge(LargeAllocation* out, int capacity);

	// ---- operator new / delete (runtime/memory.cpp) -----------------------------------------------------------------------
	bool TimeThisCall();							// This thread's turn to time a call
	void OnAllocated(void* p, std::size_t bytes, double ns) noexcept;	// ns < 0: not timed
	void OnFreeing(void* p, std::size_t bytes, double ns) noexcept;
	double NowNs();									// For timing (QueryPerformanceCounter)

	// Real time in milliseconds since boot, read from the kernel's shared page (KUSER_SHARED_DATA interrupt time). Unlike
	// QueryPerformanceCounter and GetTickCount(64), the TAS can't make it virtual: on the game thread during a TAS
	// session those run on the replay's clock. For anything that compares times across threads.
	std::uint64_t RealTimeMs();
}
