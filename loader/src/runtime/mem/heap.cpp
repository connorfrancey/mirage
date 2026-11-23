#include "runtime/mem/heap.h"

#include "runtime/mem/sampler.h"

#include <Windows.h>

#include <atomic>
#include <bit>
#include <cstring>

namespace mirage::mem
{
	namespace
	{
		// Constant-initialized: operator new may run before any dynamic initializer of this DLL
		std::atomic<const char*> g_tagNames[kMaxTags] = {};
		std::atomic<int> g_tagCount{1};
		std::atomic<bool> g_tagLock{false};
		std::atomic<std::uint64_t> g_tagAllocs[kMaxTags] = {};
		std::atomic<std::uint64_t> g_tagBytes[kMaxTags] = {};
		thread_local TagId t_tag = 0;

		std::atomic<std::int64_t> g_live{0}, g_peak{0};
		std::atomic<std::uint64_t> g_allocs{0}, g_frees{0}, g_bytesIn{0}, g_bytesOut{0};
		std::atomic<std::uint64_t> g_failures{0};
		std::atomic<std::size_t> g_lastFailedBytes{0};
		std::atomic<std::uint64_t> g_sizeClass[kSizeClasses] = {};

		struct TimingCounters
		{
			std::atomic<std::uint64_t> samples{0}, sumNs{0}, maxNs{0};
			std::atomic<std::uint64_t> buckets[12] = {};
			void Add(double ns)
			{
				const auto n = static_cast<std::uint64_t>(ns < 0.0 ? 0.0 : ns);
				samples.fetch_add(1, std::memory_order_relaxed);
				sumNs.fetch_add(n, std::memory_order_relaxed);
				std::uint64_t m = maxNs.load(std::memory_order_relaxed);
				while (n > m && !maxNs.compare_exchange_weak(m, n, std::memory_order_relaxed)) {}
				int b = 0;
				for (std::uint64_t limit = 64; b < 11 && n >= limit; limit <<= 1) ++b;
				buckets[b].fetch_add(1, std::memory_order_relaxed);
			}
			Timing Read() const
			{
				Timing t;
				t.samples = samples.load(std::memory_order_relaxed);
				t.averageNs = t.samples ? static_cast<double>(sumNs.load(std::memory_order_relaxed)) / static_cast<double>(t.samples) : 0.0;
				t.maxNs = static_cast<double>(maxNs.load(std::memory_order_relaxed));
				for (int i = 0; i < 12; ++i) t.buckets[i] = buckets[i].load(std::memory_order_relaxed);
				return t;
			}
		};
		TimingCounters g_allocTiming, g_freeTiming;
		thread_local std::uint32_t t_timing = 0;

		// Large allocations: a small ring behind an SRW lock (no allocation, rare)
		constexpr int kRing = 32;
		SRWLOCK g_ringLock = SRWLOCK_INIT;
		LargeAllocation g_ring[kRing];
		int g_ringHead = 0, g_ringCount = 0;
		std::atomic<std::size_t> g_largeThreshold{8u << 20};

		std::atomic<std::int64_t> g_qpcFreq{0};
	}

	// ---- Tags --------------------------------------------------------------------------------------------------------------
	TagId RegisterTag(const char* name)
	{
		if (!name || !*name) return 0;
		while (g_tagLock.exchange(true, std::memory_order_acquire)) YieldProcessor();
		TagId id = 0;
		const int count = g_tagCount.load(std::memory_order_relaxed);
		for (int i = 1; i < count; ++i) {
			const char* n = g_tagNames[i].load(std::memory_order_relaxed);
			if (n && std::strcmp(n, name) == 0) { id = static_cast<TagId>(i); break; }
		}
		if (!id && count < kMaxTags) {
			g_tagNames[count].store(name, std::memory_order_relaxed);
			g_tagCount.store(count + 1, std::memory_order_release);
			id = static_cast<TagId>(count);
		}
		g_tagLock.store(false, std::memory_order_release);
		return id;
	}

	const char* TagName(TagId id)
	{
		if (id == 0 || id >= g_tagCount.load(std::memory_order_acquire)) return "untagged";
		const char* n = g_tagNames[id].load(std::memory_order_relaxed);
		return n ? n : "?";
	}

	int TagCount() { return g_tagCount.load(std::memory_order_acquire); }
	TagId CurrentTag() { return t_tag; }

	TagScope::TagScope(TagId tag) : previous_(t_tag) { t_tag = tag; }
	TagScope::~TagScope() { t_tag = previous_; }

	std::vector<TagCounters> ReadTags()
	{
		std::vector<TagCounters> out;
		const int count = TagCount();
		for (int i = 0; i < count; ++i)
			out.push_back({TagName(static_cast<TagId>(i)), g_tagAllocs[i].load(std::memory_order_relaxed), g_tagBytes[i].load(std::memory_order_relaxed)});
		return out;
	}

	// ---- Counters ----------------------------------------------------------------------------------------------------------
	int SizeClassOf(std::size_t bytes)
	{
		if (bytes <= 16) return 0;
		const int c = static_cast<int>(std::bit_width(bytes - 1)) - 4;
		return c < kSizeClasses - 1 ? c : kSizeClasses - 1;
	}

	std::size_t SizeClassLimit(int c)
	{
		if (c >= kSizeClasses - 1) return SIZE_MAX;
		return std::size_t{16} << c;
	}

