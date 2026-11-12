#include "runtime/mem/sampler.h"

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

namespace mirage::mem::sampler
{
	namespace
	{
		constexpr std::size_t kSlots = 1u << 16;		// Live samples (power of two)
		constexpr std::size_t kMaxLoad = kSlots * 3 / 4;
		constexpr std::size_t kStacks = 4096;			// Distinct call stacks (power of two)
		constexpr int kDepth = 14;
		constexpr std::size_t kFilter = 1u << 14;		// Buckets in front of the table (counts of samples per bucket)

		struct Record
		{
			std::uintptr_t ptr;			// 0: empty slot
			std::uint32_t weight;		// Bytes it stands for (capped)
			std::uint32_t size;
			std::uint32_t seq;			// Generation
			std::uint32_t stack;		// Index + 1 in the stack table, 0 = none
			std::uint32_t thread;		// Thread id that allocated it
			std::uint64_t timeMs;
			TagId tag;
		};

		struct Stack
		{
			std::uint32_t hash;			// 0: empty
			std::uint32_t depth;
			std::uintptr_t frames[kDepth];
		};

		struct Storage
		{
			Record records[kSlots];
			Stack stacks[kStacks];
			std::uint32_t filter[kFilter];
		};

		// Constant-initialized: reachable from operator new before any initializer runs
		std::atomic<Storage*> g_store{nullptr};
		std::atomic<bool> g_storeFailed{false};
		SRWLOCK g_lock = SRWLOCK_INIT;
		std::atomic<int> g_mode{static_cast<int>(Mode::Off)};
		std::atomic<std::size_t> g_interval{256 * 1024};
		std::atomic<std::uint32_t> g_settingsEpoch{0};
		std::atomic<std::uint32_t> g_seq{0};
		std::atomic<std::uint64_t> g_dropped{0};
		std::atomic<std::size_t> g_used{0};			// Live records
		std::size_t g_stacksUsed = 0;				// Under the lock
		std::uint64_t g_lifetime[5] = {};			// Under the lock

		thread_local bool t_busy = false;			// In the sampler: never sample our own work
		thread_local std::int64_t t_until = 0;		// Bytes until this thread's next sample
		thread_local std::uint32_t t_epoch = ~0u;
		thread_local std::uint64_t t_rng = 0;

