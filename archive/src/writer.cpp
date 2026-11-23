#include "hash.h"

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

		std::uint64_t GetFileSize(const fs::path& path)
		{
			if (FileType(path) != fs::file_type::regular)
				Fail(ErrorCode::Io, "MPAF source must be a regular file without symlinks or reparse points");
			std::error_code error;
			const auto size = fs::file_size(path, error);
			if (error)
				Fail(ErrorCode::Io, "Could not determine MPAF source file size: " + error.message());
			return size;
		}

		template<typename Sink>
		ArchiveHash StreamFile(const fs::path& path, std::uint64_t size, const Sink& sink)
		{
			if (GetFileSize(path) != size)
				Fail(ErrorCode::Io, "MPAF source file size changed");
			std::ifstream input(path, std::ios::binary);
			if (!input)
				Fail(ErrorCode::Io, "Could not open MPAF source file");
			HashState hash;
			std::array<std::byte, ChunkSize> buffer;
			while (size != 0)
			{
				const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size));
				const auto chunk = std::span(buffer).first(count);
				if (!input.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(count)))
					Fail(ErrorCode::Io, "Could not read MPAF source file");
				hash.Update(chunk);
				sink(chunk);
				size -= count;
			}
			if (input.peek() != std::char_traits<char>::eof() || input.bad() || !input.eof())
				Fail(ErrorCode::Io, "Could not read MPAF source file through EOF");
			return hash.Finish();
		}

		std::array<std::byte, EntrySize> EncodeEntry(const ArchiveEntry& entry, std::uint32_t pathOffset)
		{
			std::array<std::byte, EntrySize> bytes{};
			Save(bytes, 0, pathOffset);
			Save(bytes, 4, static_cast<std::uint32_t>(entry.path.size()));
			Save(bytes, 8, entry.dataOffset);
			Save(bytes, 16, entry.dataSize);
			Save(bytes, 24, entry.dataSize);
			Save(bytes, 32, entry.hash.crc32);
			Save(bytes, 36, static_cast<std::uint16_t>(entry.type));
			Save(bytes, 40, entry.hash.xxh3);
			return bytes;
		}
	}

	ArchiveWriter ArchiveWriter::FromDirectory(const fs::path& input, Limits limits)
	{
		ArchiveWriter writer(limits);
		const auto root = fs::absolute(input);
		CheckParents(root);
		if (FileType(root) != fs::file_type::directory)
			Fail(ErrorCode::Io, "MPAF source must be a directory without symlinks or reparse points");
		try
		{
			for (const auto& entry : fs::recursive_directory_iterator(root))
			{
				const auto path = ToUtf8(entry.path().lexically_relative(root));
				switch (FileType(entry.path()))
				{
				case fs::file_type::directory:
					writer.AddDirectory(path);
					break;
				case fs::file_type::regular:
					writer.AddFile(path, entry.path());
					break;
				default:
					Fail(ErrorCode::Path, "MPAF directory contains a symlink, reparse point or special file");
				}
			}
		}
		catch (const fs::filesystem_error& error)
		{
			Fail(ErrorCode::Io, error.what());
		}
		return writer;
	}

	void ArchiveWriter::CheckSlot(std::string_view path, bool replace, std::uint64_t memorySize) const
	{
		ArchiveValidator::ValidatePath(path);
		const auto found = sources_.find(path);
		if (!replace && found != sources_.end())
			Fail(ErrorCode::Duplicate, "Duplicate MPAF path: " + std::string(path));
		if (replace && (found == sources_.end() || std::holds_alternative<std::monostate>(found->second)))
			Fail(ErrorCode::Missing, "MPAF file not found: " + std::string(path));
		const auto count = sources_.size() + (replace ? 0 : 1);
		if (count > limits_.entryCount)
			Fail(ErrorCode::Limit, "MPAF entry count exceeds writer limits");
		const auto strings = detail::Add(stringSize_, replace ? 0 : path.size(), limits_.stringSize, ErrorCode::Limit);
		const auto previous = found == sources_.end() ? nullptr : std::get_if<std::vector<std::byte>>(&found->second);
		const auto maxSize = std::min<std::uint64_t>(limits_.archiveSize, MaxArchiveSize);
		const auto minimum = detail::Add(HeaderSize + std::uint64_t(count) * EntrySize, strings, maxSize, ErrorCode::Limit);
		const auto stored = detail::Add(memorySize_ - (previous ? previous->size() : 0), memorySize, maxSize, ErrorCode::Limit);
		(void)detail::Add(minimum, stored, maxSize, ErrorCode::Limit);
		if (memorySize > std::vector<std::byte>().max_size())
			Fail(ErrorCode::Limit, "MPAF source exceeds addressable memory");
	}

	void ArchiveWriter::Store(std::string_view path, Source source, bool replace)
	{
		const auto* data = std::get_if<std::vector<std::byte>>(&source);
		const auto size = data ? data->size() : 0;
		if (replace)
		{
			auto& target = sources_.find(path)->second;
			if (const auto* old = std::get_if<std::vector<std::byte>>(&target))
				memorySize_ -= old->size();
			target = std::move(source);
		}
		else
		{
			sources_.emplace(std::string(path), std::move(source));
			stringSize_ += path.size();
		}
		memorySize_ += size;
	}

	void ArchiveWriter::Add(std::string_view path, Bytes data)
	{
		CheckSlot(path, false, data.size());
		Store(path, std::vector<std::byte>(data.begin(), data.end()), false);
	}

	void ArchiveWriter::Add(std::string_view path, std::istream& input, std::uint64_t size)
	{
		CheckSlot(path, false, size);
		std::vector<std::byte> data(static_cast<std::size_t>(size));
		ReadExact(input, data);
		Store(path, std::move(data), false);
	}

	void ArchiveWriter::AddFile(std::string_view path, const fs::path& source)
	{
		CheckSlot(path, false, 0);
		const auto absolute = fs::absolute(source);
		CheckParents(absolute);
		(void)GetFileSize(absolute);
		Store(path, absolute, false);
	}

	void ArchiveWriter::AddDirectory(std::string_view path)
	{
		CheckSlot(path, false, 0);
		if (IsManifestPath(path))
			Fail(ErrorCode::Path, "MPAF manifest must be a file");
		Store(path, std::monostate{}, false);
	}

	void ArchiveWriter::Replace(std::string_view path, Bytes data)
	{
		CheckSlot(path, true, data.size());
		Store(path, std::vector<std::byte>(data.begin(), data.end()), true);
	}

	void ArchiveWriter::ReplaceFile(std::string_view path, const fs::path& source)
	{
		CheckSlot(path, true, 0);
		const auto absolute = fs::absolute(source);
		CheckParents(absolute);
		(void)GetFileSize(absolute);
		Store(path, absolute, true);
	}

	bool ArchiveWriter::Remove(std::string_view path)
	{
		const auto found = sources_.find(path);
		if (found == sources_.end())
			return false;
		if (const auto* data = std::get_if<std::vector<std::byte>>(&found->second))
			memorySize_ -= data->size();
		stringSize_ -= found->first.size();
		sources_.erase(found);
		return true;
	}

	void ArchiveWriter::WriteTo(const std::function<void(Bytes)>& sink) const
	{
		const bool toml = sources_.contains(TomlManifestPath), legacy = sources_.contains(ManifestPath);
		if (!toml && !legacy)
			Fail(ErrorCode::Missing, "MPAF requires a manifest");
		if (toml && legacy)
			Fail(ErrorCode::Duplicate, "MPAF contains two manifests");

		struct PlannedEntry
		{
			ArchiveEntry entry;
			const Source* source;
		};
		const auto maxSize = std::min<std::uint64_t>(limits_.archiveSize, MaxArchiveSize);
		ArchiveHeader header;
		header.entryCount = static_cast<std::uint32_t>(sources_.size());
		header.stringTableOffset = detail::Add(HeaderSize, std::uint64_t(header.entryCount) * EntrySize,
			maxSize, ErrorCode::Limit);
		std::vector<PlannedEntry> plan;
		plan.reserve(header.entryCount);
		std::uint64_t totalDataSize = 0;
		std::string prefix;
		for (auto it = sources_.begin(); it != sources_.end(); ++it)
		{
			const auto& [path, source] = *it;
			ArchiveEntry entry;
			entry.path = path;
			if (const auto* data = std::get_if<std::vector<std::byte>>(&source))
				entry.dataSize = data->size();
			else if (const auto* file = std::get_if<fs::path>(&source))
				entry.dataSize = GetFileSize(*file);
			else
				entry.type = EntryType::Directory;
			if (IsManifestPath(path))
				entry.type = EntryType::Manifest;
			const auto next = std::next(it);
			if (entry.type != EntryType::Directory && next != sources_.end() && next->first.starts_with(path))
			{
				prefix.assign(path);
				prefix += '/';
				const auto child = sources_.lower_bound(prefix);
				if (child != sources_.end() && child->first.starts_with(prefix))
					Fail(ErrorCode::Path, "MPAF file cannot be an ancestor of another entry");
			}
			header.stringTableSize = detail::Add(header.stringTableSize, path.size(), limits_.stringSize, ErrorCode::Limit);
			totalDataSize = detail::Add(totalDataSize, entry.dataSize, maxSize, ErrorCode::Limit);
			plan.push_back({entry, &source});
		}
		header.dataOffset = detail::Add(header.stringTableOffset, header.stringTableSize, maxSize, ErrorCode::Limit);
		header.archiveSize = detail::Add(header.dataOffset, totalDataSize, maxSize, ErrorCode::Limit);

		sink(EncodeHeader(header));
		std::uint32_t pathOffset = 0;
		auto dataOffset = header.dataOffset;
		for (auto& [entry, source] : plan)
		{
			entry.dataOffset = dataOffset;
			if (const auto* data = std::get_if<std::vector<std::byte>>(source))
				entry.hash = ArchiveHash::Compute(*data);
			else if (const auto* file = std::get_if<fs::path>(source))
				entry.hash = StreamFile(*file, entry.dataSize, [](Bytes) {});
			sink(EncodeEntry(entry, pathOffset));
			pathOffset += static_cast<std::uint32_t>(entry.path.size());
			dataOffset += entry.dataSize;
		}
		for (const auto& item : plan)
			sink(AsBytes(item.entry.path));
		for (const auto& [entry, source] : plan)
		{
			if (const auto* data = std::get_if<std::vector<std::byte>>(source))
				sink(*data);
			else if (const auto* file = std::get_if<fs::path>(source))
			{
				if (StreamFile(*file, entry.dataSize, sink) != entry.hash)
					Fail(ErrorCode::Io, "MPAF source content changed during serialization");
			}
		}
	}

	std::vector<std::byte> ArchiveWriter::Serialize() const
	{
		std::vector<std::byte> bytes;
		WriteTo([&](Bytes data)
		{
			if (bytes.empty())
			{
				const auto size = Load<std::uint64_t>(data, 56);
				if (size > bytes.max_size())
					Fail(ErrorCode::Limit, "MPAF output exceeds addressable memory");
				bytes.reserve(static_cast<std::size_t>(size));
			}
			if (!data.empty())
				bytes.insert(bytes.end(), data.begin(), data.end());
		});
		return bytes;
	}

	void ArchiveWriter::Write(std::ostream& output) const
	{
		WriteTo([&](Bytes data) { WriteStream(output, data); });
	}

	void ArchiveWriter::Write(const fs::path& output) const
	{
		CheckParents(output);
		WriteNewFile(output, [&](const auto& sink) { WriteTo(sink); });
	}
}
