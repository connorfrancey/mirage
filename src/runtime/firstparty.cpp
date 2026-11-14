#include "runtime/firstparty.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <Windows.h>
#include <toml.hpp>

#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace mirage::firstparty
{
	void MigrateCollisionCache(const std::filesystem::path& modsDir);

	namespace
	{
		bool Same(const std::filesystem::path& file, const File& f)
		{
			std::error_code ec;
			if (std::filesystem::file_size(file, ec) != f.size || ec) return false;
			std::ifstream in(file, std::ios::binary);
			std::vector<char> bytes(f.size);
			return in.read(bytes.data(), static_cast<std::streamsize>(f.size)) && std::memcmp(bytes.data(), f.data, f.size) == 0;
		}
	}

	// A native mod's DLL from the menu's resources
	File FromResource(const File& f)
	{
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						   reinterpret_cast<LPCWSTR>(&FromResource), &self);
		File out = f;
		HRSRC r = FindResourceW(self, MAKEINTRESOURCEW(f.resource), MAKEINTRESOURCEW(10));	// RT_RCDATA
		HGLOBAL g = r ? LoadResource(self, r) : nullptr;
		out.data = g ? static_cast<const unsigned char*>(LockResource(g)) : nullptr;
		out.size = r ? SizeofResource(self, r) : 0;
		return out;
	}

	int Install(const std::filesystem::path& modsDir)
	{
		int written = 0;
		std::set<std::string> mods;
		for (File f : Files()) {
			if (!f.data && f.resource) {
				f = FromResource(f);
				if (!f.data) {
					spdlog::error("Mods: the first-party file {} is missing from the menu's resources", f.path);
					continue;
				}
			}
			const std::string path = f.path;
			mods.insert(path.substr(0, path.find('/')));
			const auto target = modsDir / std::filesystem::path(std::u8string(path.begin(), path.end()));
			if (Same(target, f)) continue;
			std::error_code ec;
			std::filesystem::create_directories(target.parent_path(), ec);
			std::ofstream out(target, std::ios::binary | std::ios::trunc);
			if (!out.write(reinterpret_cast<const char*>(f.data), static_cast<std::streamsize>(f.size))) {
				spdlog::error("Mods: the first-party file {} can't be written", target.string());
				continue;
			}
			++written;
		}
		if (written) spdlog::info("Mods: {} first-party mod file(s) written ({} mods)", written, mods.size());
		MigrateCollisionCache(modsDir);
		return written;
	}


	// The collision view's level caches (<menu folder>/cache/*.mevc and *.baked; the folder also holds the runtime's
	// texture cache) are copied once into mods/memenu-collision/cache/, where the mod keeps them now
	void MigrateCollisionCache(const std::filesystem::path& modsDir)
	{
		namespace fs = std::filesystem;
		const fs::path old = modsDir.parent_path() / "cache";
		const fs::path target = modsDir / "memenu-collision" / "cache";
		std::error_code ec;
		if (!fs::is_directory(old, ec) || fs::exists(target / ".migrated", ec)) return;
		fs::create_directories(target, ec);
		int copied = 0;
		for (const auto& e : fs::directory_iterator(old, ec)) {
			const auto ext = e.path().extension();
			if (!e.is_regular_file(ec) || (ext != ".mevc" && ext != ".baked") || fs::exists(target / e.path().filename(), ec)) continue;
			fs::copy_file(e.path(), target / e.path().filename(), ec);
			if (!ec) ++copied;
		}
		std::ofstream(target / ".migrated") << "copied from memenu\\cache\n";
		if (copied) spdlog::info("Mods: {} collision view level cache(s) copied into memenu-collision", copied);
	}

}
