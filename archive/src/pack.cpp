#include <mirage/mpaf.h>
#include <mirage/mums.h>

#include "detail.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace
{
	namespace fs = std::filesystem;

	void Verify(const fs::path& file)
	{
		if (!fs::is_regular_file(file))
			throw std::runtime_error("Expected a .miragemod file");
		const auto size = fs::file_size(file);
		if (size > mirage::mums::MaxEnvelopeSize)
			throw std::runtime_error("MUMS file exceeds the size limit");
		std::vector<std::byte> envelope(static_cast<std::size_t>(size));
		std::ifstream input(file, std::ios::binary);
		if (!input.read(reinterpret_cast<char*>(envelope.data()), static_cast<std::streamsize>(size)) ||
			input.peek() != std::char_traits<char>::eof())
			throw std::runtime_error("Could not read the complete MUMS file");
		const auto payload = mirage::mums::Decode(envelope);
		mirage::mpaf::ArchiveValidator::Validate(payload);
	}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
	if (argc != 3) {
		std::cerr << "Usage: mirage_pack <staged mod folder> <output.miragemod>\n"
					 "       mirage_pack --verify <file.miragemod>\n";
		return 2;
	}
	try {
		const std::filesystem::path source(argv[1]), output(argv[2]);
		if (source == "--verify") {
			Verify(output);
			return 0;
		}
		if (!std::filesystem::is_regular_file(source / mirage::mpaf::TomlManifestPath)) {
			std::cerr << "The mod folder needs mod.toml\n";
			return 1;
		}
		const auto payload = mirage::mpaf::ArchiveWriter::FromDirectory(source).Serialize();
		const auto envelope = mirage::mums::Encode(payload);
		mirage::mpaf::detail::CheckParents(output);
		mirage::mpaf::detail::WriteNewFile(output, [&](const auto& sink) { sink(envelope); });
		return 0;
	} catch (const std::exception& e) {
		std::cerr << e.what() << '\n';
		return 1;
	}
}
