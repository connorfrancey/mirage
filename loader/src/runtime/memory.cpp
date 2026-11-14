// Counting replacements of the global operator new / delete for this DLL (static CRT: they cover all of the menu's
// code and its libraries, and nothing of the game's). The size freed is the block's own (_msize), so the count stays
// right whichever delete form is used. Aligned new / delete keep the CRT's versions (and aren't counted).
//
// Every allocation also feeds the memory manager's instruments (runtime/mem/heap.h: counters by size class and tag,
// sampled timing, large allocations, the sampling profiler). They never allocate and never fail an allocation: no
// header is added to blocks, so memory stays plain CRT memory whatever frees it.

#include "runtime/memory.h"

#include "runtime/mem/heap.h"

#include <cstdlib>
#include <malloc.h>
#include <new>

namespace
{
	thread_local std::int64_t t_tally = 0;		// This thread's net bytes while a Tally lives
	thread_local int t_tallies = 0;

	void* Allocate(std::size_t n) noexcept
	{
		const bool timed = mirage::mem::TimeThisCall();
		const double t0 = timed ? mirage::mem::NowNs() : 0.0;
		void* p = std::malloc(n ? n : 1);
		const double ns = timed ? mirage::mem::NowNs() - t0 : -1.0;
		if (p) {
			const auto size = _msize(p);
			mirage::mem::OnAllocated(p, size, ns);
			if (t_tallies) t_tally += static_cast<std::int64_t>(size);
		}
		return p;
	}

	void Free(void* p) noexcept
	{
		if (!p) return;
		const auto size = _msize(p);
		if (t_tallies) t_tally -= static_cast<std::int64_t>(size);
		const bool timed = mirage::mem::TimeThisCall();
		mirage::mem::OnFreeing(p, size, -1.0);
		if (!timed) {
			std::free(p);
			return;
		}
		const double t0 = mirage::mem::NowNs();
		std::free(p);
		mirage::mem::OnFreeing(nullptr, 0, mirage::mem::NowNs() - t0);		// Only the timing (nullptr: no block)
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

std::int64_t mirage::memory::LiveBytes() { return mirage::mem::LiveHeapBytes(); }
std::int64_t mirage::memory::TotalAllocations() { return static_cast<std::int64_t>(mirage::mem::HeapAllocationCount()); }
