#include "hash.h"

namespace mirage::mpaf
{
	namespace detail
	{
		constexpr auto CrcTables = []
		{
			std::array<std::array<std::uint32_t, 256>, 8> tables{};
			for (std::uint32_t value = 0; value < 256; ++value)
			{
				auto crc = value;
				for (int bit = 0; bit < 8; ++bit)
					crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0);
				tables[0][value] = crc;
			}
			for (std::size_t row = 1; row < tables.size(); ++row)
				for (std::size_t value = 0; value < tables[row].size(); ++value)
				{
					const auto crc = tables[row - 1][value];
					tables[row][value] = (crc >> 8) ^ tables[0][crc & 0xff];
				}
			return tables;
		}();

		std::uint32_t UpdateCrc(std::uint32_t crc, Bytes bytes) noexcept
		{
			while (bytes.size() >= 8)
			{
				const auto a = crc ^ Load<std::uint32_t>(bytes, 0);
				const auto b = Load<std::uint32_t>(bytes, 4);
				crc = CrcTables[7][a & 0xff] ^ CrcTables[6][(a >> 8) & 0xff] ^
					CrcTables[5][(a >> 16) & 0xff] ^ CrcTables[4][a >> 24] ^
					CrcTables[3][b & 0xff] ^ CrcTables[2][(b >> 8) & 0xff] ^
					CrcTables[1][(b >> 16) & 0xff] ^ CrcTables[0][b >> 24];
				bytes = bytes.subspan(8);
			}
			for (auto byte : bytes)
				crc = (crc >> 8) ^ CrcTables[0][(crc ^ std::to_integer<unsigned>(byte)) & 0xff];
			return crc;
		}
	}

	ArchiveHash ArchiveHash::Compute(Bytes bytes) noexcept
	{
		return {~detail::UpdateCrc(0xffffffff, bytes), XXH3_64bits(bytes.data(), bytes.size())};
	}
}
