#pragma once
// Whether the running game has the "4GB patch" (Large Address Aware, util/pe_laa.h) and how much address space it got:
// 2 GB without it, 4 GB with it on 64-bit Windows. Logged at startup; running out of address space is how a 32-bit game
// with many mods crashes. Windows only.
#include "util/pe_laa.h"

#include <Windows.h>

#include <cstdint>
#include <span>

namespace crashhandler::analyzer
{
	struct AddressSpace
	{
		bool largeAddressAware = false;		// MirrorsEdge.exe's header flag
		std::uint64_t totalMb = 0;			// The process's user address space
	};

	inline AddressSpace GameAddressSpace()
	{
		AddressSpace a;
		// The exe's headers, mapped at its base: the first page holds them
		const auto* base = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
		if (base) a.largeAddressAware = pe_laa::IsLargeAddressAware(std::span(base, 4096));
		MEMORYSTATUSEX ms{sizeof ms};
		if (GlobalMemoryStatusEx(&ms)) a.totalMb = ms.ullTotalVirtual / (1024 * 1024);
		return a;
	}
}
