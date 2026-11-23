#include "runtime/mem/process.h"

#include "runtime/mem/heap.h"		// NowNs

#include <Windows.h>
#include <Psapi.h>

#include <algorithm>

namespace mirage::mem
{
	ProcessSample ReadProcess()
	{
		ProcessSample s;
		PROCESS_MEMORY_COUNTERS_EX pmc{};
		pmc.cb = sizeof pmc;
		if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc)) {
			s.workingSet = pmc.WorkingSetSize;
			s.peakWorkingSet = pmc.PeakWorkingSetSize;
			s.privateBytes = pmc.PrivateUsage;
			s.peakPrivate = pmc.PeakPagefileUsage;
			s.pageFaults = pmc.PageFaultCount;
			s.countersOk = true;
		}
		MEMORYSTATUSEX ms{};
		ms.dwLength = sizeof ms;
		if (GlobalMemoryStatusEx(&ms)) {
			s.physicalAvailable = ms.ullAvailPhys;
			s.physicalTotal = ms.ullTotalPhys;
			s.commitAvailable = ms.ullAvailPageFile;
			s.commitLimit = ms.ullTotalPageFile;
			s.addressTotal = ms.ullTotalVirtual;
			s.addressAvailable = ms.ullAvailVirtual;
			s.memoryLoad = ms.dwMemoryLoad;
			s.limitsOk = true;
		}
		s.ok = s.countersOk && s.limitsOk;
		return s;
	}

	const ProcessRegion* FindProcessRegion(const std::vector<ProcessRegion>& regions, std::uint64_t address)
	{
		const auto it = std::upper_bound(regions.begin(), regions.end(), address,
			[](std::uint64_t point, const ProcessRegion& region) { return point < region.end; });
		return it != regions.end() && it->begin <= address ? &*it : nullptr;
	}

	bool AddressSpaceWalker::Step(double budgetMs, std::vector<ProcessRegion>* regions)
	{
		static const SYSTEM_INFO info = [] {
			SYSTEM_INFO si{};
			GetSystemInfo(&si);
			return si;
		}();
		const double start = NowNs();
		if (!walking_) {
			current_ = AddressSpace{};
			current_.begin = reinterpret_cast<std::uintptr_t>(info.lpMinimumApplicationAddress);
			current_.end = std::uint64_t{reinterpret_cast<std::uintptr_t>(info.lpMaximumApplicationAddress)} + 1;
			cursor_ = current_.begin;
			if (regions) regions->clear();
			walking_ = true;
		}
		int sinceCheck = 0;
		while (cursor_ < current_.end) {
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<void*>(static_cast<std::uintptr_t>(cursor_)), &mbi, sizeof mbi)) {
				current_.queryError = GetLastError();
				break;
			}
			const std::uint64_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
			const std::uint64_t next = std::min(base + mbi.RegionSize, current_.end);
			if (base > cursor_ || next <= cursor_) {
				current_.queryError = ERROR_INVALID_DATA;
				break;
			}
			const std::uint64_t size = next - cursor_;
			const bool free = mbi.State == MEM_FREE;
			++current_.regions;
			if (free) {
				current_.free.AddFree(size);
				const std::uint64_t mask = info.dwAllocationGranularity - 1;
				const std::uint64_t aligned = (cursor_ + mask) & ~mask;
				if (aligned < next) current_.largestAlignedFree = std::max(current_.largestAlignedFree, next - aligned);
			} else {
				if (mbi.State == MEM_COMMIT) {
					current_.committed += size;
					if (mbi.Type == MEM_PRIVATE) current_.privateCommitted += size;
				} else {
					current_.reserved += size;
				}
				if (mbi.Type == MEM_IMAGE) current_.image += size;
				else if (mbi.Type == MEM_MAPPED) current_.mapped += size;
			}
			if (regions && regions->size() < std::min(regions->capacity(), kMaxProfileRegions)) {
				regions->push_back({cursor_, next, free ? 0 : reinterpret_cast<std::uintptr_t>(mbi.AllocationBase),
					mbi.State, free ? 0 : mbi.Type, mbi.State == MEM_COMMIT ? mbi.Protect : 0});
			}
			cursor_ = next;
			if (++sinceCheck >= 64) {
				sinceCheck = 0;
				const double spent = (NowNs() - start) / 1e6;
				if (spent >= budgetMs && cursor_ < current_.end) {
					current_.walkMs += spent;
					return false;
				}
			}
		}
		current_.walkMs += (NowNs() - start) / 1e6;
		current_.complete = cursor_ == current_.end;
		result_ = current_;
		walking_ = false;
		return true;
	}
}
