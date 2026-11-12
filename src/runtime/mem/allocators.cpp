#include "runtime/mem/allocators.h"

#include <Windows.h>

#include <algorithm>
#include <cassert>
#include <cstdlib>

namespace mirage::mem
{
	// ---- Buddy -------------------------------------------------------------------------------------------------------------
	BuddyAllocator::BuddyAllocator(std::string name, int maxOrder, int minOrder)
		: name_(std::move(name)), maxOrder_(std::clamp(maxOrder, 12, 28)), minOrder_(std::clamp(minOrder, 4, 12))
	{
		if (minOrder_ > maxOrder_) minOrder_ = maxOrder_;
		size_ = std::size_t{1} << maxOrder_;
		base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
		free_.assign(static_cast<std::size_t>(maxOrder_ - minOrder_ + 1), nullptr);
		const std::size_t units = size_ >> minOrder_;
		state_.assign(units, 0);
		requested_.assign(units, 0);
		stats_.name = name_;
		stats_.regionBytes = base_ ? size_ : 0;
		stats_.minBlock = std::size_t{1} << minOrder_;
		if (base_) {
			state_[0] = static_cast<std::uint8_t>(maxOrder_ - minOrder_ + 1);
			Push(maxOrder_, reinterpret_cast<Node*>(base_));
		}
		pools::Add({this, [this] {
			const BuddyStats b = Stats();
			PoolStats p;
			p.name = b.name + " (buddy)";
			p.blockSize = b.minBlock;
			p.used = b.usedBytes / b.minBlock;
			p.capacity = b.regionBytes / b.minBlock;
			p.allocations = b.allocations;
			p.frees = b.frees;
			p.fallbacks = b.failures;
			p.slabs = 1;
			p.slabBytes = b.regionBytes;
			return p;
		}, {}});
	}

	BuddyAllocator::~BuddyAllocator()
	{
		pools::Remove(this);
		if (base_) VirtualFree(base_, 0, MEM_RELEASE);
	}

	int BuddyAllocator::OrderFor(std::size_t bytes) const
	{
		int order = minOrder_;
		while (order <= maxOrder_ && (std::size_t{1} << order) < bytes) ++order;
		return order;
	}

	void BuddyAllocator::Push(int order, Node* n)
	{
		Node*& head = free_[order - minOrder_];
		n->prev = nullptr;
		n->next = head;
		if (head) head->prev = n;
		head = n;
	}

	void BuddyAllocator::Unlink(int order, Node* n)
	{
		Node*& head = free_[order - minOrder_];
		if (n->prev) n->prev->next = n->next;
		else head = n->next;
		if (n->next) n->next->prev = n->prev;
	}

	std::size_t BuddyAllocator::Index(const void* p) const
	{
		return static_cast<std::size_t>(static_cast<const std::uint8_t*>(p) - base_) >> minOrder_;
	}

	bool BuddyAllocator::Owns(const void* p) const
	{
		if (!base_ || p < base_ || p >= base_ + size_) return false;
		const auto off = static_cast<std::size_t>(static_cast<const std::uint8_t*>(p) - base_);
		if (off & ((std::size_t{1} << minOrder_) - 1)) return false;
		std::lock_guard<std::mutex> l(lock_);
		return (state_[Index(p)] & 0x80) != 0;
	}

	std::size_t BuddyAllocator::BlockSize(const void* p) const
	{
		if (!Owns(p)) return 0;
		std::lock_guard<std::mutex> l(lock_);
		return std::size_t{1} << ((state_[Index(p)] & 0x7F) - 1 + minOrder_);
	}

	void* BuddyAllocator::Allocate(std::size_t bytes)
	{
		if (!base_ || bytes == 0) return nullptr;
		const int order = OrderFor(bytes);
		std::lock_guard<std::mutex> l(lock_);
		if (order > maxOrder_) {
			++stats_.failures;
			return nullptr;
		}
		// The smallest free block that fits, split down to size
		int from = order;
		while (from <= maxOrder_ && !free_[from - minOrder_]) ++from;
		if (from > maxOrder_) {
			++stats_.failures;
			return nullptr;
		}
		Node* n = free_[from - minOrder_];
		Unlink(from, n);
		auto* block = reinterpret_cast<std::uint8_t*>(n);
		while (from > order) {
			--from;
			auto* buddy = block + (std::size_t{1} << from);
			state_[Index(buddy)] = static_cast<std::uint8_t>(from - minOrder_ + 1);
			Push(from, reinterpret_cast<Node*>(buddy));
			++stats_.splits;
		}
		const std::size_t idx = Index(block);
		state_[idx] = static_cast<std::uint8_t>((order - minOrder_ + 1) | 0x80);
		requested_[idx] = static_cast<std::uint32_t>(bytes);
		stats_.usedBytes += std::size_t{1} << order;
		stats_.requestedBytes += bytes;
		++stats_.allocations;
		return block;
	}

