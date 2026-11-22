#include "detail.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <utility>

namespace fs = std::filesystem;

namespace mirage::mpaf
{
	namespace
	{
		using namespace detail;

		ArchiveEntry DecodeEntry(Bytes record, Bytes strings, std::uint64_t& pathCursor, std::uint64_t& dataCursor,
			std::uint64_t archiveSize)
		{
			const auto pathOffset = Load<std::uint32_t>(record, 0);
			const auto pathSize = Load<std::uint32_t>(record, 4);
			if (pathOffset != pathCursor)
				Fail(ErrorCode::Format, "Noncontiguous MPAF string table");
			pathCursor = Add(pathCursor, pathSize, strings.size(), ErrorCode::Format);
			const auto* pathData = reinterpret_cast<const char*>(strings.data() + pathOffset);
			const std::string_view path(pathData, pathSize);
			ArchiveValidator::ValidatePath(path);

			const auto offset = Load<std::uint64_t>(record, 8);
			const auto size = Load<std::uint64_t>(record, 16);
			const auto type = static_cast<EntryType>(Load<std::uint16_t>(record, 36));
			const ArchiveHash hash{Load<std::uint32_t>(record, 32), Load<std::uint64_t>(record, 40)};
			if (offset != dataCursor || size != Load<std::uint64_t>(record, 24) ||
				Load<std::uint16_t>(record, 38) != 0 || Load<std::uint64_t>(record, 48) != 0 ||
				Load<std::uint64_t>(record, 56) != 0)
				Fail(ErrorCode::Format, "Invalid MPAF entry offset, size, flags or reserved fields");
			switch (type)
			{
			case EntryType::Directory:
				if (size != 0 || hash != ArchiveHash{})
					Fail(ErrorCode::Format, "MPAF directory must have zero size and hashes");
				break;
			case EntryType::Manifest:
			case EntryType::File:
				break;
			default:
				Fail(ErrorCode::Format, "Unknown MPAF entry type");
			}
			if ((type == EntryType::Manifest) != (path == ManifestPath))
				Fail(ErrorCode::Format, "MPAF manifest type is reserved for manifest.yaml");
			dataCursor = Add(dataCursor, size, archiveSize, ErrorCode::Format);
			return {path, offset, size, type, hash};
		}

		void CheckEof(std::istream& input)
		{
			try
			{
				if (input.peek() != std::char_traits<char>::eof())
					Fail(ErrorCode::Format, "Trailing bytes after MPAF payload");
			}
			catch (const std::ios_base::failure&)
			{
				if (!input.eof() || input.bad())
					Fail(ErrorCode::Io, "Could not check the end of the MPAF stream");
			}
			if (input.bad() || !input.eof())
				Fail(ErrorCode::Io, "Could not check the end of the MPAF stream");
		}
	}

	ArchiveReader ArchiveReader::Open(Bytes bytes, Limits limits)
	{
		ArchiveReader reader;
		reader.header_ = DecodeHeader(bytes, limits);
		const auto& header = reader.header_;
		if (bytes.size() != header.archiveSize || header.entryCount == 0)
			Fail(ErrorCode::Format, "MPAF payload has an invalid size or no entries");
		reader.bytes_ = bytes;
		const auto strings = bytes.subspan(static_cast<std::size_t>(header.stringTableOffset),
			static_cast<std::size_t>(header.stringTableSize));
		if (header.entryCount > reader.entries_.max_size())
			Fail(ErrorCode::Limit, "MPAF index exceeds addressable memory");
		reader.entries_.reserve(header.entryCount);
		std::uint64_t pathCursor = 0;
		auto dataCursor = header.dataOffset;
		auto recordOffset = HeaderSize;
		for (std::uint32_t i = 0; i < header.entryCount; ++i, recordOffset += EntrySize)
		{
			const auto entry = DecodeEntry(bytes.subspan(recordOffset, EntrySize), strings, pathCursor, dataCursor,
				header.archiveSize);
			if (!reader.entries_.empty())
			{
				const auto cmp = ComparePaths(reader.entries_.back().path, entry.path);
				if (cmp == 0)
					Fail(ErrorCode::Duplicate, "Duplicate MPAF path");
				if (cmp > 0)
					Fail(ErrorCode::Format, "MPAF paths must be sorted");
			}
			reader.entries_.push_back(entry);
		}
		if (pathCursor != strings.size() || dataCursor != header.archiveSize)
			Fail(ErrorCode::Format, "Unreferenced data in MPAF payload");
		if (!reader.Find(ManifestPath))
			Fail(ErrorCode::Missing, "MPAF requires manifest.yaml");

		std::string prefix;
		for (auto it = reader.entries_.begin(); it != reader.entries_.end(); ++it)
		{
			const auto next = std::next(it);
			if (it->type != EntryType::Directory && next != reader.entries_.end() && next->path.starts_with(it->path))
			{
				prefix.assign(it->path);
				prefix += '/';
				const auto child = std::lower_bound(next, reader.entries_.end(), std::string_view(prefix),
					[](const ArchiveEntry& entry, auto key) { return ComparePaths(entry.path, key) < 0; });
				if (child != reader.entries_.end() && child->path.starts_with(prefix))
					Fail(ErrorCode::Path, "MPAF file cannot be an ancestor of another entry");
			}
		}
		return reader;
	}

	ArchiveReader ArchiveReader::Open(std::vector<std::byte>&& bytes, Limits limits)
	{
		auto reader = Open(Bytes(bytes), limits);
		reader.owned_ = std::make_shared<const std::vector<std::byte>>(std::move(bytes));
		return reader;
	}

