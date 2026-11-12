#include "runtime/mem/arena.h"

#include "runtime/mem/pool.h"		// TrimEpoch

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <thread>

namespace mirage::mem
{
	struct Arena::Chunk
	{
		Chunk* prev = nullptr;
		std::size_t capacity = 0;		// Bytes after the header
		std::size_t used = 0;
		char* Data() { return reinterpret_cast<char*>(this) + Header(); }
		static constexpr std::size_t Header() { return (sizeof(Chunk) + alignof(std::max_align_t) - 1) / alignof(std::max_align_t) * alignof(std::max_align_t); }
	};

	Arena::Arena(std::string name, std::size_t chunkSize, std::size_t keepBytes)
		: name_(std::move(name)), chunkSize_(std::max<std::size_t>(chunkSize, 4096)), keepBytes_(keepBytes)
	{
		stats_.name = name_;
		epoch_ = TrimEpoch();
	}

	Arena::~Arena()
	{
		Reset();
		Trim();
		keepBytes_ = 0;
		while (spare_) {
			Chunk* next = spare_->prev;
			std::free(spare_);
			spare_ = next;
		}
	}

	Arena::Chunk* Arena::NewChunk(std::size_t minBytes)
	{
		const bool oversized = minBytes > chunkSize_;
		if (!oversized) {
			// A spare chunk of the normal size, if there is one
			if (spare_) {
				Chunk* c = spare_;
				spare_ = c->prev;
				spareBytes_ -= c->capacity;
				c->prev = nullptr;
				c->used = 0;
				return c;
			}
		}
		const std::size_t capacity = oversized ? minBytes : chunkSize_;
		void* mem = std::malloc(Chunk::Header() + capacity);
		if (!mem) {
			++stats_.failures;
			return nullptr;
		}
		auto* c = ::new (mem) Chunk();
		c->capacity = capacity;
		++stats_.chunks;
		stats_.chunkBytes += capacity;
		++stats_.chunkAllocs;
		if (oversized) ++stats_.oversized;
		return c;
	}

	void Arena::Retire(Chunk* c)
	{
		// Normal chunks are kept for reuse up to keepBytes; oversized ones always go
		if (c->capacity == chunkSize_ && spareBytes_ + c->capacity <= keepBytes_) {
			c->prev = spare_;
			c->used = 0;
			spare_ = c;
			spareBytes_ += c->capacity;
			return;
		}
		--stats_.chunks;
		stats_.chunkBytes -= c->capacity;
		++stats_.chunkFrees;
		c->~Chunk();
		std::free(c);
	}

	void* Arena::Allocate(std::size_t bytes, std::size_t align)
	{
		if (align == 0 || (align & (align - 1))) align = alignof(std::max_align_t);
		if (bytes == 0) bytes = 1;
		auto fit = [&](Chunk* c) -> void* {
			const auto base = reinterpret_cast<std::uintptr_t>(c->Data());
			const std::uintptr_t at = (base + c->used + align - 1) & ~(static_cast<std::uintptr_t>(align) - 1);
			const std::size_t end = static_cast<std::size_t>(at - base) + bytes;
			if (end > c->capacity) return nullptr;
			total_ += end - c->used;
			c->used = end;
			return reinterpret_cast<void*>(at);
		};
		void* p = current_ ? fit(current_) : nullptr;
		if (!p) {
			Chunk* c = NewChunk(bytes + align);
			if (!c) return nullptr;
			c->prev = current_;
			current_ = c;
			p = fit(c);
		}
		++stats_.allocations;
		stats_.peak = std::max(stats_.peak, total_);
		return p;
	}

	Arena::Marker Arena::Mark() const { return {current_, current_ ? current_->used : 0, total_}; }

	void Arena::Rewind(const Marker& m)
	{
		while (current_ && current_ != m.chunk) {
			Chunk* c = current_;
			current_ = c->prev;
			Retire(c);
		}
		if (current_) current_->used = m.used;
		total_ = current_ ? m.total : 0;
	}

	void Arena::Reset() { Rewind({}); }

	std::size_t Arena::Trim()
	{
		std::size_t freed = 0;
		while (spare_) {
			Chunk* c = spare_;
			spare_ = c->prev;
			spareBytes_ -= c->capacity;
			freed += c->capacity;
			--stats_.chunks;
			stats_.chunkBytes -= c->capacity;
			++stats_.chunkFrees;
			c->~Chunk();
			std::free(c);
		}
		return freed;
	}

