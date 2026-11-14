// Texture replacement's encoder (runtime/texture_files): what a mod's PNG costs to turn into the game's format. It runs
// on demand inside the file hook, while the engine waits for the package, so its time is part of a level load.
#include "bench.h"

#include "runtime/texture_files.h"

#include <cstdint>
#include <vector>

using mirage::textures::PixelFormat;

namespace {

// A 1024x1024 image with detail in every channel (a flat colour would make DXT blocks trivial)
const std::vector<std::uint8_t>& Image()
{
    static const std::vector<std::uint8_t> rgba = [] {
        std::vector<std::uint8_t> v(1024u * 1024u * 4u);
        std::uint32_t x = 2463534242u;
        for (std::size_t i = 0; i < v.size(); ++i) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            v[i] = static_cast<std::uint8_t>((i / 4 % 1024) / 4 + (x & 31));
        }
        return v;
    }();
    return rgba;
}

void Chain(benchmark::State& state, PixelFormat format)
{
    const auto& rgba = Image();
    for (auto _ : state) bench::Keep(mirage::textures::EncodeChain(rgba.data(), 1024, 1024, format).mips.size());
}

void Surface(benchmark::State& state, PixelFormat format)
{
    const auto& rgba = Image();
    for (auto _ : state) bench::Keep(mirage::textures::EncodeSurface(rgba.data(), 1024, 1024, format).size());
}

} // namespace

BENCHMARK_CAPTURE(Chain, dxt1, PixelFormat::DXT1)->Name("Texture/EncodeChain 1024 DXT1")->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(Chain, dxt5, PixelFormat::DXT5)->Name("Texture/EncodeChain 1024 DXT5")->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(Chain, argb, PixelFormat::A8R8G8B8)->Name("Texture/EncodeChain 1024 ARGB")->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(Surface, dxt5, PixelFormat::DXT5)->Name("Texture/EncodeSurface 1024 DXT5 (top mip only)")->Unit(benchmark::kMillisecond);
BENCHMARK_CAPTURE(Surface, argb, PixelFormat::A8R8G8B8)->Name("Texture/EncodeSurface 1024 ARGB (top mip only)")->Unit(benchmark::kMillisecond);
