#include "detail.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <istream>
#include <ostream>
#include <utility>

#include <simdutf.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace mirage::mpaf
{
	ArchiveError::ArchiveError(ErrorCode code, std::string message)
		: std::runtime_error(std::move(message)), code_(code)
	{
	}

	void ArchiveValidator::ValidatePath(std::string_view path)
	{
		using detail::Fail;
		if (path.empty() || path.size() > MaxPathSize || !simdutf::validate_utf8(path.data(), path.size()))
			Fail(ErrorCode::Path, "MPAF path must be valid UTF-8 and contain 1 to 4096 bytes");

		while (!path.empty())
		{
			const auto slash = path.find('/');
			const auto part = path.substr(0, slash);
			if (part.empty() || part.back() == '.' || part.back() == ' ' ||
				(slash != std::string_view::npos && slash + 1 == path.size()))
				Fail(ErrorCode::Path, "MPAF path contains an empty or unsafe component");
			for (const unsigned char ch : part)
			{
				if (ch < 32 || ch == 127 || std::string_view("\\:<>|\"?*").find(static_cast<char>(ch)) != std::string_view::npos)
					Fail(ErrorCode::Path, "MPAF path contains an unsafe character");
			}
			auto stem = part.substr(0, part.find('.'));
			while (!stem.empty() && stem.back() == ' ')
				stem.remove_suffix(1);
			if (stem.size() <= 7)
			{
				char upper[7];
				std::transform(stem.begin(), stem.end(), upper, [](char c)
					{ return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; });
				const std::string_view device(upper, stem.size());
				if (device == "CON" || device == "PRN" || device == "AUX" || device == "NUL" ||
					device == "CONIN$" || device == "CONOUT$")
					Fail(ErrorCode::Path, "MPAF path names a Windows device");
				if (device.starts_with("COM") || device.starts_with("LPT"))
				{
					const auto number = device.substr(3);
					if ((number.size() == 1 && number[0] >= '0' && number[0] <= '9') ||
						number == "\xc2\xb9" || number == "\xc2\xb2" || number == "\xc2\xb3")
						Fail(ErrorCode::Path, "MPAF path names a Windows device");
				}
			}
			if (slash == std::string_view::npos)
				break;
			path.remove_prefix(slash + 1);
		}
	}
}

namespace mirage::mpaf::detail
{
	void Fail(ErrorCode code, std::string message)
	{
		throw ArchiveError(code, std::move(message));
	}

	std::uint64_t Add(std::uint64_t left, std::uint64_t right, std::uint64_t limit, ErrorCode code)
	{
		if (left > limit || right > limit - left)
			Fail(code, "MPAF section exceeds its size boundary");
		return left + right;
	}

	ArchiveHeader DecodeHeader(Bytes bytes, Limits limits)
	{
		if (bytes.size() < HeaderSize || !std::equal(AsBytes(Magic).begin(), AsBytes(Magic).end(), bytes.begin()))
			Fail(ErrorCode::Format, "Invalid or truncated MPAF header");
		ArchiveHeader header;
		header.major = Load<std::uint16_t>(bytes, 8);
		header.minor = Load<std::uint16_t>(bytes, 10);
		if (header.major != 1 || header.minor != 0)
			Fail(ErrorCode::Version, "Unsupported MPAF version");
		if (Load<std::uint32_t>(bytes, 12) != 0 || Load<std::uint32_t>(bytes, 20) != EntrySize)
			Fail(ErrorCode::Format, "Invalid MPAF flags or entry size");
		header.entryCount = Load<std::uint32_t>(bytes, 16);
		header.entryTableOffset = Load<std::uint64_t>(bytes, 24);
		header.stringTableOffset = Load<std::uint64_t>(bytes, 32);
		header.stringTableSize = Load<std::uint64_t>(bytes, 40);
		header.dataOffset = Load<std::uint64_t>(bytes, 48);
		header.archiveSize = Load<std::uint64_t>(bytes, 56);

		if (header.archiveSize > MaxArchiveSize || header.archiveSize > limits.archiveSize ||
			header.archiveSize > std::vector<std::byte>().max_size() ||
			header.entryCount > limits.entryCount || header.stringTableSize > limits.stringSize)
			Fail(ErrorCode::Limit, "MPAF header exceeds reader limits");
		if (header.entryTableOffset != HeaderSize ||
			header.stringTableOffset != Add(HeaderSize, std::uint64_t(header.entryCount) * EntrySize,
				header.archiveSize, ErrorCode::Format) ||
			header.dataOffset != Add(header.stringTableOffset, header.stringTableSize, header.archiveSize, ErrorCode::Format) ||
			header.stringTableSize < header.entryCount ||
			header.stringTableSize > std::uint64_t(header.entryCount) * MaxPathSize)
			Fail(ErrorCode::Format, "Invalid MPAF section layout");
		return header;
	}