		Storage* Store()
		{
			Storage* s = g_store.load(std::memory_order_acquire);
			if (s || g_storeFailed.load(std::memory_order_relaxed)) return s;
			void* mem = VirtualAlloc(nullptr, sizeof(Storage), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			if (!mem) {
				g_storeFailed.store(true, std::memory_order_relaxed);
				return nullptr;
			}
			Storage* expected = nullptr;
			if (!g_store.compare_exchange_strong(expected, static_cast<Storage*>(mem))) {
				VirtualFree(mem, 0, MEM_RELEASE);
				return expected;
			}
			return static_cast<Storage*>(mem);
		}

		std::uint32_t Hash(std::uintptr_t p)
		{
			std::uint32_t x = static_cast<std::uint32_t>(p >> 3);
			x ^= x >> 16;
			x *= 0x7feb352dU;
			x ^= x >> 15;
			x *= 0x846ca68bU;
			x ^= x >> 16;
			return x;
		}

		std::uint64_t NextRandom()
		{
			if (!t_rng) t_rng = (static_cast<std::uint64_t>(GetCurrentThreadId()) << 32) ^ GetTickCount64() ^ 0x9E3779B97F4A7C15ull;
			t_rng ^= t_rng << 13;
			t_rng ^= t_rng >> 7;
			t_rng ^= t_rng << 17;
			return t_rng;
		}

		// Bytes until the next sample: exponentially distributed around the interval (no aliasing with allocation
		// patterns of a fixed period)
		std::int64_t NextGap(std::size_t interval)
		{
			const double u = (static_cast<double>(NextRandom() >> 11) + 0.5) / 9007199254740992.0;
			const double gap = -std::log(u) * static_cast<double>(interval);
			return static_cast<std::int64_t>(std::min(gap, 64.0 * static_cast<double>(interval))) + 1;
		}

		std::uint32_t Weight(std::size_t size, std::size_t interval)
		{
			if (interval == 0) return static_cast<std::uint32_t>(std::min<std::size_t>(size, UINT32_MAX));
			// Poisson sampling by bytes: a block of `size` is sampled with p = 1 - e^(-size/interval); it stands for size / p
			const double p = 1.0 - std::exp(-static_cast<double>(size) / static_cast<double>(interval));
			const double w = p > 0.0 ? static_cast<double>(size) / p : static_cast<double>(interval);
			return static_cast<std::uint32_t>(std::min(w, 4.0e9));
		}

		std::uint32_t InternStack(Storage* s, const std::uintptr_t* frames, int depth)
		{
			std::uint32_t h = 2166136261u;
			for (int i = 0; i < depth; ++i) h = (h ^ static_cast<std::uint32_t>(frames[i])) * 16777619u;
			if (!h) h = 1;
			for (std::size_t i = 0, idx = h & (kStacks - 1); i < kStacks; ++i, idx = (idx + 1) & (kStacks - 1)) {
				Stack& st = s->stacks[idx];
				if (st.hash == 0) {
					if (g_stacksUsed >= kStacks * 7 / 8) return 0;		// Nearly full: no new stacks
					st.hash = h;
					st.depth = static_cast<std::uint32_t>(depth);
					std::memcpy(st.frames, frames, sizeof(std::uintptr_t) * depth);
					++g_stacksUsed;
					return static_cast<std::uint32_t>(idx + 1);
				}
				if (st.hash == h && st.depth == static_cast<std::uint32_t>(depth) &&
					std::memcmp(st.frames, frames, sizeof(std::uintptr_t) * depth) == 0)
					return static_cast<std::uint32_t>(idx + 1);
			}
			return 0;
		}

		void Insert(Storage* s, const Record& r)
		{
			for (std::size_t i = 0, idx = Hash(r.ptr) & (kSlots - 1); i < kSlots; ++i, idx = (idx + 1) & (kSlots - 1)) {
				Record& slot = s->records[idx];
				if (slot.ptr == r.ptr) {			// Freed without us seeing it (an earlier mode change): replace
					slot = r;
					return;
				}
				if (slot.ptr == 0) {
					slot = r;
					g_used.fetch_add(1, std::memory_order_relaxed);
					std::atomic_ref<std::uint32_t>(s->filter[Hash(r.ptr) & (kFilter - 1)]).fetch_add(1, std::memory_order_release);
					return;
				}
			}
		}

		// Linear probing with backward-shift deletion: no tombstones, lookups stay short
		bool Erase(Storage* s, std::uintptr_t ptr, Record* out)
		{
			std::size_t idx = Hash(ptr) & (kSlots - 1);
			for (std::size_t i = 0; i < kSlots; ++i, idx = (idx + 1) & (kSlots - 1)) {
				if (s->records[idx].ptr == 0) return false;
				if (s->records[idx].ptr == ptr) break;
				if (i == kSlots - 1) return false;
			}
			*out = s->records[idx];
			std::size_t hole = idx;
			for (std::size_t j = (hole + 1) & (kSlots - 1);; j = (j + 1) & (kSlots - 1)) {
				Record& r = s->records[j];
				if (r.ptr == 0) break;
				const std::size_t home = Hash(r.ptr) & (kSlots - 1);
				// Move r into the hole if its home is not in (hole, j]
				const bool between = hole <= j ? (home > hole && home <= j) : (home > hole || home <= j);
				if (!between) {
					s->records[hole] = r;
					hole = j;
				}
			}
			s->records[hole] = Record{};
			g_used.fetch_sub(1, std::memory_order_relaxed);
			std::atomic_ref<std::uint32_t>(s->filter[Hash(ptr) & (kFilter - 1)]).fetch_sub(1, std::memory_order_release);
			return true;
		}

		int LifetimeBucket(std::uint64_t ms)
		{
			if (ms < 16) return 0;
			if (ms < 1000) return 1;
			if (ms < 60000) return 2;
			if (ms < 600000) return 3;
			return 4;
		}
	}

