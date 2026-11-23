#pragma once
// Fast non-cryptographic hashing (rapidhash, header only). For hash keys, content fingerprints and change detection in
// memory or caches. Not for file formats that already store another hash (.merp keeps FNV-1a).
#include <rapidhash.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hash
{
	inline std::uint64_t Bytes(const void* data, std::size_t size, std::uint64_t seed = 0)
	{
		return rapidhash_withSeed(data, size, seed);
	}
	inline std::uint64_t Text(std::string_view s, std::uint64_t seed = 0) { return Bytes(s.data(), s.size(), seed); }

	// Combine two hashes (order matters)
	inline std::uint64_t Combine(std::uint64_t a, std::uint64_t b) { return a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2)); }
}
