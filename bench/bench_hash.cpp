// FNV-1a (the byte loop the menu used for every hash) against rapidhash (core/hash.h), on the sizes the menu hashes: names
// and keys (16 B), records (256 B), files read by the resource cache (64 KB, 4 MB).
#include "core/hash.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <vector>

namespace {

std::uint64_t Fnv1a(const void* data, std::size_t size)
{
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

std::vector<std::uint8_t> Data(std::size_t n)
{
    std::vector<std::uint8_t> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::uint8_t>(i * 131 + 7);
    return v;
}

void BM_Fnv1a(benchmark::State& state)
{
    const auto data = Data(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) benchmark::DoNotOptimize(Fnv1a(data.data(), data.size()));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
}

void BM_Rapidhash(benchmark::State& state)
{
    const auto data = Data(static_cast<std::size_t>(state.range(0)));
    for (auto _ : state) benchmark::DoNotOptimize(hash::Bytes(data.data(), data.size()));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations()) * state.range(0));
}

} // namespace

BENCHMARK(BM_Fnv1a)->Arg(16)->Arg(256)->Arg(64 << 10)->Arg(4 << 20);
BENCHMARK(BM_Rapidhash)->Arg(16)->Arg(256)->Arg(64 << 10)->Arg(4 << 20);
