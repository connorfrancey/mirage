#pragma once
#include <cstddef>
#include <cstdint>

// The mod manager's window (features/misc/mod_manager.cpp), for the self-test: frames it was drawn, and the mods it listed.
namespace mod_manager
{
	std::uint64_t Frames();
	std::size_t Rows();
}