	void BuddyAllocator::Free(void* p)
	{
		if (!p) return;
		std::lock_guard<std::mutex> l(lock_);
		std::size_t idx = Index(p);
		assert(p >= base_ && p < base_ + size_ && (state_[idx] & 0x80) && "BuddyAllocator::Free: not a block of this allocator");
		if (!(p >= base_ && p < base_ + size_) || !(state_[idx] & 0x80)) return;		// Not ours, or freed twice: ignored
		int order = (state_[idx] & 0x7F) - 1 + minOrder_;
		stats_.usedBytes -= std::size_t{1} << order;
		stats_.requestedBytes -= requested_[idx];
		requested_[idx] = 0;
		++stats_.frees;
		auto* block = static_cast<std::uint8_t*>(p);
		// Merge with the buddy while it is a free block of the same order
		while (order < maxOrder_) {
			const std::size_t off = static_cast<std::size_t>(block - base_);
			auto* buddy = base_ + (off ^ (std::size_t{1} << order));
			const std::uint8_t bs = state_[Index(buddy)];
			if (bs != static_cast<std::uint8_t>(order - minOrder_ + 1)) break;		// Allocated, split, or another size
			Unlink(order, reinterpret_cast<Node*>(buddy));
			state_[Index(buddy)] = 0;
			state_[Index(block)] = 0;
			block = std::min(block, buddy);
			++order;
			++stats_.merges;
		}
		state_[Index(block)] = static_cast<std::uint8_t>(order - minOrder_ + 1);
		Push(order, reinterpret_cast<Node*>(block));
	}

	BuddyStats BuddyAllocator::Stats() const
	{
		std::lock_guard<std::mutex> l(lock_);
		BuddyStats s = stats_;
		s.free = {};
		for (int order = minOrder_; order <= maxOrder_; ++order)
			for (Node* n = free_[order - minOrder_]; n; n = n->next) s.free.AddFree(std::uint64_t{1} << order);
		return s;
	}

	// ---- Small objects -------------------------------------------------------------------------------------------------
	SmallAllocator::SmallAllocator(std::string name)
	{
		for (std::size_t size : kClasses)
			pools_.push_back(std::make_unique<SyncPool>(name + " " + std::to_string(size) + " B", size, 4096 / size, 0, 1));
	}

	int SmallAllocator::ClassOf(std::size_t bytes)
	{
		for (std::size_t i = 0; i < kClasses.size(); ++i)
			if (bytes <= kClasses[i]) return static_cast<int>(i);
		return -1;
	}

	std::size_t SmallAllocator::ClassSize(int c) { return c >= 0 && c < static_cast<int>(kClasses.size()) ? kClasses[c] : 0; }

	void* SmallAllocator::Allocate(std::size_t bytes)
	{
		const int c = ClassOf(bytes ? bytes : 1);
		if (c >= 0)
			if (void* p = pools_[c]->Allocate()) return p;
		// Larger, or the pool can't grow: the heap, with the same Free rule (the size tells which)
		heap_.fetch_add(1, std::memory_order_relaxed);
		return std::malloc(bytes ? bytes : 1);
	}

	void SmallAllocator::Free(void* p, std::size_t bytes)
	{
		if (!p) return;
		const int c = ClassOf(bytes ? bytes : 1);
		if (c >= 0 && pools_[c]->Owns(p)) pools_[c]->Free(p);
		else std::free(p);
	}

	std::vector<PoolStats> SmallAllocator::Stats() const
	{
		std::vector<PoolStats> out;
		for (const auto& p : pools_) out.push_back(p->Stats());
		return out;
	}

	SmallAllocator& Small()
	{
		static SmallAllocator* s = new SmallAllocator();		// Never destroyed: objects may outlive static destructors
		return *s;
	}

	// ---- Stack ---------------------------------------------------------------------------------------------------------
	namespace
	{
		struct StackHeader
		{
			std::uint32_t previousTop;		// The top before this block
			std::uint32_t previousNewest;	// The newest block before this one (offset + 1, 0 = none)
		};
	}

	StackAllocator::StackAllocator(std::size_t bytes)
	{
		buffer_ = static_cast<std::uint8_t*>(_aligned_malloc(bytes ? bytes : 1, alignof(std::max_align_t)));
		size_ = buffer_ ? bytes : 0;
	}

	StackAllocator::~StackAllocator() { _aligned_free(buffer_); }

	void* StackAllocator::Allocate(std::size_t bytes, std::size_t align)
	{
		if (!buffer_) return nullptr;
		if (align < alignof(StackHeader) || (align & (align - 1))) align = alignof(std::max_align_t);
		const auto base = reinterpret_cast<std::uintptr_t>(buffer_);
		const std::uintptr_t at = (base + top_ + sizeof(StackHeader) + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
		const std::size_t end = static_cast<std::size_t>(at - base) + bytes;
		if (end > size_) return nullptr;
		auto* h = reinterpret_cast<StackHeader*>(at - sizeof(StackHeader));
		h->previousTop = static_cast<std::uint32_t>(top_);
		h->previousNewest = static_cast<std::uint32_t>(newest_);
		newest_ = static_cast<std::size_t>(at - base) + 1;
		top_ = end;
		peak_ = std::max(peak_, top_);
		return reinterpret_cast<void*>(at);
	}

	bool StackAllocator::Free(void* p)
	{
		if (!p) return true;
		auto* at = static_cast<std::uint8_t*>(p);
		// Only the newest block: freeing another would free every block allocated after it too, so it is refused
		if (at < buffer_ || at >= buffer_ + size_ || static_cast<std::size_t>(at - buffer_) + 1 != newest_) {
			++misuses_;
			return false;
		}
		const auto* h = reinterpret_cast<const StackHeader*>(at - sizeof(StackHeader));
		top_ = h->previousTop;
		newest_ = h->previousNewest;
		return true;
	}

	void StackAllocator::Rewind(Marker m)
	{
		if (m.top > top_) return;		// A marker from after a rewind past it: nothing to do
		top_ = m.top;
		newest_ = m.newest;
	}
}
