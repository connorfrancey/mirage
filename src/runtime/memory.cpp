// Counting replacements of the global operator new / delete for this DLL (static CRT: they cover all of the menu's
// code and its libraries, and nothing of the game's). The size freed is the block's own (_msize), so the count stays
// right whichever delete form is used. Aligned new / delete keep the CRT's versions (and aren't counted).

#include "runtime/memory.h"

#include <atomic>
#include <cstdlib>
#include <malloc.h>
#include <new>

namespace
{
	std::atomic<std::int64_t> g_live{0};
	std::atomic<std::int64_t> g_count{0};
	thread_local std::int64_t t_tally = 0;		// This thread's net bytes while a Tally lives
	thread_local int t_tallies = 0;

	void* Allocate(std::size_t n) noexcept
	{
		void* p = std::malloc(n ? n : 1);
		if (p) {
			const auto size = static_cast<std::int64_t>(_msize(p));
			g_live.fetch_add(size, std::memory_order_relaxed);
			g_count.fetch_add(1, std::memory_order_relaxed);
			if (t_tallies) t_tally += size;
		}
		return p;
	}

	void Free(void* p) noexcept
	{
		if (!p) return;
		const auto size = static_cast<std::int64_t>(_msize(p));
		g_live.fetch_sub(size, std::memory_order_relaxed);
		if (t_tallies) t_tally -= size;
		std::free(p);
	}
}

void* operator new(std::size_t n)
{
	if (void* p = Allocate(n)) return p;
	throw std::bad_alloc();
}
void* operator new[](std::size_t n)
{
	if (void* p = Allocate(n)) return p;
	throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return Allocate(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return Allocate(n); }
void operator delete(void* p) noexcept { Free(p); }
void operator delete[](void* p) noexcept { Free(p); }
void operator delete(void* p, std::size_t) noexcept { Free(p); }
void operator delete[](void* p, std::size_t) noexcept { Free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { Free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { Free(p); }

mirage::memory::Tally::Tally() : start_(t_tally) { ++t_tallies; }
mirage::memory::Tally::~Tally() { --t_tallies; }
std::int64_t mirage::memory::Tally::Bytes() const { return t_tally - start_; }

std::int64_t mirage::memory::LiveBytes() { return g_live.load(std::memory_order_relaxed); }
std::int64_t mirage::memory::TotalAllocations() { return g_count.load(std::memory_order_relaxed); }
