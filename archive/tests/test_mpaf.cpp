#include <mirage/mpaf.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <limits>
#include <sstream>

namespace fs = std::filesystem;
using namespace mirage::mpaf;

TEST(Mpaf, PreservesTomlManifestAndDeclarationFiles)
{
	ArchiveWriter writer;
	const std::string manifest = "id = \"cryptigo.field-lab\"\nversion = \"1.0.0\"\n"
		"actions = \"config/actions.toml\"\nsettings = \"config/settings.toml\"\n";
	const std::string actions = "deploy = \"F6\"\n";
	const std::string settings = "hud = true\n";
	const auto bytes = [](const std::string& text) { return std::as_bytes(std::span(text.data(), text.size())); };
	writer.Add(TomlManifestPath, bytes(manifest));
	writer.Add("config/actions.toml", bytes(actions));
	writer.Add("config/settings.toml", bytes(settings));
	const auto serialized = writer.Serialize();
	const auto reader = ArchiveReader::Open(serialized);
	ASSERT_NE(reader.Find(TomlManifestPath), nullptr);
	EXPECT_EQ(reader.Find(TomlManifestPath)->type, EntryType::Manifest);
	EXPECT_TRUE(std::ranges::equal(reader.Manifest(), bytes(manifest)));
	EXPECT_TRUE(std::ranges::equal(reader.Read("config/actions.toml"), bytes(actions)));
	EXPECT_TRUE(std::ranges::equal(reader.Read("config/settings.toml"), bytes(settings)));
	writer.Add(ManifestPath, {});
	EXPECT_THROW((void)writer.Serialize(), ArchiveError);
}

namespace
{
	Bytes View(std::string_view text)
	{
		return std::as_bytes(std::span(text.data(), text.size()));
	}

	std::string_view Text(Bytes bytes)
	{
		return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
	}

	void Set(std::vector<std::byte>& bytes, std::size_t offset, std::size_t width, std::uint64_t value)
	{
		for (std::size_t i = 0; i < width; ++i)
			bytes.at(offset + i) = std::byte((value >> (i * 8)) & 0xff);
	}

	template<typename Fn>
	void ExpectError(ErrorCode code, Fn fn)
	{
		try
		{
			fn();
			ADD_FAILURE() << "Expected ArchiveError";
		}
		catch (const ArchiveError& error)
		{
			EXPECT_EQ(error.Code(), code) << error.what();
		}
	}

	class TempDirectory
	{
	public:
		TempDirectory()
		{
			const auto time = std::chrono::steady_clock::now().time_since_epoch().count();
			for (int i = 0; i < 100; ++i)
			{
				root = fs::canonical(fs::temp_directory_path()) / ("mirage-mpaf-" + std::to_string(time) + "-" + std::to_string(i));
				if (fs::create_directory(root))
					return;
			}
			throw std::runtime_error("Could not reserve a test directory");
		}
		~TempDirectory()
		{
			std::error_code ignored;
			fs::remove_all(root, ignored);
		}
		fs::path root;
	};

	std::vector<std::byte> LoadVector(const char* name)
	{
		std::ifstream file(fs::path(MPAF_VECTOR_DIR) / name, std::ios::binary);
		if (!file)
			throw std::runtime_error("Missing MPAF golden vector");
		const std::string data{std::istreambuf_iterator<char>(file), {}};
		return {View(data).begin(), View(data).end()};
	}

	ArchiveWriter Sample()
	{
		ArchiveWriter writer;
		writer.Add("manifest.yaml", {});
		writer.Add("check.txt", View("123456789"));
		return writer;
	}
}

TEST(Mpaf, GoldenBinaryVectors)
{
	const auto bytes = LoadVector("manifest-only.mpaf");
	ArchiveWriter writer;
	writer.Add(ManifestPath, {});
	EXPECT_EQ(writer.Serialize(), bytes);
	const auto reader = ArchiveReader::Open(Bytes(bytes));
	EXPECT_EQ(reader.Header().entryTableOffset, 64u);
	EXPECT_EQ(reader.Header().stringTableOffset, 128u);
	EXPECT_EQ(reader.Header().dataOffset, 141u);
	EXPECT_EQ(reader.Header().archiveSize, 141u);
	ASSERT_EQ(reader.Entries().size(), 1u);
	EXPECT_EQ(reader.Entries()[0].type, EntryType::Manifest);
	EXPECT_EQ(reader.Entries()[0].hash, (ArchiveHash{0, 0x2d06800538d394c2}));
	EXPECT_TRUE(reader.Manifest().empty());

	const auto sample = LoadVector("check.mpaf");
	EXPECT_EQ(Sample().Serialize(), sample);
	const auto verified = ArchiveReader::Open(Bytes(sample));
	EXPECT_EQ(Text(verified.Read("check.txt")), "123456789");
	EXPECT_EQ(verified.Find("check.txt")->hash, (ArchiveHash{0xcbf43926, 0x72dcb18b67a17dff}));
	EXPECT_EQ(verified.Header().dataOffset, 214u);
	EXPECT_EQ(verified.Header().archiveSize, 223u);
	EXPECT_NO_THROW(verified.Verify());
}

