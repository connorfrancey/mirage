#include "runtime/mem/pool.h"

#include <algorithm>
#include <cassert>
#include <malloc.h>

namespace mirage::mem
{
	namespace
	{
		constexpr std::size_t kAlign = alignof(std::max_align_t);
		std::size_t Round(std::size_t n, std::size_t a) { return (n + a - 1) / a * a; }
		std::size_t Pow2(std::size_t n)
		{
			std::size_t p = 4096;
			while (p < n) p <<= 1;
			return p;
		}
		std::atomic<std::uint32_t> g_trimEpoch{0};
	}

	void RequestSpareTrim() { g_trimEpoch.fetch_add(1, std::memory_order_relaxed); }
	std::uint32_t TrimEpoch() { return g_trimEpoch.load(std::memory_order_relaxed); }

	// Slabs are aligned to their (power of two) size, so a block's slab is its address with the low bits cleared
	struct FixedPool::Slab
	{
		Slab* prev = nullptr;
		Slab* next = nullptr;
		Block* free = nullptr;			// Blocks given back
		std::size_t used = 0;
		std::size_t fresh = 0;			// Blocks never handed out start here (no free list built up front)
		bool full = false;
	};

	namespace
	{
		constexpr std::size_t kHeader = (sizeof(void*) * 4 + sizeof(std::size_t) * 2 + kAlign) / kAlign * kAlign;
	}

	FixedPool::FixedPool(std::string name, std::size_t blockSize, std::size_t blocksPerSlab, std::size_t maxSlabs,
						 std::size_t keepSlabs, bool registered)
		: name_(std::move(name)), block_(Round(std::max(blockSize, sizeof(Block)), kAlign)),
		  perSlab_(std::max<std::size_t>(blocksPerSlab, 1)), maxSlabs_(maxSlabs), keepSlabs_(keepSlabs), registered_(registered)
	{
		static_assert(sizeof(Slab) <= kHeader);
		slabBytes_ = Pow2(kHeader + block_ * perSlab_);
		perSlab_ = (slabBytes_ - kHeader) / block_;		// Use the whole power of two
		epoch_ = TrimEpoch();
		if (registered_) pools::Add({this, [this] { return Stats(); }, {}});
	}

	FixedPool::~FixedPool()
	{
		if (registered_) pools::Remove(this);
		// Blocks still in use are the owner's leak: their slabs are freed anyway (nothing may use a dead pool)
		for (Slab* list : {partial_, full_}) {
			while (list) {
				Slab* next = list->next;
				_aligned_free(list);
				list = next;
			}
		}
	}

	void FixedPool::Push(Slab*& head, Slab* s)
	{
		s->prev = nullptr;
		s->next = head;
		if (head) head->prev = s;
		head = s;
	}

	void FixedPool::Unlink(Slab*& head, Slab* s)
	{
		if (s->prev) s->prev->next = s->next;
		else head = s->next;
		if (s->next) s->next->prev = s->prev;
		s->prev = s->next = nullptr;
	}

	FixedPool::Slab* FixedPool::NewSlab()
	{
		if (maxSlabs_ && slabs_.Get() >= maxSlabs_) return nullptr;
		void* mem = _aligned_malloc(slabBytes_, slabBytes_);
		if (!mem) return nullptr;
		Slab* s = ::new (mem) Slab();
		const auto base = reinterpret_cast<std::uintptr_t>(mem);
		bases_.insert(std::upper_bound(bases_.begin(), bases_.end(), base), base);
		slabs_.Add();
		slabAllocs_.Add();
		Push(partial_, s);
		return s;
	}

	void FixedPool::FreeSlab(Slab* s)
	{
		Unlink(s->full ? full_ : partial_, s);
		const auto base = reinterpret_cast<std::uintptr_t>(s);
		bases_.erase(std::lower_bound(bases_.begin(), bases_.end(), base));
		slabs_.Sub();
		slabFrees_.Add();
		s->~Slab();
		_aligned_free(s);
	}

	FixedPool::Slab* FixedPool::SlabOf(const void* p) const
	{
		return reinterpret_cast<Slab*>(reinterpret_cast<std::uintptr_t>(p) & ~(static_cast<std::uintptr_t>(slabBytes_) - 1));
	}

	bool FixedPool::Owns(const void* p) const
	{
		if (!p) return false;
		const auto base = reinterpret_cast<std::uintptr_t>(SlabOf(p));
		if (!std::binary_search(bases_.begin(), bases_.end(), base)) return false;
		const auto off = reinterpret_cast<std::uintptr_t>(p) - base;
		return off >= kHeader && (off - kHeader) % block_ == 0 && (off - kHeader) / block_ < perSlab_;
	}

