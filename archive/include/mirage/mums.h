#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace mirage::mums
{
	inline constexpr std::size_t HeaderSize = 64;
	inline constexpr std::size_t FooterSize = 32;
	inline constexpr std::size_t MaxPayloadSize = 256u * 1024u * 1024u;
	inline constexpr std::size_t MaxEnvelopeSize = MaxPayloadSize + 4u * 1024u * 1024u;

	/// Encode a single LZ4 frame with an optional CRC-32 footer.
	[[nodiscard]] std::vector<std::byte> Encode(std::span<const std::byte> payload, bool footer = true);

	/// Validate the envelope, frame, and optional footer, then return the inner payload.
	[[nodiscard]] std::vector<std::byte> Decode(std::span<const std::byte> envelope);
}