TEST(Mpaf, DeterministicRoundTrip)
{
	constexpr auto path = "assets/caf\xc3\xa9.dat";
	const std::string binary("a\0\xff", 3);
	ArchiveWriter writer;
	writer.AddDirectory("empty");
	writer.Add(ManifestPath, View("id: sample\n"));
	writer.Add(path, View(binary));
	writer.Add("assets/empty.bin", {});

	ArchiveWriter reversed;
	reversed.Add("assets/empty.bin", View("temporary"));
	reversed.Add(path, View(binary));
	reversed.Add(ManifestPath, View("id: sample\n"));
	reversed.AddDirectory("empty");
	reversed.Replace("assets/empty.bin", {});
	ASSERT_EQ(reversed.Serialize(), writer.Serialize());
	reversed.Add("unneeded", {});
	EXPECT_TRUE(reversed.Remove("unneeded"));
	EXPECT_FALSE(reversed.Remove("unneeded"));
	EXPECT_EQ(reversed.Serialize(), writer.Serialize());
	ExpectError(ErrorCode::Missing, [&] { reversed.Replace("absent", {}); });

	const auto reader = ArchiveReader::Open(writer.Serialize());
	ASSERT_EQ(reader.Entries().size(), 4u);
	EXPECT_EQ(reader.Entries()[0].path, path);
	EXPECT_EQ(Text(reader.Read(path)), binary);
	EXPECT_EQ(Text(reader.Manifest()), "id: sample\n");
	EXPECT_TRUE(reader.Read("assets/empty.bin").empty());
	EXPECT_EQ(reader.Find("empty")->type, EntryType::Directory);
	EXPECT_EQ(reader.Find("absent"), nullptr);
	ExpectError(ErrorCode::Missing, [&] { (void)reader.Read("empty"); });
}

TEST(Mpaf, RejectsUnsafePaths)
{
	const std::string_view invalid[] = {"", "/file", "a//b", "a/", "../file", "./file", "a/../b", "a\\b",
		"c:/file", "file:stream", "file.", "file ", "a/NUL.cfg", "a/COM\xc2\xb9.cfg", "a/AUX .txt",
		"a/<bad>", "a/\xc0\xaf", "a/\xed\xa0\x80", std::string_view("a\0b", 3)};
	for (const auto path : invalid)
		ExpectError(ErrorCode::Path, [&] { ArchiveValidator::ValidatePath(path); });
	EXPECT_NO_THROW(ArchiveValidator::ValidatePath(std::string(MaxPathSize, 'a')));
	ExpectError(ErrorCode::Path, [] { ArchiveValidator::ValidatePath(std::string(MaxPathSize + 1, 'a')); });
}

TEST(Mpaf, EveryTruncationAndTrailingData)
{
	const auto bytes = Sample().Serialize();
	for (std::size_t size = 0; size < bytes.size(); ++size)
		EXPECT_THROW((void)ArchiveReader::Open(Bytes(bytes).first(size)), ArchiveError) << size;
	auto trailing = bytes;
	trailing.push_back(std::byte{0});
	EXPECT_THROW((void)ArchiveReader::Open(Bytes(trailing)), ArchiveError);
	std::istringstream stream(std::string(Text(trailing)));
	EXPECT_THROW((void)ArchiveReader::Open(stream), ArchiveError);
}

TEST(Mpaf, RejectsHeaderAndEntryCorruption)
{
	const auto pristine = Sample().Serialize();
	const auto change = [&](std::size_t offset, std::size_t width, std::uint64_t value, ErrorCode code)
	{
		auto bytes = pristine;
		Set(bytes, offset, width, value);
		ExpectError(code, [&] { (void)ArchiveReader::Open(Bytes(bytes)); });
	};
	change(0, 1, 0, ErrorCode::Format);
	change(8, 2, 2, ErrorCode::Version);
	change(10, 2, 1, ErrorCode::Version);
	change(12, 4, 1, ErrorCode::Format);
	change(16, 4, 0xffffffff, ErrorCode::Limit);
	change(20, 4, 63, ErrorCode::Format);
	change(24, 8, 63, ErrorCode::Format);
	change(32, 8, 64, ErrorCode::Format);
	change(40, 8, 0x100000000, ErrorCode::Limit);
	change(48, 8, 1, ErrorCode::Format);
	change(56, 8, std::numeric_limits<std::uint64_t>::max(), ErrorCode::Limit);
	change(64, 4, 1, ErrorCode::Format);
	change(68, 4, 0xffffffff, ErrorCode::Format);
	change(72, 8, 213, ErrorCode::Format);
	change(80, 8, 4, ErrorCode::Format);
	change(88, 8, 4, ErrorCode::Format);
	change(100, 2, 3, ErrorCode::Format);
	change(102, 2, 1, ErrorCode::Format);
	change(112, 8, 1, ErrorCode::Format);
	change(120, 8, 1, ErrorCode::Format);
	change(136, 8, 214, ErrorCode::Format); // Manifest overlaps the first file.
	change(164, 2, 0, ErrorCode::Format);
	change(192, 1, '/', ErrorCode::Path);
}