	void FixedPool::CheckEpoch()
	{
		const std::uint32_t e = TrimEpoch();
		if (e != epoch_) {
			epoch_ = e;
			Trim();
		}
	}

	void* FixedPool::Allocate()
	{
		CheckEpoch();
		// The head is the slab most recently freed into: good locality, and cheap
		Slab* s = partial_;
		if (!s) s = NewSlab();
		if (!s) return nullptr;
		void* p;
		if (s->free) {
			Block* b = s->free;
			s->free = b->next;
			p = b;
		} else {
			p = reinterpret_cast<char*>(s) + kHeader + s->fresh * block_;
			++s->fresh;
		}
		++s->used;
		if (s->used == perSlab_) {
			Unlink(partial_, s);
			Push(full_, s);
			s->full = true;
		}
		++used_;
		usedOut_.Set(used_);
		allocs_.Add();
		if (used_ > peak_.Get()) peak_.Set(used_);
		return p;
	}

	void FixedPool::Free(void* p)
	{
		if (!p) return;
		assert(Owns(p) && "FixedPool::Free: not a block of this pool");
		Slab* s = SlabOf(p);
		auto* b = static_cast<Block*>(p);
		b->next = s->free;
		s->free = b;
		if (s->full) {
			Unlink(full_, s);
			Push(partial_, s);
			s->full = false;
		}
		--s->used;
		--used_;
		usedOut_.Set(used_);
		frees_.Add();
		CheckEpoch();
	}

	std::size_t FixedPool::FreeEmpty(std::size_t keep)
	{
		std::size_t empty = 0, freed = 0;
		for (Slab* s = partial_; s;) {
			Slab* next = s->next;
			if (s->used == 0 && ++empty > keep) {
				FreeSlab(s);
				freed += slabBytes_;
			}
			s = next;
		}
		return freed;
	}

	std::size_t FixedPool::Trim() { return FreeEmpty(keepSlabs_); }
	std::size_t FixedPool::Release() { return FreeEmpty(0); }

	PoolStats FixedPool::Stats() const
	{
		PoolStats st;
		st.name = name_;
		st.blockSize = block_;
		st.blocksPerSlab = perSlab_;
		st.slabs = static_cast<std::size_t>(slabs_.Get());
		st.slabBytes = st.slabs * slabBytes_;
		st.used = static_cast<std::size_t>(usedOut_.Get());
		st.capacity = st.slabs * perSlab_;
		st.allocations = allocs_.Get();
		st.frees = frees_.Get();
		st.slabAllocs = slabAllocs_.Get();
		st.slabFrees = slabFrees_.Get();
		st.fallbacks = fallbacks_.Get();
		st.peakUsed = static_cast<std::size_t>(peak_.Get());
		return st;
	}

	SyncPool::SyncPool(std::string name, std::size_t blockSize, std::size_t blocksPerSlab, std::size_t maxSlabs,
					   std::size_t keepSlabs)
		: pool_(std::move(name), blockSize, blocksPerSlab, maxSlabs, keepSlabs, false)
	{
		pools::Add({this, [this] { return Stats(); }, [this] { return Trim(); }});
	}

	SyncPool::~SyncPool() { pools::Remove(this); }

	// ---- Registry ----------------------------------------------------------------------------------------------------------
	namespace pools
	{
		namespace
		{
			std::mutex& Lock()
			{
				static std::mutex m;
				return m;
			}
			std::vector<Entry>& Entries()
			{
				static std::vector<Entry> e;
				return e;
			}
		}

		void Add(Entry e)
		{
			std::lock_guard<std::mutex> l(Lock());
			Entries().push_back(std::move(e));
		}

		void Remove(const void* owner)
		{
			std::lock_guard<std::mutex> l(Lock());
			auto& v = Entries();
			v.erase(std::remove_if(v.begin(), v.end(), [owner](const Entry& e) { return e.owner == owner; }), v.end());
		}

		std::vector<PoolStats> Snapshot()
		{
			std::lock_guard<std::mutex> l(Lock());
			std::vector<PoolStats> out;
			for (const Entry& e : Entries())
				if (e.stats) out.push_back(e.stats());
			return out;
		}

		std::size_t TrimAll()
		{
			RequestSpareTrim();		// The unsynchronized ones, on their own threads
			std::lock_guard<std::mutex> l(Lock());
			std::size_t freed = 0;
			for (const Entry& e : Entries())
				if (e.trim) freed += e.trim();
			return freed;
		}
	}
}
