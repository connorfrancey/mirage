#pragma once
// The whole process's memory, read with queries that take no lock the game could be holding: GetProcessMemoryInfo,
// GlobalMemoryStatusEx and VirtualQuery (never HeapWalk or a heap lock: the game's heaps are left alone). The address
// space walk can be sliced and optionally retains region metadata for the profiler.
#include "runtime/mem/analysis.h"

#include <cstddef>
#include <cstdint>
#include <vector>

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
		bool countersOk = false, limitsOk = false;
	};
	ProcessSample ReadProcess();

	struct AddressSpace
	{
		Fragmentation free;
		std::uint64_t begin = 0, end = 0; // Exclusive end can be 4 GiB in an x86 process.
		std::uint64_t largestAlignedFree = 0;
		std::uint64_t committed = 0, reserved = 0;		// Not free
		std::uint64_t image = 0, mapped = 0, privateCommitted = 0;
		std::size_t regions = 0;
		double walkMs = 0.0;							// Time the walk took (all its slices)
		bool complete = false;
		std::uint32_t queryError = 0;
	};

	struct ProcessRegion
	{
		std::uint64_t begin, end;
		std::uintptr_t allocationBase;
		std::uint32_t state, type, protection;
		std::uint64_t Size() const { return end - begin; }
	};

	inline constexpr std::size_t kMaxProfileRegions = 32768;
	const ProcessRegion* FindProcessRegion(const std::vector<ProcessRegion>& regions, std::uint64_t address);

	// Step returns true at the end of an attempt. Check Result().complete for a successful walk.
	class AddressSpaceWalker
	{
	public:
		// Pass the same pre-reserved buffer to each slice. Totals include regions beyond its capacity.
		bool Step(double budgetMs, std::vector<ProcessRegion>* regions = nullptr);
		const AddressSpace& Result() const { return result_; }
		bool HasResult() const { return result_.complete; }
	private:
		std::uint64_t cursor_ = 0;
		AddressSpace current_;
		AddressSpace result_;
		bool walking_ = false;
	};
}