	void Arena::CheckEpoch()
	{
		const std::uint32_t e = TrimEpoch();
		if (e != epoch_) {
			epoch_ = e;
			Trim();
		}
	}

	ArenaStats Arena::Stats() const
	{
		ArenaStats s = stats_;
		s.used = total_;
		s.spareBytes = spareBytes_;
		return s;
	}

	// ---- Thread scratch ----------------------------------------------------------------------------------------------------
	namespace
	{
		struct ScratchRegistry
		{
			std::mutex lock;
			std::vector<std::pair<const void*, ArenaStats>> arenas;		// Last published stats of each thread's arena
		};
		ScratchRegistry& Registry()
		{
			static ScratchRegistry* r = new ScratchRegistry();		// Never destroyed: threads may end after statics
			return *r;
		}

		struct ThreadArena
		{
			Arena arena;
			std::uint32_t scopes = 0;
			ThreadArena() : arena("Scratch (thread " + std::to_string(GetId()) + ")", 64 * 1024, 256 * 1024)
			{
				std::lock_guard<std::mutex> l(Registry().lock);
				Registry().arenas.emplace_back(this, arena.Stats());
			}
			~ThreadArena()
			{
				std::lock_guard<std::mutex> l(Registry().lock);
				auto& v = Registry().arenas;
				v.erase(std::remove_if(v.begin(), v.end(), [this](const auto& e) { return e.first == this; }), v.end());
			}
			void Publish()
			{
				std::lock_guard<std::mutex> l(Registry().lock);
				for (auto& e : Registry().arenas)
					if (e.first == this) e.second = arena.Stats();
			}
			static unsigned long GetId()
			{
				const auto id = std::this_thread::get_id();
				return static_cast<unsigned long>(std::hash<std::thread::id>{}(id) & 0xFFFFFFFF);
			}
		};
		thread_local ThreadArena* t_arena = nullptr;
		thread_local struct ArenaOwner {
			~ArenaOwner() { delete t_arena; t_arena = nullptr; }
		} t_owner;
	}

	Arena& ThreadScratch()
	{
		if (!t_arena) {
			(void)&t_owner;			// Makes sure the owner exists to free it at thread exit
			t_arena = new ThreadArena();
		}
		return t_arena->arena;
	}

	Scratch::Scratch() : arena_(ThreadScratch()), mark_(arena_.Mark()) { arena_.CheckEpoch(); }

	Scratch::~Scratch()
	{
		arena_.Rewind(mark_);
		if (heap_) {
			for (void* p : *heap_) _aligned_free(p);
			delete heap_;
		}
		if (t_arena && (++t_arena->scopes & 63) == 0) t_arena->Publish();		// Diagnostics, now and then
	}

	void* Scratch::Allocate(std::size_t bytes, std::size_t align)
	{
		if (void* p = arena_.Allocate(bytes, align)) return p;
		// No chunk: the heap, freed with the scope
		if (!heap_) heap_ = new std::vector<void*>();
		void* p = _aligned_malloc(bytes ? bytes : 1, std::max<std::size_t>(align, alignof(void*)));
		if (!p) throw std::bad_alloc();
		heap_->push_back(p);
		return p;
	}

	std::vector<ArenaStats> ScratchSnapshot()
	{
		std::lock_guard<std::mutex> l(Registry().lock);
		std::vector<ArenaStats> out;
		for (const auto& e : Registry().arenas) out.push_back(e.second);
		return out;
	}

	// ---- Persistent --------------------------------------------------------------------------------------------------------
	namespace
	{
		struct PersistentArena
		{
			std::mutex lock;
			Arena arena{"Persistent", 256 * 1024, 0};
		};
		PersistentArena& Persist()
		{
			static PersistentArena p;
			return p;
		}
	}

	void* PersistentAllocate(std::size_t bytes, std::size_t align)
	{
		PersistentArena& p = Persist();
		std::lock_guard<std::mutex> l(p.lock);
		return p.arena.Allocate(bytes, align);
	}

	const char* PersistentString(const char* text, std::size_t length)
	{
		auto* out = static_cast<char*>(PersistentAllocate(length + 1, 1));
		if (!out) return nullptr;
		std::memcpy(out, text, length);
		out[length] = '\0';
		return out;
	}

	ArenaStats PersistentStats()
	{
		PersistentArena& p = Persist();
		std::lock_guard<std::mutex> l(p.lock);
		return p.arena.Stats();
	}
}
