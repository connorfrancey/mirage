// mirage_index: every export of every cooked package as one tab-separated line: package, object path, class, outer
// path, serialized size. `mirage index` builds it, runs it on the game's CookedPC and loads the lines into SQLite to
// search ("which material has a TextureSampleParameter2D", "which maps have a PortalTeleporter").
//
//   mirage_index <CookedPC> <out.tsv>
#include <UnrealLib/Package/Package.hpp>

#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: mirage_index <CookedPC> <out.tsv>\n");
        return 2;
    }
    const fs::path cooked = argv[1];
    std::ofstream out(argv[2], std::ios::binary | std::ios::trunc);
    if (!fs::is_directory(cooked) || !out) {
        std::fprintf(stderr, "mirage_index: no folder %s, or can't write %s\n", argv[1], argv[2]);
        return 1;
    }

    std::size_t packages = 0, rows = 0, failed = 0;
    for (const auto& entry : fs::recursive_directory_iterator(cooked)) {
        const std::string ext = entry.path().extension().string();
        if (!entry.is_regular_file() || (ext != ".u" && ext != ".upk" && ext != ".me1")) continue;
        const std::string name = entry.path().stem().string();
        try {
            // One package at a time: a 32-bit process can't hold them all (export paths need no other package)
            const UnrealLib::Package package = UnrealLib::Package::open(entry.path());
            const UnrealLib::Package* p = &package;
            const auto& exports = p->exports();
            for (std::size_t i = 0; i < exports.size(); ++i) {
                const auto idx = UnrealLib::ObjectIndex::from_export(i);
                const auto& e = exports[i];
                out << name << '\t' << p->object_path(idx) << '\t' << p->class_name(idx) << '\t'
                    << (e.outer.is_null() ? std::string() : p->object_path(e.outer)) << '\t' << e.serial_size << '\n';
            }
            rows += exports.size();
            ++packages;
        } catch (const std::exception& ex) {
            ++failed;      // One unreadable package doesn't stop the index
            std::fprintf(stderr, "mirage_index: %s: %s\n", name.c_str(), ex.what());
        }
    }
    std::printf("mirage_index: %zu packages, %zu objects (%zu packages unreadable)\n", packages, rows, failed);
    return packages ? 0 : 1;
}