TEST(Mpaf, RejectsDuplicateUnsortedAndFileAncestor)
{
	ArchiveWriter writer;
	writer.Add(ManifestPath, {});
	writer.Add("one", {});
	writer.Add("two", {});
	auto bytes = writer.Serialize();
	const auto strings = ArchiveReader::Open(Bytes(bytes)).Header().stringTableOffset;
	std::copy_n(View("one").begin(), 3, bytes.begin() + static_cast<std::size_t>(strings) + 16);
	ExpectError(ErrorCode::Duplicate, [&] { (void)ArchiveReader::Open(Bytes(bytes)); });
	std::copy_n(View("abc").begin(), 3, bytes.begin() + static_cast<std::size_t>(strings) + 16);
	ExpectError(ErrorCode::Format, [&] { (void)ArchiveReader::Open(Bytes(bytes)); });

	writer.Add("one-other", {});
	writer.Add("one/nested", {});
	ExpectError(ErrorCode::Path, [&] { (void)writer.Serialize(); });
	ASSERT_TRUE(writer.Remove("one"));
	writer.AddDirectory("one");
	auto conflicting = writer.Serialize();
	const auto reader = ArchiveReader::Open(Bytes(conflicting));
	const auto it = std::find_if(reader.Entries().begin(), reader.Entries().end(),
		[](const auto& entry) { return entry.path == "one"; });
	ASSERT_NE(it, reader.Entries().end());
	Set(conflicting, HeaderSize + (it - reader.Entries().begin()) * EntrySize + 36, 2, 0);
	ExpectError(ErrorCode::Path, [&] { (void)ArchiveReader::Open(Bytes(conflicting)); });
}

TEST(Mpaf, ManifestAndLimits)
{
	ArchiveWriter writer;
	EXPECT_THROW((void)writer.Serialize(), ArchiveError);
	EXPECT_THROW(writer.AddDirectory(ManifestPath), ArchiveError);
	writer.Add("not-a-manifest", {});
	auto noManifest = Sample().Serialize();
	noManifest[201] = std::byte{'d'};
	Set(noManifest, 164, 2, 0);
	ExpectError(ErrorCode::Missing, [&] { (void)ArchiveReader::Open(Bytes(noManifest)); });

	writer.Add(ManifestPath, {});
	ExpectError(ErrorCode::Duplicate, [&] { writer.Add(ManifestPath, {}); });
	const auto bytes = writer.Serialize();
	ExpectError(ErrorCode::Limit, [&] { (void)ArchiveReader::Open(Bytes(bytes), {.archiveSize = bytes.size() - 1}); });
	ExpectError(ErrorCode::Limit, [&] { (void)ArchiveReader::Open(Bytes(bytes), {.entryCount = 1}); });
	ExpectError(ErrorCode::Limit, [&] { (void)ArchiveReader::Open(Bytes(bytes), {.stringSize = 12}); });
	ArchiveWriter small({.archiveSize = 141, .entryCount = 1, .stringSize = 13});
	ExpectError(ErrorCode::Limit, [&] { small.Add(ManifestPath, View("x")); });
	small.Add(ManifestPath, {});
	EXPECT_EQ(small.Serialize().size(), 141u);
}

TEST(Mpaf, VerifiesBothHashesBeforeReturningData)
{
	for (const std::size_t offset : {96, 104, 214})
	{
		auto bytes = Sample().Serialize();
		bytes.at(offset) ^= std::byte{1};
		const auto reader = ArchiveReader::Open(Bytes(bytes));
		EXPECT_TRUE(reader.Manifest().empty());
		ExpectError(ErrorCode::Checksum, [&] { (void)reader.Read("check.txt"); });
		ExpectError(ErrorCode::Checksum, [&] { ArchiveValidator::Validate(bytes); });
		std::ostringstream out;
		ExpectError(ErrorCode::Checksum, [&] { reader.Extract("check.txt", out); });
		EXPECT_TRUE(out.str().empty());
	}
}

