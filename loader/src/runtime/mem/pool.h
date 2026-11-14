#pragma once
// Resource pools: fixed-size blocks carved from slabs, for objects made and destroyed often (events, nodes, records).
// No heap call per object, no fragmentation of the CRT heap by them, and free slabs go back when memory is short.
//
//   mirage::mem::ObjectPool<Node> nodes("Graph nodes");          // registers itself: diagnostics, pressure trimming
//   Node* n = nodes.Create(args...);  ...  nodes.Destroy(n);
//
// FixedPool and ObjectPool are for one thread (or the caller's own lock); SyncPool / SyncObjectPool take a lock.
// Safe fallback: when a slab can't be had (the pool's slab cap, or no memory), ObjectPool::Create uses plain new and
// Destroy knows which is which (Owns), so a full pool never fails a caller that the heap wouldn't.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace mirage::mem
{
	struct PoolStats
	{
		std::string name;
		std::size_t blockSize = 0, blocksPerSlab = 0;
		std::size_t slabs = 0, slabBytes = 0;
		std::size_t used = 0, capacity = 0;		// Blocks
		std::uint64_t allocations = 0, frees = 0, slabAllocs = 0, slabFrees = 0, fallbacks = 0;
		std::size_t peakUsed = 0;
	};

	// A counter one thread writes and any thread reads (plain moves on x86: no locked instruction on the hot path)
	struct OwnerCounter
	{
		std::atomic<std::uint64_t> v{0};
		void Add(std::uint64_t n = 1) { v.store(v.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); }
		void Sub(std::uint64_t n = 1) { v.store(v.load(std::memory_order_relaxed) - n, std::memory_order_relaxed); }
		void Set(std::uint64_t n) { v.store(n, std::memory_order_relaxed); }
		std::uint64_t Get() const { return v.load(std::memory_order_relaxed); }
	};

	// Asks every pool and scratch arena to give back what it keeps spare. Unsynchronized ones do it themselves, on their
	// own thread, the next time they are used; SyncPools are trimmed by pools::TrimAll at once. (The manager's own RequestTrim, manager.h, ends up here.)
	void RequestSpareTrim();
	std::uint32_t TrimEpoch();

	class FixedPool
	{
	public:
		// `maxSlabs` 0 = no cap. `keepSlabs`: empty slabs kept for reuse (Trim gives back the rest; Release all).
		// `registered`: listed in the diagnostics (its statistics are safe to read from any thread).
		FixedPool(std::string name, std::size_t blockSize, std::size_t blocksPerSlab = 64, std::size_t maxSlabs = 0,
				  std::size_t keepSlabs = 1, bool registered = true);
		~FixedPool();
		FixedPool(const FixedPool&) = delete;
		FixedPool& operator=(const FixedPool&) = delete;

		void* Allocate();			// nullptr when no slab can be had
		void Free(void* p);			// A block of this pool (checked in debug builds)
		bool Owns(const void* p) const;
		std::size_t Trim();			// Gives back empty slabs beyond keepSlabs; returns bytes freed
		std::size_t Release();		// Gives back every empty slab
		PoolStats Stats() const;	// Any thread
		const std::string& Name() const { return name_; }
		void CountFallback() { fallbacks_.Add(); }

	private:
		struct Slab;
		struct Block { Block* next; };
		Slab* NewSlab();
		void FreeSlab(Slab* s);
		static void Unlink(Slab*& head, Slab* s);
		static void Push(Slab*& head, Slab* s);
		Slab* SlabOf(const void* p) const;
		std::size_t FreeEmpty(std::size_t keep);
		void CheckEpoch();

		std::string name_;
		std::size_t block_, perSlab_, maxSlabs_, keepSlabs_, slabBytes_;
		Slab* partial_ = nullptr;		// Slabs with free blocks (empty ones too)
		Slab* full_ = nullptr;
		std::vector<std::uintptr_t> bases_;	// Sorted slab addresses (Owns)
		std::size_t used_ = 0;
		OwnerCounter slabs_, usedOut_, peak_, allocs_, frees_, slabAllocs_, slabFrees_, fallbacks_;
		std::uint32_t epoch_ = 0;
		bool registered_;
	};

	// A FixedPool behind a lock (any thread; the manager trims it directly)
	class SyncPool
	{
	public:
		SyncPool(std::string name, std::size_t blockSize, std::size_t blocksPerSlab = 64, std::size_t maxSlabs = 0,
				 std::size_t keepSlabs = 1);
		~SyncPool();
		SyncPool(const SyncPool&) = delete;
		SyncPool& operator=(const SyncPool&) = delete;
		void* Allocate() { std::lock_guard<std::mutex> l(lock_); return pool_.Allocate(); }
		void Free(void* p) { std::lock_guard<std::mutex> l(lock_); pool_.Free(p); }
		bool Owns(const void* p) const { std::lock_guard<std::mutex> l(lock_); return pool_.Owns(p); }
		std::size_t Trim() { std::lock_guard<std::mutex> l(lock_); return pool_.Trim(); }
		PoolStats Stats() const { return pool_.Stats(); }
		void CountFallback() { std::lock_guard<std::mutex> l(lock_); pool_.CountFallback(); }
	private:
		mutable std::mutex lock_;
		FixedPool pool_;
	};

	// Typed objects from a pool, falling back to new when the pool can't give a block
	template <class T, class Pool = FixedPool>
	class ObjectPool
	{
	public:
		explicit ObjectPool(std::string name, std::size_t perSlab = 64, std::size_t maxSlabs = 0)
			: pool_(std::move(name), sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T), perSlab, maxSlabs)
		{
			static_assert(alignof(T) <= alignof(std::max_align_t), "over-aligned types need their own allocator");
		}
		template <class... A>
		T* Create(A&&... args)
		{
			void* p = pool_.Allocate();
			if (!p) {
				pool_.CountFallback();
				return new T(std::forward<A>(args)...);
			}
			try {
				return ::new (p) T(std::forward<A>(args)...);
			} catch (...) {
				pool_.Free(p);
				throw;
			}
		}
		void Destroy(T* t)
		{
			if (!t) return;
			if (pool_.Owns(t)) {
				t->~T();
				pool_.Free(t);
			} else {
				delete t;
			}
		}
		Pool& Raw() { return pool_; }
		PoolStats Stats() const { return pool_.Stats(); }
	private:
		Pool pool_;
	};

	template <class T>
	using SyncObjectPool = ObjectPool<T, SyncPool>;

	// ---- The registry: every registered pool, for diagnostics and the pressure response -----------------------------------
	namespace pools
	{
		struct Entry
		{
			const void* owner;
			std::function<PoolStats()> stats;		// Any thread
			std::function<std::size_t()> trim;		// Any thread; empty: it trims itself (RequestSpareTrim)
		};
		void Add(Entry e);
		void Remove(const void* owner);
		std::vector<PoolStats> Snapshot();
		std::size_t TrimAll();						// Bytes given back now (self-trimming pools follow later)
	}
}
