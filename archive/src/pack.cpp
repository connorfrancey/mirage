#include <mirage/mpaf.h>

#include <exception>
#include <filesystem>
#include <iostream>

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
	if (argc != 3) {
		std::cerr << "Usage: mirage_pack <staged mod folder> <output.miragemod>\n";
		return 2;
	}
	try {
		const std::filesystem::path source(argv[1]);
		if (!std::filesystem::is_regular_file(source / mirage::mpaf::TomlManifestPath)) {
			std::cerr << "The mod folder needs mod.toml\n";
			return 1;
		}
		mirage::mpaf::ArchiveWriter::FromDirectory(source).Write(std::filesystem::path(argv[2]));
		return 0;
	} catch (const std::exception& e) {
		std::cerr << e.what() << '\n';
		return 1;
	}
}