TEST(Mpaf, StreamsFilesystemAndMemory)
{
	TempDirectory temp;
	const auto source = temp.root / "source";
	fs::create_directories(source / "assets" / "empty");
	std::ofstream(source / ManifestPath, std::ios::binary) << "id: sample\n";
	const std::string content(2 * 1024 * 1024 + 13, 'a');
	std::ofstream(source / "assets" / "large.bin", std::ios::binary) << content;
	auto writer = ArchiveWriter::FromDirectory(source);
	writer.ReplaceFile("assets/large.bin", source / "assets" / "large.bin");
	writer.Add("assets/caf\xc3\xa9.txt", View("unicode"));
	std::istringstream incoming("cut-trailing");
	writer.Add("stream.bin", incoming, 3);
	EXPECT_EQ(incoming.peek(), '-');
	std::istringstream shortInput("a");
	EXPECT_THROW(writer.Add("short", shortInput, 2), ArchiveError);
	EXPECT_FALSE(writer.Remove("short"));

	const auto archive = temp.root / "sample.mpaf";
	writer.Write(archive);
	EXPECT_THROW(writer.Write(archive), ArchiveError);
	const auto reader = ArchiveReader::Open(archive);
	EXPECT_EQ(Text(reader.Read("assets/large.bin")), content);
	EXPECT_EQ(reader.Find("assets/large.bin")->hash, ArchiveHash::Compute(View(content)));
	EXPECT_EQ(Text(reader.Read("stream.bin")), "cut");

	std::ostringstream output;
	writer.Write(output);
	std::istringstream input(output.str());
	input.exceptions(std::ios::failbit | std::ios::badbit);
	EXPECT_NO_THROW(ArchiveReader::Open(input).Verify());
	EXPECT_EQ(output.str(), Text(writer.Serialize()));
	const auto root = temp.root / "extracted";
	reader.ExtractAll(root);
	EXPECT_TRUE(fs::is_directory(root / "assets" / "empty"));
	EXPECT_EQ(fs::file_size(root / "assets" / "large.bin"), content.size());
	EXPECT_EQ(fs::file_size(root / "assets" / fs::path(u8"caf\u00e9.txt")), 7u);
	reader.Extract("stream.bin", temp.root / "single");
	EXPECT_THROW(reader.Extract("stream.bin", temp.root / "single"), ArchiveError);
	EXPECT_THROW(reader.ExtractAll(root), ArchiveError);
}

TEST(Mpaf, ExtractionRejectsLinksAndRollsBack)
{
	TempDirectory temp;
	const auto outside = temp.root / "outside";
	fs::create_directory(outside);
	const auto reader = ArchiveReader::Open(Sample().Serialize());
	std::error_code error;
	const auto link = temp.root / "link";
	fs::create_directory_symlink(outside, link, error);
	if (!error)
	{
		EXPECT_THROW(reader.ExtractAll(link), ArchiveError);
		EXPECT_THROW(reader.ExtractAll(link / "new"), ArchiveError);
		EXPECT_THROW(reader.Extract("check.txt", link / "escaped"), ArchiveError);
		EXPECT_THROW((void)ArchiveWriter::FromDirectory(temp.root), ArchiveError);
		EXPECT_TRUE(fs::is_empty(outside));
	}

	auto writer = Sample();
	writer.Add("dir/nested/file", {});
	const auto nested = ArchiveReader::Open(writer.Serialize());
	const auto limited = temp.root / "limited";
	ExpectError(ErrorCode::Limit, [&] { nested.ExtractAll(limited, 1); });
	EXPECT_FALSE(fs::exists(limited));
	auto bytes = writer.Serialize();
	bytes[96] ^= std::byte{1};
	const auto corrupt = ArchiveReader::Open(Bytes(bytes));
	const auto corruptRoot = temp.root / "corrupt";
	ExpectError(ErrorCode::Checksum, [&] { corrupt.ExtractAll(corruptRoot); });
	EXPECT_FALSE(fs::exists(corruptRoot));
}

TEST(Mpaf, ExtractionDetectsCaseInsensitiveAliases)
{
	TempDirectory temp;
	fs::create_directory(temp.root / "probe");
	const bool insensitive = fs::exists(temp.root / "PROBE");
	auto writer = Sample();
	writer.Add("Assets/one", {});
	writer.Add("assets/two", {});
	const auto reader = ArchiveReader::Open(writer.Serialize());
	const auto root = temp.root / "output";
	if (insensitive)
	{
		ExpectError(ErrorCode::Destination, [&] { reader.ExtractAll(root); });
		EXPECT_FALSE(fs::exists(root));
	}
	else
	{
		EXPECT_NO_THROW(reader.ExtractAll(root));
		EXPECT_TRUE(fs::exists(root / "Assets" / "one"));
		EXPECT_TRUE(fs::exists(root / "assets" / "two"));
	}
}
