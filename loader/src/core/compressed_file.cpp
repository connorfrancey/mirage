#include "core/compressed_file.h"

#include <zstd.h>

#include <fstream>
#include <iterator>
#include <vector>

bool zfile::Compress(const std::filesystem::path& src, const std::filesystem::path& dst, int level, std::string& error)
{
    namespace fs = std::filesystem;
    std::ifstream in(src, std::ios::binary);
    const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (!in && !in.eof()) { error = "cannot read it"; return false; }

    std::vector<char> out(ZSTD_compressBound(data.size()));
    const std::size_t size = ZSTD_compress(out.data(), out.size(), data.data(), data.size(), level);
    if (ZSTD_isError(size)) { error = ZSTD_getErrorName(size); return false; }

    const fs::path tmp = dst.string() + ".tmp";
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(out.data(), static_cast<std::streamsize>(size));
        if (!o) { error = "cannot write " + tmp.string(); return false; }
    }
    std::error_code ec;
    fs::rename(tmp, dst, ec);
    if (ec) { fs::remove(tmp, ec); error = "cannot rename to " + dst.string(); return false; }
    return true;
}

std::string zfile::ReadText(const std::filesystem::path& path, std::size_t maxBytes)
{
    std::ifstream in(path, std::ios::binary);
    if (path.extension() != ".zst") {
        std::string text(maxBytes, '\0');
        in.read(text.data(), static_cast<std::streamsize>(maxBytes));
        text.resize(static_cast<std::size_t>(in.gcount()));
        return text;
    }
    const std::string packed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    ZSTD_DStream* ds = ZSTD_createDStream();
    if (!ds) return {};
    ZSTD_initDStream(ds);
    std::string text;
    std::vector<char> chunk(ZSTD_DStreamOutSize());
    ZSTD_inBuffer input{packed.data(), packed.size(), 0};
    while (input.pos < input.size && text.size() < maxBytes) {
        ZSTD_outBuffer output{chunk.data(), chunk.size(), 0};
        const std::size_t r = ZSTD_decompressStream(ds, &output, &input);
        if (ZSTD_isError(r)) break;
        text.append(chunk.data(), output.pos);
        if (r == 0 && input.pos >= input.size) break;
    }
    ZSTD_freeDStream(ds);
    if (text.size() > maxBytes) text.resize(maxBytes);
    return text;
}