	HeapCounters ReadHeap()
	{
		HeapCounters h;
		h.failures = g_failures.load(std::memory_order_acquire);
		h.lastFailedBytes = g_lastFailedBytes.load(std::memory_order_relaxed);
		h.live = g_live.load(std::memory_order_relaxed);
		h.peakLive = g_peak.load(std::memory_order_relaxed);
		h.allocations = g_allocs.load(std::memory_order_relaxed);
		h.frees = g_frees.load(std::memory_order_relaxed);
		h.bytesAllocated = g_bytesIn.load(std::memory_order_relaxed);
		h.bytesFreed = g_bytesOut.load(std::memory_order_relaxed);
		for (int i = 0; i < kSizeClasses; ++i) h.sizeClass[i] = g_sizeClass[i].load(std::memory_order_relaxed);
		return h;
	}

	Timing ReadTiming(bool allocations) { return (allocations ? g_allocTiming : g_freeTiming).Read(); }

	// ---- Large allocations -------------------------------------------------------------------------------------------------
	void SetLargeThreshold(std::size_t bytes) { g_largeThreshold.store(bytes, std::memory_order_relaxed); }

	int DrainLarge(LargeAllocation* out, int capacity)
	{
		AcquireSRWLockExclusive(&g_ringLock);
		int n = 0;
		while (g_ringCount > 0 && n < capacity) {
			const int idx = (g_ringHead - g_ringCount + kRing) % kRing;
			out[n++] = g_ring[idx];
			--g_ringCount;
		}
		ReleaseSRWLockExclusive(&g_ringLock);
		return n;
	}

	// ---- Hooks -------------------------------------------------------------------------------------------------------------
	double NowNs()
	{
		std::int64_t f = g_qpcFreq.load(std::memory_order_relaxed);
		if (!f) {
			LARGE_INTEGER li;
			QueryPerformanceFrequency(&li);
			f = li.QuadPart ? li.QuadPart : 1;
			g_qpcFreq.store(f, std::memory_order_relaxed);
		}
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return static_cast<double>(now.QuadPart) * 1e9 / static_cast<double>(f);
	}

	std::uint64_t RealTimeMs()
	{
		// KSYSTEM_TIME at 0x7FFE0008 (all Windows versions): LowPart, High1Time, High2Time; read until the two highs agree
		struct KSystemTime { volatile ULONG low; volatile LONG high1; volatile LONG high2; };
		const auto* t = reinterpret_cast<const KSystemTime*>(0x7FFE0008);
		LONG high;
		ULONG low;
		do {
			high = t->high1;
			low = t->low;
		} while (high != t->high2);
		const std::uint64_t ticks = (static_cast<std::uint64_t>(static_cast<ULONG>(high)) << 32) | low;	// 100 ns
		return ticks / 10000;
	}

	bool TimeThisCall() { return (++t_timing & (kTimingEvery - 1)) == 0; }

	void OnAllocationFailure(std::size_t bytes) noexcept
	{
		g_lastFailedBytes.store(bytes, std::memory_order_relaxed);
		g_failures.fetch_add(1, std::memory_order_release);
	}

	void OnAllocated(void* p, std::size_t bytes, double ns) noexcept
	{
		const auto size = static_cast<std::int64_t>(bytes);
		const std::int64_t live = g_live.fetch_add(size, std::memory_order_relaxed) + size;
		std::int64_t peak = g_peak.load(std::memory_order_relaxed);
		while (live > peak && !g_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
		g_allocs.fetch_add(1, std::memory_order_relaxed);
		g_bytesIn.fetch_add(bytes, std::memory_order_relaxed);
		g_sizeClass[SizeClassOf(bytes)].fetch_add(1, std::memory_order_relaxed);
		const TagId tag = t_tag;
		if (tag) {
			g_tagAllocs[tag].fetch_add(1, std::memory_order_relaxed);
			g_tagBytes[tag].fetch_add(bytes, std::memory_order_relaxed);
		}
		if (ns >= 0.0) g_allocTiming.Add(ns);

		const std::size_t large = g_largeThreshold.load(std::memory_order_relaxed);
		if (large && bytes >= large) {
			LargeAllocation a;
			a.bytes = bytes;
			a.tag = tag;
			a.timeMs = RealTimeMs();
			a.depth = RtlCaptureStackBackTrace(2, 10, reinterpret_cast<void**>(a.frames), nullptr);
			AcquireSRWLockExclusive(&g_ringLock);
			g_ring[g_ringHead] = a;
			g_ringHead = (g_ringHead + 1) % kRing;
			if (g_ringCount < kRing) ++g_ringCount;
			ReleaseSRWLockExclusive(&g_ringLock);
		}
		sampler::OnAlloc(p, bytes, tag);
	}

	void OnFreeing(void* p, std::size_t bytes, double ns) noexcept
	{
		if (p) {		// nullptr: only the timing of a free already counted
			g_live.fetch_sub(static_cast<std::int64_t>(bytes), std::memory_order_relaxed);
			g_frees.fetch_add(1, std::memory_order_relaxed);
			g_bytesOut.fetch_add(bytes, std::memory_order_relaxed);
			sampler::OnFree(p);
		}
		if (ns >= 0.0) g_freeTiming.Add(ns);
	}

	std::int64_t LiveHeapBytes() { return g_live.load(std::memory_order_relaxed); }
	std::uint64_t HeapAllocationCount() { return g_allocs.load(std::memory_order_relaxed); }
}
