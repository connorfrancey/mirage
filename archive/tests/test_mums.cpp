#include <mirage/mpaf.h>
#include <mirage/mums.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace
{
	namespace mums = mirage::mums;
	namespace mpaf = mirage::mpaf;

	std::vector<std::byte> Fixture(const char* name)
	{
		const auto file = std::filesystem::path(MPAF_VECTOR_DIR) / name;
		std::vector<std::byte> bytes(static_cast<std::size_t>(std::filesystem::file_size(file)));
		std::ifstream input(file, std::ios::binary);
		if (!input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
			throw std::runtime_error("Unable to read fixture");
		return bytes;
	}

	void Set32(std::vector<std::byte>& bytes, std::size_t offset, std::uint32_t value)
	{
		for (unsigned i = 0; i < 4; ++i) bytes.at(offset + i) = static_cast<std::byte>(value >> (i * 8));
	}
}

TEST(Mums, DecodesIndependentGoldenVector)
{
	const auto payload = mums::Decode(Fixture("manifest-only.mums"));
	EXPECT_EQ(payload, Fixture("manifest-only.mpaf"));
	EXPECT_NO_THROW(mpaf::ArchiveValidator::Validate(payload));
}

TEST(Mums, RoundTripsCompressedPayloadWithAndWithoutFooter)
{
	const std::vector payload(180'000, std::byte{0xab});
	for (const bool footer : {false, true}) {
		const auto envelope = mums::Encode(payload, footer);
		EXPECT_LT(envelope.size(), payload.size());
		EXPECT_EQ(envelope.at(0x24), static_cast<std::byte>(footer ? mums::FooterSize : 0));
		EXPECT_EQ(mums::Decode(envelope), payload);
	}
	EXPECT_THROW(static_cast<void>(mums::Encode({})), std::runtime_error);
}

TEST(Mums, RejectsUnexpectedHeaderFooterAndBounds)
{
	const auto good = Fixture("manifest-only.mums");
	for (const std::size_t offset : {0, 7, 8, 10, 12, 14, 16, 20, 24, 32, 36, 40, 63}) {
		auto changed = good;
		changed.at(offset) ^= std::byte{0x10};
		EXPECT_THROW(static_cast<void>(mums::Decode(changed)), std::runtime_error) << offset;
	}
	for (const std::size_t offset : {0, 4, 6, 8, 12, 31}) {
		auto changed = good;
		changed.at(changed.size() - mums::FooterSize + offset) ^= std::byte{1};
		EXPECT_THROW(static_cast<void>(mums::Decode(changed)), std::runtime_error) << offset;
	}
	auto changed = good;
	Set32(changed, 0x1c, static_cast<std::uint32_t>(mums::MaxPayloadSize + 1));
	EXPECT_THROW(static_cast<void>(mums::Decode(changed)), std::runtime_error);
	Set32(changed, 0x1c, 1);
	EXPECT_THROW(static_cast<void>(mums::Decode(changed)), std::runtime_error);
	Set32(changed, 0x1c, 200);
	EXPECT_THROW(static_cast<void>(mums::Decode(changed)), std::runtime_error);
}

TEST(Mums, RejectsTruncatedExtraAndNonstandardFrames)
{
	const auto payload = Fixture("manifest-only.mpaf");
	const auto good = mums::Encode(payload, false);
	for (const auto size : {std::size_t{0}, mums::HeaderSize - 1, good.size() - 1}) {
		const auto truncated = std::span<const std::byte>(good).first(size);
		EXPECT_THROW(static_cast<void>(mums::Decode(truncated)), std::runtime_error);
	}

	auto changed = good;
	changed.pop_back();
	Set32(changed, 0x18, static_cast<std::uint32_t>(changed.size() - mums::HeaderSize));
	EXPECT_THROW(static_cast<void>(mums::Decode(changed)), std::runtime_error);

	changed = good;
	changed.insert(changed.end(), good.begin() + mums::HeaderSize, good.end());
	Set32(changed, 0x18, static_cast<std::uint32_t>(changed.size() - mums::HeaderSize));
	EXPECT_THROW(static_cast<void>(mums::Decode(changed)), std::runtime_error);
	Set32(changed, mums::HeaderSize, 0x184d2a50);
	EXPECT_THROW(static_cast<void>(mums::Decode(changed)), std::runtime_error);
}
