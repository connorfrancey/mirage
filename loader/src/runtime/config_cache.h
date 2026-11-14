#pragma once
// The configuration cache: Mirage's TOML files (mod.toml, a mod's config.toml and defaults.toml, crashes.toml,
// disabled.toml, ...) parsed once and kept, read again only when the file changed on disk (size or write time, checked
// at most every `checkEvery` seconds per file), with typed lookups by dotted key.
//
//   auto doc = mirage::config::Load(dir / "mod.toml");          // shared_ptr<const Document>, null: missing or bad
//   int n = mirage::config::Get<int>(path, "settings.speed.default", 1);
//
// A document never changes once made: callers may keep one and read it on any thread. A file that fails to parse
// keeps the last good document (and the error is reported once); a file that is gone gives null. Any thread.
#include <toml.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace mirage::config
{
	struct Document
	{
		std::filesystem::path path;
		toml::value root;
		std::uint64_t version = 0;			// Bumped each time this path is read again with new contents

		// The value at a dotted key ("settings.speed.default"), or nullptr
		const toml::value* Find(std::string_view dottedKey) const;
		template <class T>
		std::optional<T> As(std::string_view dottedKey) const
		{
			const toml::value* v = Find(dottedKey);
			if (!v) return std::nullopt;
			try {
				return toml::get<T>(*v);
			} catch (const std::exception&) {
				return std::nullopt;			// Another type: as good as missing
			}
		}
	};
	using DocumentRef = std::shared_ptr<const Document>;

	DocumentRef Load(const std::filesystem::path& path, double checkEvery = 1.0);
	void Invalidate(const std::filesystem::path& path);		// Parsed again next time (after the menu wrote it)

	template <class T>
	T Get(const std::filesystem::path& path, std::string_view dottedKey, T fallback)
	{
		if (const DocumentRef d = Load(path))
			if (auto v = d->As<T>(dottedKey)) return *v;
		return fallback;
	}

	struct Stats
	{
		std::size_t documents = 0;
		std::uint64_t hits = 0, parses = 0, failures = 0, stampChecks = 0;
	};
	Stats GetStats();
}
