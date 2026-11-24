#include "runtime/firstparty.h"

#include <spdlog/spdlog.h>
#include <Windows.h>
#include <toml.hpp>

#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

namespace mirage::firstparty
{
	void MigrateCollisionCache(const std::filesystem::path& modsDir);
	void MigrateFromMenu(const std::filesystem::path& modsDir);

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
		MigrateFromMenu(modsDir);
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
	// texture cache) are copied once into mods/mirage-collision/cache/, where the mod keeps them now
	void MigrateCollisionCache(const std::filesystem::path& modsDir)
	{
		namespace fs = std::filesystem;
		const fs::path old = modsDir.parent_path() / "cache";
		const fs::path target = modsDir / "mirage-collision" / "cache";
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
		std::ofstream(target / ".migrated") << "copied from mirage\\cache\n";
		if (copied) spdlog::info("Mods: {} collision view level cache(s) copied into mirage-collision", copied);
	}


	// Mirage grew out of Mirror's Edge Menu, which kept everything in <Binaries>\memenu\. The first time Mirage starts
	// next to one (no mirage\ folder had mods yet), the player's things are copied over: their own mods, the level layers
	// and settings of the two first-party mods Mirage kept (memenu-levels, memenu-collision: now mirage-*), the collision
	// caches, and the mod manager's lists with those ids renamed. The menu's other mods aren't Mirage's: left where they
	// are, like the whole memenu folder (copied from, never changed). Once: a marker file says it was done.
	void MigrateFromMenu(const std::filesystem::path& modsDir)
	{
		namespace fs = std::filesystem;
		std::error_code ec;
		const fs::path mirageDir = modsDir.parent_path();
		const fs::path old = mirageDir.parent_path() / "memenu" / "mods";
		const fs::path marker = mirageDir / "migrated-from-memenu";
		if (!fs::is_directory(old, ec) || fs::exists(marker, ec)) return;
		fs::create_directories(modsDir, ec);
		const auto copyOptions = fs::copy_options::recursive | fs::copy_options::skip_existing;
		int copied = 0;
		const auto copy = [&](const fs::path& from, const fs::path& to) {
			if (!fs::exists(from, ec)) return;
			fs::create_directories(to.parent_path(), ec);
			fs::copy(from, to, copyOptions, ec);
			if (ec) spdlog::warn("Mods: {} not copied from the menu: {}", from.string(), ec.message());
			else ++copied;
			ec.clear();
		};
		for (const auto& e : fs::directory_iterator(old, ec)) {
			const std::string name = e.path().filename().string();
			if (!e.is_directory() || name.rfind("memenu-", 0) == 0 || name == ".mirage") continue;
			copy(e.path(), modsDir / name);			// The player's own mod
		}
		for (const char* kept : {"levels", "collision"}) {
			const fs::path from = old / (std::string("memenu-") + kept), to = modsDir / (std::string("mirage-") + kept);
			for (const char* part : {"levels", "cache", "config.toml"}) copy(from / part, to / part);
		}
		for (const char* list : {"disabled.toml", "crashes.toml"}) {
			std::ifstream in(old / list, std::ios::binary);
			if (!in || fs::exists(modsDir / list, ec)) continue;
			std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
			for (const char* kept : {"levels", "collision"})
				for (std::size_t at; (at = text.find(std::string("memenu-") + kept)) != std::string::npos;)
					text.replace(at, 6, "mirage");
			std::ofstream(modsDir / list, std::ios::binary) << text;
			++copied;
		}
		std::ofstream(marker) << "Copied from " << old.string() << " (Mirror's Edge Menu) once; delete to copy again.\n";
		spdlog::info("Mods: {} item(s) copied from Mirror's Edge Menu's folder ({})", copied, old.string());
	}
}