	std::array<std::byte, HeaderSize> EncodeHeader(const ArchiveHeader& header)
	{
		std::array<std::byte, HeaderSize> bytes{};
		std::copy(AsBytes(Magic).begin(), AsBytes(Magic).end(), bytes.begin());
		Save(bytes, 8, header.major);
		Save(bytes, 10, header.minor);
		Save(bytes, 16, header.entryCount);
		Save(bytes, 20, std::uint32_t{EntrySize});
		Save(bytes, 24, header.entryTableOffset);
		Save(bytes, 32, header.stringTableOffset);
		Save(bytes, 40, header.stringTableSize);
		Save(bytes, 48, header.dataOffset);
		Save(bytes, 56, header.archiveSize);
		return bytes;
	}

	void ReadExact(std::istream& input, std::span<std::byte> data)
	{
		try
		{
			while (!data.empty())
			{
				const auto count = std::min(data.size(), ChunkSize);
				if (!input.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(count)))
					Fail(input.bad() ? ErrorCode::Io : ErrorCode::Format, "Could not read complete MPAF data");
				data = data.subspan(count);
			}
		}
		catch (const std::ios_base::failure&)
		{
			Fail(input.bad() ? ErrorCode::Io : ErrorCode::Format, "Could not read complete MPAF data");
		}
	}

	void WriteStream(std::ostream& output, Bytes data)
	{
		try
		{
			while (!data.empty())
			{
				const auto count = std::min(data.size(), ChunkSize);
				if (!output.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(count)))
					Fail(ErrorCode::Io, "Could not write MPAF output");
				data = data.subspan(count);
			}
		}
		catch (const std::ios_base::failure&)
		{
			Fail(ErrorCode::Io, "Could not write MPAF output");
		}
	}

	fs::file_type FileType(const fs::path& path)
	{
		std::error_code error;
		const auto type = fs::symlink_status(path, error).type();
		if (error && error != std::errc::no_such_file_or_directory)
			Fail(ErrorCode::Io, "Could not inspect filesystem path: " + error.message());
#ifdef _WIN32
		if (type != fs::file_type::not_found)
		{
			const auto attributes = GetFileAttributesW(path.c_str());
			if (attributes == INVALID_FILE_ATTRIBUTES)
				Fail(ErrorCode::Io, "Could not read Windows file attributes");
			if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
				return fs::file_type::symlink;
		}
#endif
		return type;
	}

	void CheckParents(const fs::path& path)
	{
		for (auto parent = path.parent_path(); !parent.empty(); parent = parent.parent_path())
		{
			if (FileType(parent) != fs::file_type::directory)
				Fail(ErrorCode::Destination, "MPAF output parent must be a directory without symlinks or reparse points");
			if (parent == parent.parent_path())
				break;
		}
	}

	void CreateExtractionRoot(const fs::path& path)
	{
		CheckParents(path);
#ifdef _WIN32
		if (!CreateDirectoryW(path.c_str(), nullptr))
#else
		if (::mkdir(path.c_str(), S_IRWXU) != 0)
#endif
			Fail(ErrorCode::Destination, "Could not exclusively create an MPAF extraction root");
	}

	void WriteNewFile(const fs::path& path, const std::function<void(const std::function<void(Bytes)>&)>& write)
	{
		if (!path.has_filename())
			Fail(ErrorCode::Destination, "MPAF output path must name a file");
#ifdef _WIN32
		std::FILE* file = nullptr;
		const auto error = _wfopen_s(&file, path.c_str(), L"wbx");
#else
		auto* file = std::fopen(path.c_str(), "wbx");
		const auto error = file ? 0 : errno;
#endif
		if (!file)
			Fail(error == EEXIST ? ErrorCode::Destination : ErrorCode::Io, "Could not create a new MPAF output file");
		try
		{
			write([&](Bytes data)
			{
				if (!data.empty() && std::fwrite(data.data(), 1, data.size(), file) != data.size())
					Fail(ErrorCode::Io, "Could not write MPAF output file");
			});
			const auto result = std::fclose(file);
			file = nullptr;
			if (result != 0)
				Fail(ErrorCode::Io, "Could not close MPAF output file");
		}
		catch (...)
		{
			if (file)
				std::fclose(file);
			std::error_code ignored;
			fs::remove(path, ignored);
			throw;
		}
	}

	fs::path FromUtf8(std::string_view path)
	{
		const auto* text = reinterpret_cast<const char8_t*>(path.data());
		return fs::path(std::u8string_view(text, path.size()));
	}

	std::string ToUtf8(const fs::path& path)
	{
		const auto text = path.generic_u8string();
		return std::string(reinterpret_cast<const char*>(text.data()), text.size());
	}
}
