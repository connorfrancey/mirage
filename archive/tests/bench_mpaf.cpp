#include <mirage/mpaf.h>

#include <benchmark/benchmark.h>

#include <sstream>

namespace
{
	using namespace mirage::mpaf;

	ArchiveWriter MakeArchive()
	{
		ArchiveWriter writer;
		const std::vector<std::byte> data(4096, std::byte{42});
		writer.Add(ManifestPath, {});
		for (int i = 0; i < 512; ++i)
			writer.Add("assets/file-" + std::to_string(i), data);
		return writer;
	}

	void MpafParse(benchmark::State& state)
	{
		const auto bytes = MakeArchive().Serialize();
		for (auto _ : state)
		{
			const auto reader = ArchiveReader::Open(Bytes(bytes));
			benchmark::DoNotOptimize(reader.Entries().data());
		}
		state.SetItemsProcessed(state.iterations() * 513);
	}

	void MpafSerialize(benchmark::State& state)
	{
		const auto writer = MakeArchive();
		std::size_t size = 0;
		for (auto _ : state)
		{
			const auto bytes = writer.Serialize();
			size = bytes.size();
			benchmark::DoNotOptimize(bytes.data());
		}
		state.SetBytesProcessed(state.iterations() * size);
	}

	void MpafHash(benchmark::State& state)
	{
		const std::vector<std::byte> bytes(static_cast<std::size_t>(state.range(0)), std::byte{42});
		for (auto _ : state)
			benchmark::DoNotOptimize(ArchiveHash::Compute(bytes));
		state.SetBytesProcessed(state.iterations() * state.range(0));
	}

	void MpafLookup(benchmark::State& state)
	{
		const auto reader = ArchiveReader::Open(MakeArchive().Serialize());
		std::vector<std::string> keys;
		for (int i = 0; i < 512; ++i)
			keys.push_back("assets/file-" + std::to_string((i * 149) % 512));
		std::size_t index = 0;
		for (auto _ : state)
			benchmark::DoNotOptimize(reader.Find(keys[index++ % keys.size()]));
	}

	void MpafExtract(benchmark::State& state)
	{
		const auto reader = ArchiveReader::Open(MakeArchive().Serialize());
		for (auto _ : state)
		{
			std::ostringstream output;
			reader.Extract("assets/file-311", output);
			benchmark::DoNotOptimize(output.view().data());
		}
		state.SetBytesProcessed(state.iterations() * 4096);
	}

	BENCHMARK(MpafParse);
	BENCHMARK(MpafSerialize);
	BENCHMARK(MpafHash)->Arg(4096)->Arg(1024 * 1024);
	BENCHMARK(MpafLookup);
	BENCHMARK(MpafExtract);
}