	void SetMode(Mode mode, std::size_t intervalBytes)
	{
		g_interval.store(mode == Mode::Full ? 0 : std::max<std::size_t>(intervalBytes, 4096), std::memory_order_relaxed);
		g_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
		g_settingsEpoch.fetch_add(1, std::memory_order_release);
	}

	Mode CurrentMode() { return static_cast<Mode>(g_mode.load(std::memory_order_relaxed)); }
	std::size_t Interval() { return g_interval.load(std::memory_order_relaxed); }
	std::uint32_t Generation() { return g_seq.load(std::memory_order_relaxed); }

	void OnAlloc(void* p, std::size_t bytes, TagId tag) noexcept
	{
		const auto mode = static_cast<Mode>(g_mode.load(std::memory_order_relaxed));
		if (mode == Mode::Off || t_busy || !p) return;
		const std::size_t interval = g_interval.load(std::memory_order_relaxed);
		if (mode == Mode::Sampled) {
			const std::uint32_t epoch = g_settingsEpoch.load(std::memory_order_relaxed);
			if (t_epoch != epoch) {
				t_epoch = epoch;
				t_until = NextGap(interval);
			}
			t_until -= static_cast<std::int64_t>(bytes);
			if (t_until > 0) return;
			t_until = NextGap(interval);
		}
		t_busy = true;
		Storage* s = Store();
		if (s) {
			if (g_used.load(std::memory_order_relaxed) >= kMaxLoad) {
				g_dropped.fetch_add(1, std::memory_order_relaxed);
			} else {
				std::uintptr_t frames[kDepth];
				const int depth = RtlCaptureStackBackTrace(3, kDepth, reinterpret_cast<void**>(frames), nullptr);
				Record r{};
				r.ptr = reinterpret_cast<std::uintptr_t>(p);
				r.size = static_cast<std::uint32_t>(std::min<std::size_t>(bytes, UINT32_MAX));
				r.weight = Weight(bytes, interval);
				r.seq = g_seq.fetch_add(1, std::memory_order_relaxed) + 1;
				r.timeMs = RealTimeMs();
				r.tag = tag;
				r.thread = GetCurrentThreadId();
				AcquireSRWLockExclusive(&g_lock);
				r.stack = InternStack(s, frames, depth);
				Insert(s, r);
				ReleaseSRWLockExclusive(&g_lock);
			}
		}
		t_busy = false;
	}

	void OnFree(void* p) noexcept
	{
		// t_busy: a free inside the sampler's own work (it holds the lock): never wait on it again
		if (!p || t_busy || g_used.load(std::memory_order_relaxed) == 0) return;
		Storage* s = g_store.load(std::memory_order_acquire);
		if (!s) return;
		const auto ptr = reinterpret_cast<std::uintptr_t>(p);
		if (std::atomic_ref<std::uint32_t>(s->filter[Hash(ptr) & (kFilter - 1)]).load(std::memory_order_acquire) == 0) return;
		t_busy = true;
		AcquireSRWLockExclusive(&g_lock);
		Record r;
		if (Erase(s, ptr, &r)) g_lifetime[LifetimeBucket(RealTimeMs() - r.timeMs)] += r.weight;
		ReleaseSRWLockExclusive(&g_lock);
		t_busy = false;
	}

	void Clear()
	{
		Storage* s = g_store.load(std::memory_order_acquire);
		if (!s) return;
		AcquireSRWLockExclusive(&g_lock);
		std::memset(s->records, 0, sizeof s->records);
		std::memset(s->filter, 0, sizeof s->filter);
		std::memset(s->stacks, 0, sizeof s->stacks);
		g_stacksUsed = 0;
		std::memset(g_lifetime, 0, sizeof g_lifetime);
		g_used.store(0, std::memory_order_relaxed);
		ReleaseSRWLockExclusive(&g_lock);
	}

