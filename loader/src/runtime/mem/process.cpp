#include "runtime/mem/process.h"

#include "runtime/mem/heap.h"		// NowNs

#include <Windows.h>
#include <Psapi.h>

#include <utility>

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
			s.ok = true;
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
		} else {
			s.ok = false;
		}
		return s;
	}

	bool AddressSpaceWalker::Step(double budgetMs)
	{
		static const auto [low, high] = [] {
			SYSTEM_INFO si{};
			GetSystemInfo(&si);
			return std::pair{reinterpret_cast<std::uintptr_t>(si.lpMinimumApplicationAddress),
							 reinterpret_cast<std::uintptr_t>(si.lpMaximumApplicationAddress)};
		}();
		const double start = NowNs();
		if (!walking_) {
			current_ = AddressSpace{};
			cursor_ = low;
			walking_ = true;
		}
		int sinceCheck = 0;
		while (cursor_ < high) {
			MEMORY_BASIC_INFORMATION mbi{};
			if (!VirtualQuery(reinterpret_cast<void*>(cursor_), &mbi, sizeof mbi) || mbi.RegionSize == 0) {
				cursor_ = high;		// Can't go on: the walk ends here (what was seen counts)
				break;
			}
			const std::uint64_t size = mbi.RegionSize;
			++current_.regions;
			if (mbi.State == MEM_FREE) {
				current_.free.AddFree(size);
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
			const std::uintptr_t next = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
			if (next <= cursor_) { cursor_ = high; break; }
			cursor_ = next;
			if (++sinceCheck >= 64) {
				sinceCheck = 0;
				const double spent = (NowNs() - start) / 1e6;
				if (spent >= budgetMs) {
					current_.walkMs += spent;
					return false;
				}
			}
		}
		current_.walkMs += (NowNs() - start) / 1e6;
		current_.complete = true;
		result_ = current_;
		walking_ = false;
		return true;
	}
}