	ArchiveReader ArchiveReader::Open(std::istream& input, Limits limits)
	{
		std::array<std::byte, HeaderSize> rawHeader;
		ReadExact(input, rawHeader);
		const auto header = DecodeHeader(rawHeader, limits);
		std::vector<std::byte> bytes(rawHeader.begin(), rawHeader.end());
		std::array<std::byte, ChunkSize> chunk;
		while (bytes.size() < header.archiveSize)
		{
			const auto count = std::min(chunk.size(), static_cast<std::size_t>(header.archiveSize) - bytes.size());
			ReadExact(input, std::span(chunk).first(count));
			// Grow only after the bytes arrive. Never allocate the declared size for a short stream.
			if (bytes.capacity() - bytes.size() < count)
				bytes.reserve(std::min(static_cast<std::size_t>(header.archiveSize),
					std::max(bytes.size() + count, bytes.capacity() + bytes.capacity() / 2)));
			bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + count);
		}
		CheckEof(input);
		return Open(std::move(bytes), limits);
	}

	ArchiveReader ArchiveReader::Open(const fs::path& path, Limits limits)
	{
		if (FileType(path) != fs::file_type::regular)
			Fail(ErrorCode::Io, "MPAF input must be a regular file");
		std::ifstream input(path, std::ios::binary | std::ios::ate);
		const auto size = input.tellg();
		if (size < 0 || !input.seekg(0))
			Fail(ErrorCode::Io, "Could not open MPAF payload file");
		std::array<std::byte, HeaderSize> rawHeader;
		ReadExact(input, rawHeader);
		const auto header = DecodeHeader(rawHeader, limits);
		if (static_cast<std::uint64_t>(size) != header.archiveSize)
			Fail(ErrorCode::Format, "MPAF file length does not match its header");
		std::vector<std::byte> bytes(static_cast<std::size_t>(header.archiveSize));
		std::copy(rawHeader.begin(), rawHeader.end(), bytes.begin());
		ReadExact(input, std::span(bytes).subspan(HeaderSize));
		CheckEof(input);
		return Open(std::move(bytes), limits);
	}

	const ArchiveEntry* ArchiveReader::Find(std::string_view path) const noexcept
	{
		const auto it = std::lower_bound(entries_.begin(), entries_.end(), path, [](const ArchiveEntry& entry, auto key)
			{ return ComparePaths(entry.path, key) < 0; });
		return it != entries_.end() && it->path == path ? &*it : nullptr;
	}

	Bytes ArchiveReader::ReadEntry(const ArchiveEntry& entry) const
	{
		const auto data = bytes_.subspan(static_cast<std::size_t>(entry.dataOffset), static_cast<std::size_t>(entry.dataSize));
		if (ArchiveHash::Compute(data) != entry.hash)
			Fail(ErrorCode::Checksum, "MPAF checksum mismatch: " + std::string(entry.path));
		return data;
	}

	Bytes ArchiveReader::Read(std::string_view path) const
	{
		const auto* entry = Find(path);
		if (!entry || entry->type == EntryType::Directory)
			Fail(ErrorCode::Missing, "MPAF file not found: " + std::string(path));
		return ReadEntry(*entry);
	}

	void ArchiveReader::Verify() const
	{
		for (const auto& entry : entries_)
			if (entry.type != EntryType::Directory)
				(void)ReadEntry(entry);
	}

	void ArchiveValidator::Validate(Bytes bytes, Limits limits)
	{
		ArchiveReader::Open(bytes, limits).Verify();
	}

	void ArchiveReader::Extract(std::string_view path, std::ostream& output) const
	{
		WriteStream(output, Read(path));
	}

	void ArchiveReader::Extract(std::string_view path, const fs::path& output) const
	{
		CheckParents(output);
		const auto data = Read(path);
		WriteNewFile(output, [&](const auto& sink) { sink(data); });
	}

	void ArchiveReader::ExtractAll(const fs::path& destination, std::size_t maxDirectories) const
	{
		auto root = destination;
		if (!root.has_filename() && root != root.root_path())
			root = root.parent_path();
		auto createDirectory = [&](const fs::path& path)
		{
			std::error_code error;
			if (!fs::create_directory(path, error))
				Fail(ErrorCode::Destination, "Could not create a new MPAF extraction directory"
					+ (error ? ": " + error.message() : std::string{}));
		};

		CreateExtractionRoot(root);
		try
		{
			std::size_t directoryCount = 0;
			const auto createChild = [&](std::string_view path)
			{
				if (directoryCount == maxDirectories)
					Fail(ErrorCode::Limit, "MPAF extraction exceeds directory count limit");
				createDirectory(root / FromUtf8(path));
				++directoryCount;
			};
			std::string_view previous;
			for (const auto& entry : entries_)
			{
				auto slash = entry.path.find('/');
				while (slash != std::string_view::npos)
				{
					const auto prefix = entry.path.substr(0, slash);
					// Descendants of an implicit directory are contiguous in sorted entry order.
					const bool inPrevious = previous.size() > slash && previous[slash] == '/' && previous.starts_with(prefix);
					if (!inPrevious && !Find(prefix))
						createChild(prefix);
					slash = entry.path.find('/', slash + 1);
				}
				if (entry.type == EntryType::Directory)
					createChild(entry.path);
				else
				{
					const auto data = ReadEntry(entry);
					WriteNewFile(root / FromUtf8(entry.path), [&](const auto& sink) { sink(data); });
				}
				previous = entry.path;
			}
		}
		catch (...)
		{
			std::error_code ignored;
			fs::remove_all(root, ignored);
			throw;
		}
	}
}
