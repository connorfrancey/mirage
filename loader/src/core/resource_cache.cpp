#include "core/resource_cache.h"

#include "core/hash.h"

#include <spdlog/spdlog.h>

#include <cctype>
#include <fstream>

namespace res
{
	namespace fs = std::filesystem;

	ResourceCache::ResourceCache(std::string name, double checkEvery)
		: cache::Evictor::Member(name), checkEvery_(checkEvery), live_(name + " (live)"), byContent_(name + " (by content)"),
		  stats_(&cache::Register(name, cache::Scope::Lru))
	{}

	std::string ResourceCache::Key(const fs::path& path)
	{
		std::error_code ec;
		fs::path p = fs::weakly_canonical(path, ec);
		if (ec) p = path;
		auto u = p.generic_u8string();
		std::string key(u.begin(), u.end());
		for (char& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));	// Windows paths
		return key;
	}

	Ref ResourceCache::Read(const fs::path& path, std::string* error) const
	{
		std::error_code ec;
		auto r = std::make_shared<Resource>();
		r->path = path;
		r->fileSize = fs::file_size(path, ec);
		if (ec) { if (error) *error = ec.message(); return nullptr; }
		r->writeTime = fs::last_write_time(path, ec);
		std::ifstream in(path, std::ios::binary);
		if (!in) { if (error) *error = "can't open"; return nullptr; }
		r->bytes.resize(static_cast<std::size_t>(r->fileSize));
		if (r->fileSize && !in.read(reinterpret_cast<char*>(r->bytes.data()), static_cast<std::streamsize>(r->fileSize))) {
			if (error) *error = "read failed";
			return nullptr;
		}
		r->hash = hash::Bytes(r->bytes.data(), r->bytes.size());
		return r;
	}

	bool ResourceCache::Stale(const Resource& r) const
	{
		std::error_code ec;
		const auto size = fs::file_size(r.path, ec);
		if (ec) return true;
		const auto time = fs::last_write_time(r.path, ec);
		return ec || size != r.fileSize || time != r.writeTime;
	}

	void ResourceCache::KeepLocked(const std::string& key, Ref r)
	{
		if (auto it = kept_.find(key); it != kept_.end()) {
			keptBytes_ -= it->second.first.resource->bytes.size();
			order_.erase(it->second.second);
			kept_.erase(it);
		}
		order_.push_front(key);
		keptBytes_ += r->bytes.size();
		kept_.emplace(key, std::make_pair(Kept{std::move(r), std::chrono::steady_clock::now()}, order_.begin()));
		stats_->entries.store(kept_.size(), std::memory_order_relaxed);
	}

	Ref ResourceCache::Get(const fs::path& path)
	{
		const std::string key = Key(path);
		const auto now = std::chrono::steady_clock::now();
		Ref found;
		bool check = false;
		{
			std::lock_guard<std::mutex> l(lock_);
			if (auto it = kept_.find(key); it != kept_.end()) {
				Kept& k = it->second.first;
				order_.splice(order_.begin(), order_, it->second.second);
				found = k.resource;
				if (std::chrono::duration<double>(now - k.checked).count() >= checkEvery_) {
					check = true;
					k.checked = now;
				}
			}
		}
		if (!found) found = live_.Find(key);		// Evicted but still in use somewhere
		if (found && (!check || !Stale(*found))) {
			stats_->hits.fetch_add(1, std::memory_order_relaxed);
			std::lock_guard<std::mutex> l(lock_);
			if (!kept_.count(key)) KeepLocked(key, found);
			return found;
		}
		// Read it (outside the lock: files may be slow)
		const bool reload = static_cast<bool>(found);
		std::string error;
		Ref fresh = cache::Compute(*stats_, [&] { return Read(path, &error); });
		std::lock_guard<std::mutex> l(lock_);
		if (!fresh) {
			++failures_;
			if (auto it = kept_.find(key); it != kept_.end()) {		// Gone or unreadable: not kept any more
				keptBytes_ -= it->second.first.resource->bytes.size();
				order_.erase(it->second.second);
				kept_.erase(it);
			}
			return nullptr;
		}
		if (reload) ++reloads_;
		// The same bytes under another path: share that copy (its path is the first one it was read from)
		if (Ref same = byContent_.Find(fresh->hash); same && same->bytes == fresh->bytes) {
			++shared_;
			fresh = same;
		} else {
			byContent_.Put(fresh->hash, fresh);
		}
		live_.Put(key, fresh);
		KeepLocked(key, fresh);
		return fresh;
	}

	tasks::Task<Ref> ResourceCache::Load(const fs::path& path, tasks::Priority priority)
	{
		return tasks::Run([this, path] { return Get(path); }, {priority, "Load " + path.filename().string()});
	}

	void ResourceCache::Invalidate(const fs::path& path)
	{
		const std::string key = Key(path);
		live_.Erase(key);
		std::lock_guard<std::mutex> l(lock_);
		if (auto it = kept_.find(key); it != kept_.end()) {
			keptBytes_ -= it->second.first.resource->bytes.size();
			order_.erase(it->second.second);
			kept_.erase(it);
			stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
		}
	}

	void ResourceCache::Clear()
	{
		std::lock_guard<std::mutex> l(lock_);
		kept_.clear();
		order_.clear();
		keptBytes_ = 0;
		stats_->entries.store(0, std::memory_order_relaxed);
	}

	CacheStats ResourceCache::Stats() const
	{
		CacheStats s;
		{
			std::lock_guard<std::mutex> l(lock_);
			s.kept = kept_.size();
			s.keptBytes = keptBytes_;
			s.reloads = reloads_;
			s.shared = shared_;
			s.failures = failures_;
		}
		s.live = live_.Alive();
		s.hits = stats_->hits.load(std::memory_order_relaxed);
		s.misses = stats_->misses.load(std::memory_order_relaxed);
		return s;
	}

	std::size_t ResourceCache::Bytes() const
	{
		std::lock_guard<std::mutex> l(lock_);
		return keptBytes_;
	}

	std::size_t ResourceCache::Trim(std::size_t bytes)
	{
		std::lock_guard<std::mutex> l(lock_);
		std::size_t freed = 0;
		while (freed < bytes && !order_.empty()) {
			const std::string key = order_.back();
			order_.pop_back();
			if (auto it = kept_.find(key); it != kept_.end()) {
				freed += it->second.first.resource->bytes.size();
				keptBytes_ -= it->second.first.resource->bytes.size();
				kept_.erase(it);		// Still found through `live_` while someone holds it
				stats_->evictions.fetch_add(1, std::memory_order_relaxed);
			}
		}
		stats_->entries.store(kept_.size(), std::memory_order_relaxed);
		return freed;
	}

	std::uint64_t ResourceCache::Hits() const { return stats_->hits.load(std::memory_order_relaxed); }

	ResourceCache& Cache()
	{
		static ResourceCache* c = new ResourceCache();		// Never destroyed (the eviction manager may still list it)
		return *c;
	}
}
