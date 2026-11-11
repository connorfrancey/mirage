#include "runtime/firstparty.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <toml.hpp>

#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace mirage::firstparty
{
	void MigrateKeyBindings(const std::filesystem::path& modsDir);

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

	int Install(const std::filesystem::path& modsDir)
	{
		int written = 0;
		std::set<std::string> mods;
		for (const File& f : Files()) {
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
		MigrateKeyBindings(modsDir);
		return written;
	}

	// Keep Key Bindings was native and kept <menu folder>/keybindings.json; it is mods/memenu-misc now, keeping them in
	// its config.toml ("bindings" = "Key=Command;..."). Once: the old file is moved into the mod's config.
	void MigrateKeyBindings(const std::filesystem::path& modsDir)
	{
		const auto old = modsDir.parent_path() / "keybindings.json";
		std::error_code ec;
		if (!std::filesystem::exists(old, ec)) return;
		const auto config = modsDir / "memenu-misc" / "config.toml";
		toml::value table = toml::table{};
		if (std::filesystem::exists(config, ec)) {
			try {
				table = toml::parse(config);
			} catch (const std::exception&) {
				return;		// Not touched: the mod reports its own config
			}
		}
		if (table.is_table() && table.contains("bindings")) return;
		std::ifstream in(old);
		const auto j = nlohmann::json::parse(in, nullptr, false);
		in.close();
		if (!j.is_object() || !j.contains("bindings") || !j["bindings"].is_array()) return;
		std::set<std::string> list;
		for (const auto& b : j["bindings"])
			if (b.is_object() && b.contains("key") && b.contains("command") && b["key"].is_string() && b["command"].is_string())
				list.insert(b["key"].get<std::string>() + "=" + b["command"].get<std::string>());
		std::string joined;
		for (const std::string& b : list) joined += (joined.empty() ? "" : ";") + b;
		table.as_table()["bindings"] = joined;
		std::filesystem::create_directories(config.parent_path(), ec);
		std::ofstream out(config, std::ios::binary | std::ios::trunc);
		if (!(out << toml::format(table))) return;
		out.close();
		std::filesystem::rename(old, old.parent_path() / "keybindings.json.migrated", ec);
		spdlog::info("Mods: {} kept key bindings moved into memenu-misc's config", list.size());
	}
}
