#pragma once
// Fast non-cryptographic hashing (XXH3, header only). For hash keys, content fingerprints and change detection in
// memory or caches. Not for file formats that already store another hash (.merp keeps FNV-1a).
#include <xxhash.h>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace hash
{
	inline std::uint64_t Bytes(const void* data, std::size_t size, std::uint64_t seed = 0)
	{
		return XXH3_64bits_withSeed(data, size, seed);
	}
	inline std::uint64_t Text(std::string_view s, std::uint64_t seed = 0) { return Bytes(s.data(), s.size(), seed); }

	// For data that arrives in pieces (a file read in chunks): the same result as hashing it in one go
	class Stream
	{
	public:
		explicit Stream(std::uint64_t seed = 0) { XXH3_64bits_reset_withSeed(&state_, seed); }
		Stream& Add(const void* data, std::size_t size)
		{
			XXH3_64bits_update(&state_, data, size);
			return *this;
		}
		Stream& Add(std::string_view s) { return Add(s.data(), s.size()); }
		template <class T>
		Stream& Value(const T& v)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			return Add(&v, sizeof v);
		}
		std::uint64_t Digest() const { return XXH3_64bits_digest(&state_); }
	private:
		XXH3_state_t state_;
	};

	// Combine two hashes (order matters)
	inline std::uint64_t Combine(std::uint64_t a, std::uint64_t b) { return a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2)); }
}