	Report Snapshot(std::uint32_t since, std::size_t top, std::uint32_t thread)
	{
		Report rep;
		rep.mode = CurrentMode();
		rep.interval = Interval();
		rep.samples = Generation();
		rep.dropped = g_dropped.load(std::memory_order_relaxed);
		rep.tableCapacity = kMaxLoad;
		rep.stacksCapacity = kStacks;
		Storage* s = g_store.load(std::memory_order_acquire);
		if (!s) return rep;

		// Copy under the lock into memory reserved before it (nothing is allocated while the lock is held). The sampler's
		// own buffers are never sampled (t_busy): a report must not count itself.
		const bool busy = t_busy;
		t_busy = true;
		std::vector<Record> live;
		live.reserve(std::min(kSlots, g_used.load(std::memory_order_relaxed) + g_used.load(std::memory_order_relaxed) / 4 + 256));
		std::vector<Stack> stacks;
		stacks.reserve(kStacks);
		AcquireSRWLockShared(&g_lock);
		for (std::size_t i = 0; i < kSlots; ++i)
			if (s->records[i].ptr && s->records[i].seq > since && (!thread || s->records[i].thread == thread) &&
				live.size() < live.capacity())
				live.push_back(s->records[i]);
		for (std::size_t i = 0; i < kStacks; ++i) stacks.push_back(s->stacks[i]);
		std::memcpy(rep.lifetime, g_lifetime, sizeof rep.lifetime);
		rep.stacksUsed = g_stacksUsed;
		ReleaseSRWLockShared(&g_lock);
		t_busy = busy;
		rep.tableUsed = g_used.load(std::memory_order_relaxed);

		const std::uint64_t now = RealTimeMs();
		std::map<std::uint32_t, Site> byStack;
		for (const Record& r : live) {
			rep.liveBytes += r.weight;
			const std::uint64_t blocks = r.size ? std::max<std::uint64_t>(1, r.weight / r.size) : 1;
			rep.liveBlocks += blocks;
			rep.liveByTag[r.tag < kMaxTags ? r.tag : 0] += r.weight;
			const std::uint64_t age = now - std::min(now, r.timeMs);
			if (age > 60000) rep.longLivedBytes += r.weight;
			Site& site = byStack[r.stack];
			if (site.samples == 0) {
				site.stack = r.stack;
				site.tag = r.tag;
				if (r.stack && r.stack <= kStacks) {
					const Stack& st = stacks[r.stack - 1];
					site.frames.assign(st.frames, st.frames + st.depth);
				}
			}
			++site.samples;
			site.liveBytes += r.weight;
			site.liveBlocks += blocks;
			site.oldestAgeMs = std::max(site.oldestAgeMs, age);
		}
		for (auto& [id, site] : byStack) rep.sites.push_back(std::move(site));
		std::sort(rep.sites.begin(), rep.sites.end(), [](const Site& a, const Site& b) { return a.liveBytes > b.liveBytes; });
		if (rep.sites.size() > top) rep.sites.resize(top);
		return rep;
	}

	std::string Describe(std::uintptr_t address)
	{
		HMODULE module = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
							   reinterpret_cast<LPCWSTR>(address), &module) && module) {
			char path[MAX_PATH] = {};
			GetModuleFileNameA(module, path, MAX_PATH);
			const char* name = std::strrchr(path, '\\');
			char buf[MAX_PATH + 32];
			std::snprintf(buf, sizeof buf, "%s+0x%X", name ? name + 1 : path,
						  static_cast<unsigned>(address - reinterpret_cast<std::uintptr_t>(module)));
			return buf;
		}
		char buf[32];
		std::snprintf(buf, sizeof buf, "0x%08X", static_cast<unsigned>(address));
		return buf;
	}
}
