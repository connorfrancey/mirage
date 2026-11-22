#pragma once
/** @file
 * @ingroup mirage_core
 * @brief File resources with shared ownership and scheduler-managed eviction.
 */
// The resource cache: files the menu and mods read (images, fonts, layouts, data files) loaded once and shared.
//
//   auto r = res::Cache().Get(path);                   // shared_ptr<const res::Resource>, nullptr if unreadable
//   res::Cache().Load(path).Then([](const auto& r) { ... }, tasks::On::RenderThread);   // read on a worker
//
// Three layers, so it is fast and still conservative with memory:
//   - live: anything someone still holds is found again (cache::Weak), whatever the budget did
//   - kept: recently used resources are held by the cache itself, least recently used first to go, under the eviction
//     manager's budget (core/cache_evict.h): memory pressure takes them, never a resource that is in use
//   - by content: two paths with the same bytes share one copy (rapidhash of the contents)
// A file that changed on disk (size or write time; checked at most every `checkEvery` seconds per file) is read again.
// Any thread (one lock; files are read outside it).
#include "core/cache.h"
#include "core/cache_evict.h"
#include "core/tasks.h"
#include "core/weak_cache.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace res
{
	struct Resource
	{
		std::filesystem::path path;
		std::vector<std::uint8_t> bytes;
		std::uint64_t hash = 0;							// rapidhash of the bytes
		std::uintmax_t fileSize = 0;
		std::filesystem::file_time_type writeTime{};
		std::string_view Text() const { return {reinterpret_cast<const char*>(bytes.data()), bytes.size()}; }
	};
	using Ref = std::shared_ptr<const Resource>;

	struct CacheStats
	{
		std::size_t kept = 0, keptBytes = 0, live = 0;
		std::uint64_t hits = 0, misses = 0, reloads = 0, shared = 0, failures = 0;
	};

	/// @ingroup mirage_core
	/// @brief Reuse immutable resource snapshots across file reads.
	/// @note Eviction drops the cache's reference. Existing owners retain their snapshots.
	class ResourceCache : public cache::Evictor::Member
	{
	public:
		explicit ResourceCache(std::string name = "Resources", double checkEvery = 1.0);

		/// @brief Find the resource or read it on the calling thread.
		/// @param path File to read.
		/// @return Shared immutable bytes or an empty reference on failure.
		Ref Get(const std::filesystem::path& path);
		tasks::Task<Ref> Load(const std::filesystem::path& path, tasks::Priority priority = tasks::Priority::Normal);
		void Invalidate(const std::filesystem::path& path);			// Read again next time
		void Clear();												// Drops what it keeps (live ones stay with their owners)
		CacheStats Stats() const;

		// cache::Evictable
		std::size_t Bytes() const override;
		std::size_t Trim(std::size_t bytes) override;
		std::uint64_t Hits() const override;

	private:
		struct Kept
		{
			Ref resource;
			std::chrono::steady_clock::time_point checked;
		};
		using Order = std::list<std::string>;					// Keys, most recently used first
		Ref Read(const std::filesystem::path& path, std::string* error) const;
		bool Stale(const Resource& r) const;
		static std::string Key(const std::filesystem::path& path);
		void KeepLocked(const std::string& key, Ref r);

		mutable std::mutex lock_;
		double checkEvery_;
		std::unordered_map<std::string, std::pair<Kept, Order::iterator>> kept_;
		Order order_;
		std::size_t keptBytes_ = 0;
		cache::Weak<std::string, const Resource> live_;
		cache::Weak<std::uint64_t, const Resource> byContent_;
		cache::Stats* stats_;
		std::uint64_t reloads_ = 0, shared_ = 0, failures_ = 0;
	};

	ResourceCache& Cache();		// The menu's
}
