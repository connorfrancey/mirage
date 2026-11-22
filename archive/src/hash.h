#pragma once

#include "detail.h"

#include <xxhash.h>

namespace mirage::mpaf::detail
{
	std::uint32_t UpdateCrc(std::uint32_t crc, Bytes bytes) noexcept;

	class HashState
	{
	public:
		HashState() noexcept { XXH3_64bits_reset(&xxh_); }
		void Update(Bytes bytes) noexcept
		{
			crc_ = UpdateCrc(crc_, bytes);
			if (!bytes.empty())
				XXH3_64bits_update(&xxh_, bytes.data(), bytes.size());
		}
		ArchiveHash Finish() const noexcept { return {~crc_, XXH3_64bits_digest(&xxh_)}; }

	private:
		XXH3_state_t xxh_;
		std::uint32_t crc_ = 0xffffffff;
	};
}
