// The Large Address Aware flag (util/pe_laa.h): read, set and cleared on a real Windows image, the header checksum
// kept the way Windows computes it.
#include "util/pe_laa.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

// A 32-bit system DLL: Windows ships its images with a correct checksum
std::vector<std::uint8_t> SystemImage()
{
    const char* windir = std::getenv("WINDIR");
    const std::filesystem::path file = std::filesystem::path(windir ? windir : "C:\Windows") / "SysWOW64" / "kernel32.dll";
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

std::uint32_t StoredChecksum(const std::vector<std::uint8_t>& b)
{
    std::uint32_t nt = 0, sum = 0;
    std::memcpy(&nt, b.data() + 0x3C, 4);
    std::memcpy(&sum, b.data() + nt + 24 + 64, 4);
    return sum;
}

} // namespace

TEST(PeLaa, ChecksumMatchesWindows)
{
    const auto image = SystemImage();
    ASSERT_GT(image.size(), 4096u) << "no SysWOW64\kernel32.dll";
    std::uint32_t nt = 0;
    std::memcpy(&nt, image.data() + 0x3C, 4);
    EXPECT_EQ(pe_laa::Checksum(image, nt + 24 + 64), StoredChecksum(image));
}

TEST(PeLaa, SetAndClearKeepTheChecksumRight)
{
    auto image = SystemImage();
    ASSERT_GT(image.size(), 4096u);
    std::uint32_t nt = 0;
    std::memcpy(&nt, image.data() + 0x3C, 4);
    const bool was = pe_laa::IsLargeAddressAware(image);

    ASSERT_TRUE(pe_laa::SetLargeAddressAware(image, !was));
    EXPECT_EQ(pe_laa::IsLargeAddressAware(image), !was);
    EXPECT_EQ(StoredChecksum(image), pe_laa::Checksum(image, nt + 24 + 64));

    ASSERT_TRUE(pe_laa::SetLargeAddressAware(image, was));
    EXPECT_EQ(image, SystemImage());               // Back to the very same bytes
}

TEST(PeLaa, RejectsWhatIsNotAnImage)
{
    std::vector<std::uint8_t> junk(512, 0x41);
    EXPECT_FALSE(pe_laa::IsLargeAddressAware(junk));
    EXPECT_FALSE(pe_laa::SetLargeAddressAware(junk, true));
    std::vector<std::uint8_t> tiny{'M', 'Z'};
    EXPECT_FALSE(pe_laa::SetLargeAddressAware(tiny, true));
}
