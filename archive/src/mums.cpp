#include <mirage/mums.h>

#include "detail.h"

#include <lz4frame.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mirage::mums
{
	namespace
	{
		using mpaf::detail::Load;
		using mpaf::detail::Save;
		constexpr std::string_view HeaderMagic{"MUMS\0\0\0\0", 8};
		constexpr std::string_view FooterMagic{"MUM5"};
		constexpr std::uint32_t FrameMagic = 0x184d2204;

		void CheckLz4(std::size_t result)
		{
			if (LZ4F_isError(result))
				throw std::runtime_error(std::string("MUMS LZ4: ") + LZ4F_getErrorName(result));
		}

		bool Zero(std::span<const std::byte> bytes)
		{
			return std::ranges::all_of(bytes, [](std::byte value) { return value == std::byte{}; });
		}

		std::uint32_t Crc(std::span<const std::byte> bytes)
		{
			return ~mpaf::detail::UpdateCrc(0xffffffffu, bytes);
		}
	}

	std::vector<std::byte> Encode(std::span<const std::byte> payload, bool withFooter)
	{
		if (payload.empty() || payload.size() > MaxPayloadSize)
			throw std::runtime_error("MUMS payload size is outside the supported range");

		LZ4F_preferences_t preferences{};
		preferences.frameInfo.blockMode = LZ4F_blockIndependent;
		preferences.frameInfo.contentSize = payload.size();
		const auto frameBound = LZ4F_compressFrameBound(payload.size(), &preferences);
		CheckLz4(frameBound);
		const auto footerSize = withFooter ? FooterSize : 0;
		std::vector<std::byte> envelope(HeaderSize + frameBound + footerSize);
		const auto frameSize = LZ4F_compressFrame(envelope.data() + HeaderSize, frameBound,
												 payload.data(), payload.size(), &preferences);
		CheckLz4(frameSize);
		envelope.resize(HeaderSize + frameSize + footerSize);

		std::span<std::byte> bytes(envelope);
		std::memcpy(bytes.data(), HeaderMagic.data(), HeaderMagic.size());
		Save<std::uint16_t>(bytes, 0x08, 1);
		Save<std::uint16_t>(bytes, 0x0c, 1);
		Save<std::uint32_t>(bytes, 0x18, static_cast<std::uint32_t>(frameSize));
		Save<std::uint32_t>(bytes, 0x1c, static_cast<std::uint32_t>(payload.size()));
		Save<std::uint32_t>(bytes, 0x24, static_cast<std::uint32_t>(footerSize));
		if (withFooter) {
			const auto footer = bytes.last(FooterSize);
			std::memcpy(footer.data(), FooterMagic.data(), FooterMagic.size());
			Save<std::uint16_t>(footer, 0x06, 1);
			Save<std::uint32_t>(footer, 0x08, Crc(payload));
		}
		return envelope;
	}

	std::vector<std::byte> Decode(std::span<const std::byte> envelope)
	{
		if (envelope.size() < HeaderSize || envelope.size() > MaxEnvelopeSize ||
			std::memcmp(envelope.data(), HeaderMagic.data(), HeaderMagic.size()) != 0)
			throw std::runtime_error("Invalid MUMS header");
		if (Load<std::uint16_t>(envelope, 0x08) != 1 || Load<std::uint16_t>(envelope, 0x0a) != 0 ||
			Load<std::uint16_t>(envelope, 0x0c) != 1)
			throw std::runtime_error("Unsupported MUMS version or compression");
		if (!Zero(envelope.subspan(0x0e, 10)) || !Zero(envelope.subspan(0x20, 4)) ||
			!Zero(envelope.subspan(0x28, HeaderSize - 0x28)))
			throw std::runtime_error("Nonzero reserved MUMS header bytes");

		const auto frameSize = Load<std::uint32_t>(envelope, 0x18);
		const auto payloadSize = Load<std::uint32_t>(envelope, 0x1c);
		const auto footerSize = Load<std::uint32_t>(envelope, 0x24);
		if ((footerSize != 0 && footerSize != FooterSize) || footerSize > envelope.size() - HeaderSize ||
			frameSize != envelope.size() - HeaderSize - footerSize || frameSize < 7 ||
			payloadSize == 0 || payloadSize > MaxPayloadSize)
			throw std::runtime_error("Invalid MUMS section sizes");
		const auto footer = envelope.last(footerSize);
		if (!footer.empty() && (std::memcmp(footer.data(), FooterMagic.data(), FooterMagic.size()) != 0 ||
			!Zero(footer.subspan(4, 2)) || Load<std::uint16_t>(footer, 6) != 1 || !Zero(footer.subspan(12))))
			throw std::runtime_error("Invalid MUMS footer");

		const auto frame = envelope.subspan(HeaderSize, frameSize);
		if (Load<std::uint32_t>(frame, 0) != FrameMagic)
			throw std::runtime_error("MUMS requires a standard LZ4 frame");
		if ((std::to_integer<unsigned>(frame[4]) & 1u) != 0)
			throw std::runtime_error("MUMS LZ4 frames cannot use a dictionary");
		LZ4F_dctx* raw = nullptr;
		const auto result = LZ4F_createDecompressionContext(&raw, LZ4F_VERSION);
		const std::unique_ptr<LZ4F_dctx, decltype(&LZ4F_freeDecompressionContext)> context(
			raw, &LZ4F_freeDecompressionContext);
		CheckLz4(result);

		LZ4F_frameInfo_t info{};
		std::size_t read = frame.size();
		CheckLz4(LZ4F_getFrameInfo(context.get(), &info, frame.data(), &read));
		if (info.contentSize && info.contentSize != payloadSize)
			throw std::runtime_error("MUMS LZ4 content size is invalid");

		std::vector<std::byte> payload(payloadSize);
		std::size_t written = 0;
		for (;;) {
			auto input = frame.size() - read;
			auto output = payload.size() - written;
			const auto more = LZ4F_decompress(context.get(), payload.data() + written, &output,
											 frame.data() + read, &input, nullptr);
			CheckLz4(more);
			read += input;
			written += output;
			if (!more) break;
			if (!input && !output) throw std::runtime_error("Incomplete or oversized MUMS LZ4 frame");
		}
		if (read != frame.size() || written != payload.size())
			throw std::runtime_error("MUMS LZ4 frame length mismatch");
		if (!footer.empty() && Crc(payload) != Load<std::uint32_t>(footer, 8))
			throw std::runtime_error("MUMS payload CRC-32 mismatch");
		return payload;
	}
}
