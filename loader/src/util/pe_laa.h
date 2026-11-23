#pragma once
// Large Address Aware (the "4GB patch"): a flag in an executable's PE header. Without it a 32-bit game gets 2 GB of
// address space; with it, on 64-bit Windows, 4 GB. Mirror's Edge ships without it, and its address space (not the
// machine's RAM) is what runs out first once mods add content. The installer sets it on MirrorsEdge.exe (a backup kept,
// put back on uninstall); the loader reads it at startup to say how much space the game got.
//
// Works on the file's bytes, so it can be tested without a file: no Windows headers needed.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace pe_laa
{
	inline constexpr std::uint16_t kLargeAddressAware = 0x0020;	// IMAGE_FILE_LARGE_ADDRESS_AWARE

	namespace detail
	{
		inline std::uint32_t Read32(std::span<const std::uint8_t> b, std::size_t at)
		{
			std::uint32_t v;
			std::memcpy(&v, b.data() + at, 4);
			return v;
		}
		inline std::uint16_t Read16(std::span<const std::uint8_t> b, std::size_t at)
		{
			std::uint16_t v;
			std::memcpy(&v, b.data() + at, 2);
			return v;
		}
		// The NT headers' offset ("PE\0\0"), or 0 when the bytes aren't a PE image
		inline std::size_t NtHeaders(std::span<const std::uint8_t> b)
		{
			if (b.size() < 0x40 || b[0] != 'M' || b[1] != 'Z') return 0;
			const std::uint32_t nt = Read32(b, 0x3C);
			if (nt < 0x40 || static_cast<std::size_t>(nt) + 24 + 68 > b.size()) return 0;
			if (std::memcmp(b.data() + nt, "PE\0\0", 4) != 0) return 0;
			return nt;
		}
	}

	// The image's header flag; false for bytes that aren't a PE image
	inline bool IsLargeAddressAware(std::span<const std::uint8_t> image)
	{
		const std::size_t nt = detail::NtHeaders(image);
		return nt && (detail::Read16(image, nt + 4 + 18) & kLargeAddressAware) != 0;
	}

	// The PE checksum (what CheckSumMappedFile computes): the file's 16-bit words summed with carry, the checksum
	// field itself counted as zero, plus the file's length
	inline std::uint32_t Checksum(std::span<const std::uint8_t> image, std::size_t checksumAt)
	{
		std::uint64_t sum = 0;
		for (std::size_t i = 0; i + 1 < image.size(); i += 2) {
			if (i == checksumAt || i == checksumAt + 2) continue;
			sum += static_cast<std::uint64_t>(image[i]) | static_cast<std::uint64_t>(image[i + 1]) << 8;
			sum = (sum & 0xFFFF) + (sum >> 16);
		}
		if (image.size() % 2) sum += image.back();
		sum = (sum & 0xFFFF) + (sum >> 16);
		sum = (sum & 0xFFFF) + (sum >> 16);
		return static_cast<std::uint32_t>(sum + image.size());
	}

	// Sets or clears the flag and updates the header's checksum. False for bytes that aren't a PE image.
	inline bool SetLargeAddressAware(std::span<std::uint8_t> image, bool on)
	{
		const std::size_t nt = detail::NtHeaders(image);
		if (!nt) return false;
		const std::size_t flagsAt = nt + 4 + 18;			// IMAGE_FILE_HEADER::Characteristics
		std::uint16_t flags = detail::Read16(image, flagsAt);
		flags = on ? flags | kLargeAddressAware : flags & ~kLargeAddressAware;
		std::memcpy(image.data() + flagsAt, &flags, 2);
		const std::size_t checksumAt = nt + 24 + 64;		// IMAGE_OPTIONAL_HEADER::CheckSum (same offset in PE32 and PE32+)
		const std::uint32_t sum = Checksum(image, checksumAt);
		std::memcpy(image.data() + checksumAt, &sum, 4);
		return true;
	}
}
