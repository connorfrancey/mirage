#pragma once
// zstd-compressed files: logs/.old (log.cpp) and crashlogs/.old (runtime/crash_reporter.cpp) archive earlier sessions'
// text files with them.

#include <cstddef>
#include <filesystem>
#include <string>

namespace zfile
{
    // `src` compressed into `dst` at zstd `level` (written as dst.tmp, then renamed). False and `error` if not.
    bool Compress(const std::filesystem::path& src, const std::filesystem::path& dst, int level, std::string& error);
    // A text file, plain or zstd-compressed (extension ".zst"), up to `maxBytes` of its text. "" if it can't be read.
    std::string ReadText(const std::filesystem::path& path, std::size_t maxBytes);
}
