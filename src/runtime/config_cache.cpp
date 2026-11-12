#include "runtime/config_cache.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <mutex>
#include <unordered_map>

namespace mirage::config
{
	namespace
	{
		namespace fs = std::filesystem;
		struct Entry
		{
			DocumentRef doc;
			std::uintmax_t size = 0;
			fs::file_time_type time{};
			std::chrono::steady_clock::time_point checked{};
			std::string lastError;
		};
		std::mutex g_lock;
		std::unordered_map<std::string, Entry> g_entries;
		Stats g_stats;

		std::string Key(const fs::path& path)
		{
			std::error_code ec;
			fs::path p = fs::weakly_canonical(path, ec);
			if (ec) p = path;
			auto u = p.generic_u8string();
			std::string key(u.begin(), u.end());
			for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return key;
		}
	}

	const toml::value* Document::Find(std::string_view dottedKey) const
	{
		const toml::value* v = &root;
		while (!dottedKey.empty()) {
			const auto dot = dottedKey.find('.');
			const std::string part(dottedKey.substr(0, dot));
			if (!v->is_table()) return nullptr;
			const auto& table = v->as_table();
			const auto it = table.find(part);
			if (it == table.end()) return nullptr;
			v = &it->second;
			if (dot == std::string_view::npos) break;
			dottedKey.remove_prefix(dot + 1);
		}
		return v;
	}

	DocumentRef Load(const fs::path& path, double checkEvery)
	{
		const std::string key = Key(path);
		const auto now = std::chrono::steady_clock::now();
		DocumentRef current;
		std::uintmax_t knownSize = 0;
		fs::file_time_type knownTime{};
		{
			std::lock_guard<std::mutex> l(g_lock);
			auto it = g_entries.find(key);
			if (it != g_entries.end()) {
				Entry& e = it->second;
				if (std::chrono::duration<double>(now - e.checked).count() < checkEvery) {
					++g_stats.hits;
					return e.doc;
				}
				e.checked = now;
				current = e.doc;
				knownSize = e.size;
				knownTime = e.time;
			}
		}
		// Changed on disk? (outside the lock: the file system may be slow)
		std::error_code ec;
		const auto size = fs::file_size(path, ec);
		const auto time = ec ? fs::file_time_type{} : fs::last_write_time(path, ec);
		{
			std::lock_guard<std::mutex> l(g_lock);
			++g_stats.stampChecks;
			if (ec) {		// Gone
				g_entries.erase(key);
				g_stats.documents = g_entries.size();
				return nullptr;
			}
			if (current && size == knownSize && time == knownTime) {
				++g_stats.hits;
				return current;
			}
		}
		// Parse it
		auto doc = std::make_shared<Document>();
		doc->path = path;
		std::string error;
		try {
			doc->root = toml::parse(path);
		} catch (const std::exception& e) {
			error = e.what();
		}
		std::lock_guard<std::mutex> l(g_lock);
		Entry& e = g_entries[key];
		e.checked = now;
		e.size = size;
		e.time = time;
		if (!error.empty()) {
			++g_stats.failures;
			if (e.lastError != error) {
				spdlog::warn("Config: {} could not be read, the last good version stays: {}", path.string(), error);
				e.lastError = error;
			}
			g_stats.documents = g_entries.size();
			return e.doc;		// The last good one (or null)
		}
		e.lastError.clear();
		doc->version = e.doc ? e.doc->version + 1 : 1;
		e.doc = doc;
		++g_stats.parses;
		g_stats.documents = g_entries.size();
		return doc;
	}

	void Invalidate(const fs::path& path)
	{
		std::lock_guard<std::mutex> l(g_lock);
		if (auto it = g_entries.find(Key(path)); it != g_entries.end()) {
			it->second.checked = {};			// Checked next time...
			it->second.size = UINTMAX_MAX;	// ...and parsed again even if size and time look the same
		}
	}

	Stats GetStats()
	{
		std::lock_guard<std::mutex> l(g_lock);
		return g_stats;
	}
}
