#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mirage::mpaf
{
	using Bytes = std::span<const std::byte>;
	inline constexpr std::size_t HeaderSize = 64;
	inline constexpr std::size_t EntrySize = 64;
	inline constexpr std::size_t MaxPathSize = 4096;
	inline constexpr std::string_view ManifestPath = "manifest.yaml";

	enum class EntryType : std::uint16_t { File, Manifest, Directory };
	enum class ErrorCode { Format, Version, Limit, Path, Duplicate, Missing, Checksum, Io, Destination };

	class ArchiveError : public std::runtime_error
	{
	public:
		ArchiveError(ErrorCode code, std::string message);
		[[nodiscard]] ErrorCode Code() const noexcept { return code_; }
	private:
		ErrorCode code_;
	};

	/// Allocation and input limits. The wire format always caps the archive at UINT32_MAX.
	struct Limits
	{
		std::uint64_t archiveSize = 256ull * 1024 * 1024;
		std::uint32_t entryCount = 100'000;
		std::uint32_t stringSize = 16 * 1024 * 1024;
	};

	struct ArchiveHeader
	{
		std::uint16_t major = 1, minor = 0;
		std::uint32_t entryCount = 0;
		std::uint64_t entryTableOffset = HeaderSize;
		std::uint64_t stringTableOffset = HeaderSize;
		std::uint64_t stringTableSize = 0;
		std::uint64_t dataOffset = HeaderSize;
		std::uint64_t archiveSize = HeaderSize;
	};

	struct ArchiveHash
	{
		std::uint32_t crc32 = 0;
		std::uint64_t xxh3 = 0;
		bool operator==(const ArchiveHash&) const = default;
		[[nodiscard]] static ArchiveHash Compute(Bytes bytes) noexcept;
	};

	struct ArchiveEntry
	{
		std::string_view path;
		std::uint64_t dataOffset = 0;
		std::uint64_t dataSize = 0;
		EntryType type = EntryType::File;
		ArchiveHash hash;
	};

	class ArchiveValidator
	{
	public:
		/// Validate a portable UTF-8 relative path. Throws ArchiveError on failure.
		static void ValidatePath(std::string_view path);
		/// Validate the structure and every file checksum.
		static void Validate(Bytes bytes, Limits limits = {});
	};

	/// Index of a decompressed MPAF payload. Metadata is validated at open, hashes on read.
	class ArchiveReader
	{
	public:
		/// Borrow immutable bytes. The buffer must outlive the reader and any returned views.
		[[nodiscard]] static ArchiveReader Open(Bytes bytes, Limits limits = {});
		/// Take ownership of the decompression buffer without copying file data.
		[[nodiscard]] static ArchiveReader Open(std::vector<std::byte>&& bytes, Limits limits = {});
		/// Read one complete payload from the current stream position through EOF.
		[[nodiscard]] static ArchiveReader Open(std::istream& input, Limits limits = {});
		[[nodiscard]] static ArchiveReader Open(const std::filesystem::path& path, Limits limits = {});

		[[nodiscard]] const ArchiveHeader& Header() const noexcept { return header_; }
		[[nodiscard]] std::span<const ArchiveEntry> Entries() const noexcept { return entries_; }
		[[nodiscard]] const ArchiveEntry* Find(std::string_view path) const noexcept;
		/// Return a verified zero-copy view of the file bytes. Directory paths are rejected.
		[[nodiscard]] Bytes Read(std::string_view path) const;
		[[nodiscard]] Bytes Manifest() const { return Read(ManifestPath); }
		void Verify() const;
		void Extract(std::string_view path, std::ostream& output) const;
		/// Write a new file under a trusted destination parent. Existing paths are rejected.
		void Extract(std::string_view path, const std::filesystem::path& output) const;
		/// Extract into a new directory under a trusted parent. Never merges with an existing tree.
		void ExtractAll(const std::filesystem::path& root, std::size_t maxDirectories = 100'000) const;

	private:
		ArchiveReader() = default;
		[[nodiscard]] Bytes ReadEntry(const ArchiveEntry& entry) const;

		std::shared_ptr<const std::vector<std::byte>> owned_;
		Bytes bytes_;
		ArchiveHeader header_;
		std::vector<ArchiveEntry> entries_;
	};

	/// Builds a canonical payload. Filesystem sources are streamed during serialization.
	class ArchiveWriter
	{
	public:
		explicit ArchiveWriter(Limits limits = {}) : limits_(limits) {}
		[[nodiscard]] static ArchiveWriter FromDirectory(const std::filesystem::path& root, Limits limits = {});
		void Add(std::string_view path, Bytes data);
		/// Read exactly size bytes from input at its current position.
		void Add(std::string_view path, std::istream& input, std::uint64_t size);
		void AddFile(std::string_view path, const std::filesystem::path& source);
		void AddDirectory(std::string_view path);
		void Replace(std::string_view path, Bytes data);
		void ReplaceFile(std::string_view path, const std::filesystem::path& source);
		bool Remove(std::string_view path);

		[[nodiscard]] std::vector<std::byte> Serialize() const;
		void Write(std::ostream& output) const;
		/// Write a new payload file. Existing paths are rejected.
		void Write(const std::filesystem::path& output) const;

	private:
		using Source = std::variant<std::monostate, std::vector<std::byte>, std::filesystem::path>;
		void CheckSlot(std::string_view path, bool replace, std::uint64_t memorySize) const;
		void Store(std::string_view path, Source source, bool replace);
		void WriteTo(const std::function<void(Bytes)>& sink) const;

		Limits limits_;
		std::uint64_t memorySize_ = 0;
		std::uint64_t stringSize_ = 0;
		std::map<std::string, Source, std::less<>> sources_;
	};
}
