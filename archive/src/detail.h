#pragma once

#include <mirage/mpaf.h>

#include <array>
#include <limits>

namespace mirage::mpaf::detail
{
	inline constexpr std::string_view Magic = "MIRAGEAR";
	inline constexpr std::uint64_t MaxArchiveSize = std::numeric_limits<std::uint32_t>::max();
	inline constexpr std::size_t ChunkSize = 64 * 1024;

	[[noreturn]] void Fail(ErrorCode code, std::string message);

	template<typename UInt>
	UInt Load(Bytes bytes, std::size_t offset) noexcept
	{
		std::uint64_t value = 0;
		for (std::size_t i = 0; i < sizeof(UInt); ++i)
			value |= std::to_integer<std::uint64_t>(bytes[offset + i]) << (8 * i);
		return static_cast<UInt>(value);
	}

	template<typename UInt>
	void Save(std::span<std::byte> bytes, std::size_t offset, UInt value) noexcept
	{
		for (std::size_t i = 0; i < sizeof(UInt); ++i)
			bytes[offset + i] = static_cast<std::byte>(static_cast<std::uint64_t>(value) >> (8 * i) & 0xff);
	}

	inline Bytes AsBytes(std::string_view text) noexcept
	{
		return std::as_bytes(std::span(text.data(), text.size()));
	}

	inline int ComparePaths(std::string_view left, std::string_view right) noexcept
	{
		// string_view::compare orders bytes as unsigned char.
		return left.compare(right);
	}

	std::uint64_t Add(std::uint64_t left, std::uint64_t right, std::uint64_t limit, ErrorCode code);
	ArchiveHeader DecodeHeader(Bytes bytes, Limits limits);
	std::array<std::byte, HeaderSize> EncodeHeader(const ArchiveHeader& header);
	std::uint32_t UpdateCrc(std::uint32_t crc, Bytes bytes) noexcept;
	void ReadExact(std::istream& input, std::span<std::byte> data);
	void WriteStream(std::ostream& output, Bytes data);
	void WriteNewFile(const std::filesystem::path& path,
		const std::function<void(const std::function<void(Bytes)>&)>& write);
	std::filesystem::file_type FileType(const std::filesystem::path& path);
	void CheckParents(const std::filesystem::path& path);
	void CreateExtractionRoot(const std::filesystem::path& path);
	std::filesystem::path FromUtf8(std::string_view path);
	std::string ToUtf8(const std::filesystem::path& path);
}
