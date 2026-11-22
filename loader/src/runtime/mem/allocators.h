#pragma once
/** @file
 * @ingroup mirage_memory
 * @brief Buddy, small-object, and stack allocators for Mirage-owned memory.
 */
// More allocators for the menu's own memory (none of them is ever used for the game's):
//
//   BuddyAllocator   power-of-two blocks from one region reserved outside the heap: split on demand, merged with
//                    their buddy when freed. Variable sizes without heap fragmentation; the region is one contiguous
//                    reservation (good for the 32-bit address space: no scattering). Locked: any thread.
//   SmallAllocator   small objects (<= 256 bytes) by size class, each class a SyncPool (slabs): far fewer heap blocks
//                    and headers for many tiny allocations. Larger requests go to the heap. Any thread.
//   SmallBuffer<N>   N bytes inline (on the stack or in the owner), the heap only when more is asked for.
//   StackAllocator   LIFO from one buffer: each Free must be the newest block (checked), or rewind to a marker.
//                    For nested temporary work with a known bound. One thread.
//
// Like the pools and arenas (pool.h, arena.h): a full allocator returns nullptr (never throws, never takes memory from
// elsewhere on its own), and every one shows its statistics in the memory report when given a name.
#include "runtime/mem/analysis.h"		// Fragmentation
#include "runtime/mem/pool.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace mirage::mem
{
	// ---- Buddy ---------------------------------------------------------------------------------------------------------
	struct BuddyStats
	{
		std::string name;
		std::size_t regionBytes = 0, minBlock = 0;
		std::size_t usedBytes = 0;						// In blocks handed out (their power-of-two size)
		std::size_t requestedBytes = 0;					// What was asked for (internal fragmentation = used - requested)
		std::uint64_t allocations = 0, frees = 0, failures = 0, splits = 0, merges = 0;
		Fragmentation free;								// Of the free blocks (external fragmentation)
	};

	/// @ingroup mirage_memory
	/// @brief Own the reserved region used for buddy allocation.
	class BuddyAllocator
	{
	public:
		// The region is `1 << maxOrder` bytes (default 4 MB), blocks at least `1 << minOrder` (default 64 B)
		BuddyAllocator(std::string name, int maxOrder = 22, int minOrder = 6);
		~BuddyAllocator();
		BuddyAllocator(const BuddyAllocator&) = delete;
		BuddyAllocator& operator=(const BuddyAllocator&) = delete;

		void* Allocate(std::size_t bytes);		// nullptr: too big, or no block of that size is free
		void Free(void* p);
		bool Owns(const void* p) const;
		std::size_t BlockSize(const void* p) const;	// The size of the block p is in (0: not ours)
		BuddyStats Stats() const;
		bool Ok() const { return base_ != nullptr; }

	private:
		struct Node { Node* prev; Node* next; };
		int OrderFor(std::size_t bytes) const;
		void Push(int order, Node* n);
		void Unlink(int order, Node* n);
		std::size_t Index(const void* p) const;		// In minimum blocks

		std::string name_;
		int maxOrder_, minOrder_;
		std::uint8_t* base_ = nullptr;
		std::size_t size_ = 0;
		std::vector<Node*> free_;					// A list per order (index order - minOrder)
		// Per minimum block: 0 = not the start of a block; else (order - minOrder + 1) | 0x80 when allocated
		std::vector<std::uint8_t> state_;
		std::vector<std::uint32_t> requested_;		// Per minimum block (start of an allocated block): bytes asked for
		mutable std::mutex lock_;
		BuddyStats stats_;
	};

	// ---- Small objects -------------------------------------------------------------------------------------------------
	/// @brief Pool small allocations by size class to reduce heap traffic.
	class SmallAllocator
	{
	public:
		static constexpr std::size_t kMaxSize = 256;
		explicit SmallAllocator(std::string name = "Small objects");
		void* Allocate(std::size_t bytes);			// Heap for bytes > kMaxSize
		void Free(void* p, std::size_t bytes);		// The same size that was asked for (like sized delete)
		static int ClassOf(std::size_t bytes);		// -1: larger than kMaxSize
		static std::size_t ClassSize(int c);
		std::vector<PoolStats> Stats() const;
		std::uint64_t HeapFallbacks() const { return heap_.load(std::memory_order_relaxed); }
	private:
		static constexpr std::array<std::size_t, 10> kClasses = {8, 16, 24, 32, 48, 64, 96, 128, 192, 256};
		std::vector<std::unique_ptr<SyncPool>> pools_;
		std::atomic<std::uint64_t> heap_{0};
	};
	SmallAllocator& Small();						// The shared one

	// For objects that are made and freed often in large numbers: new / delete through Small()
	template <class T>
	struct SmallObject
	{
		static void* operator new(std::size_t n) { if (void* p = Small().Allocate(n)) return p; throw std::bad_alloc(); }
		static void operator delete(void* p, std::size_t n) { Small().Free(p, n); }
	};

	// N bytes inline, more from the heap; the contents are kept on Resize
	template <std::size_t N>
	/// @brief Owning byte buffer backed by the small allocation service.
	class SmallBuffer
	{
	public:
		SmallBuffer() = default;
		explicit SmallBuffer(std::size_t size) { Resize(size); }
		~SmallBuffer() { if (heap_) std::free(heap_); }
		SmallBuffer(const SmallBuffer&) = delete;
		SmallBuffer& operator=(const SmallBuffer&) = delete;
		bool Resize(std::size_t size)			// false: no memory (the buffer is as it was)
		{
			if (size <= Capacity()) { size_ = size; return true; }
			auto* p = static_cast<std::uint8_t*>(std::malloc(size));
			if (!p) return false;
			std::memcpy(p, Data(), size_);
			if (heap_) std::free(heap_);
			heap_ = p;
			capacity_ = size;
			size_ = size;
			return true;
		}
		std::uint8_t* Data() { return heap_ ? heap_ : inline_; }
		const std::uint8_t* Data() const { return heap_ ? heap_ : inline_; }
		std::size_t Size() const { return size_; }
		std::size_t Capacity() const { return heap_ ? capacity_ : N; }
		bool OnHeap() const { return heap_ != nullptr; }
	private:
		alignas(std::max_align_t) std::uint8_t inline_[N];
		std::uint8_t* heap_ = nullptr;
		std::size_t size_ = 0, capacity_ = 0;
	};

	// ---- Stack ---------------------------------------------------------------------------------------------------------
	/// @brief Single-thread stack allocator that can rewind to a saved marker.
	class StackAllocator
	{
	public:
		explicit StackAllocator(std::size_t bytes);
		~StackAllocator();
		StackAllocator(const StackAllocator&) = delete;
		StackAllocator& operator=(const StackAllocator&) = delete;

		void* Allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t));	// nullptr: full
		bool Free(void* p);					// false (and nothing freed): p isn't the newest block
		struct Marker { std::size_t top = 0, newest = 0; };
		Marker Mark() const { return {top_, newest_}; }
		void Rewind(Marker m);				// Everything allocated since the marker is gone
		std::size_t Used() const { return top_; }
		std::size_t Capacity() const { return size_; }
		std::size_t Peak() const { return peak_; }
		std::uint64_t Misuses() const { return misuses_; }	// Frees that weren't the newest block
	private:
		std::uint8_t* buffer_ = nullptr;
		std::size_t size_ = 0, top_ = 0, peak_ = 0;
		std::size_t newest_ = 0;			// The newest block's offset + 1 (0 = none)
		std::uint64_t misuses_ = 0;
	};
}
