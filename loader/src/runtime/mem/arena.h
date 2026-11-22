#pragma once
/** @file
 * @ingroup mirage_memory
 * @brief Arena allocation for Mirage-owned scratch and persistent data.
 */
// Arenas: bump allocation from chunks, freed all at once.
//
//   Temporary memory: a Scratch scope on the calling thread's own arena. Everything allocated in it is gone when the
//   scope ends; scopes nest. No locks, no heap call per allocation, nothing left fragmenting the CRT heap.
//
//       mirage::mem::Scratch scratch;
//       auto* names = scratch.Array<const char*>(count);            // trivially destructible types only
//       std::vector<int, mirage::mem::ScratchAllocator<int>> v(scratch.Allocator<int>());
//
//   Long-lived memory: Persistent() is a shared, locked arena that is never rewound: tables and strings made once and
//   kept for the session, packed together instead of spread through the heap among short-lived blocks.
//
// Safe fallbacks: a request larger than a chunk gets a chunk of its own; when no chunk can be had, Allocate returns
// nullptr (Scratch::Allocate falls back to the heap and frees it at the end of the scope, so a scope never fails where
// new wouldn't). Spare chunks are kept up to a cap and given back on RequestSpareTrim (pool.h), by the owning thread.
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace mirage::mem
{
	struct ArenaStats
	{
		std::string name;
		std::size_t chunks = 0, chunkBytes = 0;		// Held now (in use and spare)
		std::size_t used = 0;						// Bytes handed out and not rewound
		std::size_t peak = 0;
		std::size_t spareBytes = 0;
		std::uint64_t allocations = 0, chunkAllocs = 0, chunkFrees = 0, oversized = 0, failures = 0;
	};

	/// @ingroup mirage_memory
	/// @brief Allocate blocks that retire together or rewind to a marker.
	/// @warning Keep use on the owning thread. Rewind and reset invalidate reclaimed pointers.
	class Arena
	{
	public:
		// `chunkSize`: normal chunk; `keepBytes`: spare chunks kept for reuse after a rewind
		explicit Arena(std::string name, std::size_t chunkSize = 64 * 1024, std::size_t keepBytes = 256 * 1024);
		~Arena();
		Arena(const Arena&) = delete;
		Arena& operator=(const Arena&) = delete;

		/// @brief Allocate bytes in the current arena.
		/// @param bytes Number of bytes requested.
		/// @param align Required alignment in bytes.
		/// @return Borrowed storage, or null when allocation fails.
		void* Allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t));
		template <class T>
		T* Array(std::size_t n)
		{
			static_assert(std::is_trivially_destructible_v<T>, "arena memory is never destroyed: trivially destructible types only");
			void* p = Allocate(sizeof(T) * n, alignof(T));
			return p ? ::new (p) T[n]() : nullptr;
		}

		struct Marker
		{
			void* chunk = nullptr;
			std::size_t used = 0;
			std::size_t total = 0;
		};
		Marker Mark() const;
		void Rewind(const Marker& m);		// Everything allocated since the marker is gone
		void Reset();						// Everything
		std::size_t Trim();					// Gives back spare chunks; returns bytes
		void CheckEpoch();					// Trims if RequestSpareTrim was called since the last check
		ArenaStats Stats() const;			// Owner thread (Persistent(): any thread)

	private:
		struct Chunk;
		Chunk* NewChunk(std::size_t minBytes);
		void Retire(Chunk* c);
		std::string name_;
		std::size_t chunkSize_, keepBytes_;
		Chunk* current_ = nullptr;			// Chunks in use, newest first (each points at the one before)
		Chunk* spare_ = nullptr;
		std::size_t total_ = 0;				// Bytes handed out (the arena's "used")
		std::size_t spareBytes_ = 0;
		ArenaStats stats_;
		std::uint32_t epoch_ = 0;
	};

	// The calling thread's scratch arena (made on first use, freed when the thread ends)
	Arena& ThreadScratch();

	template <class T>
	class ScratchAllocator;

	/// @ingroup mirage_memory
	/// @brief Scope that rewinds the calling thread's scratch arena on destruction.
	/// @warning Pointers must stay within the scope. The arena only holds trivially destructible data.
	class Scratch
	{
	public:
		Scratch();
		~Scratch();
		Scratch(const Scratch&) = delete;
		Scratch& operator=(const Scratch&) = delete;

		void* Allocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t));	// Never nullptr (throws bad_alloc like new)
		template <class T>
		T* Array(std::size_t n)
		{
			static_assert(std::is_trivially_destructible_v<T>, "scratch memory is never destroyed: trivially destructible types only");
			return ::new (Allocate(sizeof(T) * n, alignof(T))) T[n]();
		}
		template <class T>
		ScratchAllocator<T> Allocator() { return ScratchAllocator<T>(this); }

	private:
		Arena& arena_;
		Arena::Marker mark_;
		std::vector<void*>* heap_ = nullptr;	// Heap fallbacks, freed at the end of the scope
	};

	// For standard containers inside a Scratch scope (deallocate does nothing: the scope frees it all)
	template <class T>
	class ScratchAllocator
	{
	public:
		using value_type = T;
		explicit ScratchAllocator(Scratch* s) : s_(s) {}
		template <class U>
		ScratchAllocator(const ScratchAllocator<U>& o) : s_(o.s_) {}
		T* allocate(std::size_t n) { return static_cast<T*>(s_->Allocate(sizeof(T) * n, alignof(T))); }
		void deallocate(T*, std::size_t) {}
		template <class U>
		bool operator==(const ScratchAllocator<U>& o) const { return s_ == o.s_; }
		template <class U>
		bool operator!=(const ScratchAllocator<U>& o) const { return s_ != o.s_; }
	private:
		template <class U>
		friend class ScratchAllocator;
		Scratch* s_;
	};

	// The long-lived arena (any thread, locked). Never rewound: only for what lives until the menu unloads.
	void* PersistentAllocate(std::size_t bytes, std::size_t align = alignof(std::max_align_t));
	const char* PersistentString(const char* text, std::size_t length);
	ArenaStats PersistentStats();

	// Every thread's scratch arena, for the diagnostics (sizes as last seen by their threads)
	std::vector<ArenaStats> ScratchSnapshot();
}
