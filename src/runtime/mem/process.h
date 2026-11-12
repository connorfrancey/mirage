#pragma once
// The whole process's memory, read with queries that take no lock the game could be holding: GetProcessMemoryInfo,
// GlobalMemoryStatusEx and VirtualQuery (never HeapWalk or a heap lock: the game's heaps are left alone). The address
// space walk is incremental, so it fits the cache scheduler's time slice; a full walk is a few thousand regions.
#include "runtime/mem/analysis.h"

#include <cstddef>
#include <cstdint>

namespace mirage::mem
{
	struct ProcessSample
	{
		std::uint64_t workingSet = 0, peakWorkingSet = 0;
		std::uint64_t privateBytes = 0, peakPrivate = 0;		// Commit charge of the process
		std::uint64_t pageFaults = 0;
		std::uint64_t physicalAvailable = 0, physicalTotal = 0;
		std::uint64_t commitAvailable = 0, commitLimit = 0;		// The system's
		std::uint64_t addressTotal = 0, addressAvailable = 0;	// This process's user address space
		std::uint32_t memoryLoad = 0;							// Percent of physical memory in use
		bool ok = false;
	};
	ProcessSample ReadProcess();

	struct AddressSpace
	{
		Fragmentation free;
		std::uint64_t committed = 0, reserved = 0;		// Not free
		std::uint64_t image = 0, mapped = 0, privateCommitted = 0;
		std::size_t regions = 0;
		double walkMs = 0.0;							// Time the walk took (all its slices)
		bool complete = false;
	};

	// Walks the address space a bounded piece at a time; Done() after Step returned true, then Result() is the walk
	class AddressSpaceWalker
	{
	public:
		// Returns true when a full walk finished in this step (the result is then fresh)
		bool Step(double budgetMs);
		const AddressSpace& Result() const { return result_; }
		bool HasResult() const { return result_.complete; }
	private:
		std::uintptr_t cursor_ = 0;
		AddressSpace current_;
		AddressSpace result_;
		bool walking_ = false;
	};
}
