#pragma once
#include "core/cache.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// cache::Weak: shared values found by key while anyone still holds them, without the cache keeping them alive. Two
// callers asking for the same resource get the same object; when the last owner lets go, it is gone and the next
// request makes it again. Holds no memory of its own worth budgeting (dead entries are swept as it goes), so it is
// not under the eviction manager: the owners decide how long things live.
//
//   cache::Weak<std::string, Texture> textures{"Textures (shared)"};
//   std::shared_ptr<Texture> t = textures.Get(path, [&] { return std::make_shared<Texture>(Load(path)); });
//
// Locked: any thread. The make function runs under the lock (so a value is made once), keep it short or make the
// value outside and Put it.
namespace cache
{
	template <class K, class V, class Hash = std::hash<K>, class Eq = std::equal_to<K>>
	class Weak
	{
	public:
		explicit Weak(std::string name) : stats_(&Register(std::move(name), Scope::Weak)) {}

		// The live value for `key`, made with `make()` (returns a shared_ptr<V>, may be null) when there is none
		template <class F>
		std::shared_ptr<V> Get(const K& key, F&& make)
		{
			std::lock_guard<std::mutex> l(lock_);
			if (auto v = LookupLocked(key)) {
				stats_->hits.fetch_add(1, std::memory_order_relaxed);
				return v;
			}
			std::shared_ptr<V> v = Compute(*stats_, std::forward<F>(make));
			if (v) StoreLocked(key, v);
			return v;
		}

		std::shared_ptr<V> Find(const K& key)
		{
			std::lock_guard<std::mutex> l(lock_);
			auto v = LookupLocked(key);
			(v ? stats_->hits : stats_->misses).fetch_add(1, std::memory_order_relaxed);
			return v;
		}

		// Makes `value` the one for `key` (another live value for it is no longer found here)
		void Put(const K& key, const std::shared_ptr<V>& value)
		{
			std::lock_guard<std::mutex> l(lock_);
			StoreLocked(key, value);
		}

		bool Erase(const K& key)
		{
			std::lock_guard<std::mutex> l(lock_);
			const bool erased = map_.erase(key) > 0;
			UpdateSize();
			return erased;
		}

		// Drops entries whose value is gone; returns how many
		std::size_t Sweep()
		{
			std::lock_guard<std::mutex> l(lock_);
			return SweepLocked();
		}

		std::size_t Size() const				// Entries, dead ones included until swept
		{
			std::lock_guard<std::mutex> l(lock_);
			return map_.size();
		}
		std::size_t Alive() const
		{
			std::lock_guard<std::mutex> l(lock_);
			std::size_t n = 0;
			for (const auto& [k, w] : map_) n += !w.expired();
			return n;
		}

	private:
		std::shared_ptr<V> LookupLocked(const K& key)
		{
			const auto it = map_.find(key);
			if (it == map_.end()) return nullptr;
			if (auto v = it->second.lock()) return v;
			map_.erase(it);			// Dead: forget it now
			stats_->evictions.fetch_add(1, std::memory_order_relaxed);
			UpdateSize();
			return nullptr;
		}

		void StoreLocked(const K& key, const std::shared_ptr<V>& v)
		{
			map_[key] = v;
			// Dead entries are swept as the map grows: amortized O(1) per insert
			if (map_.size() >= sweepAt_) {
				SweepLocked();
				sweepAt_ = std::max<std::size_t>(64, map_.size() * 2);
			}
			UpdateSize();
		}

		std::size_t SweepLocked()
		{
			std::size_t dropped = 0;
			for (auto it = map_.begin(); it != map_.end();) {
				if (it->second.expired()) {
					it = map_.erase(it);
					++dropped;
				} else {
					++it;
				}
			}
			stats_->evictions.fetch_add(dropped, std::memory_order_relaxed);
			UpdateSize();
			return dropped;
		}

		void UpdateSize() { stats_->entries.store(map_.size(), std::memory_order_relaxed); }

		Stats* stats_;
		mutable std::mutex lock_;
		std::unordered_map<K, std::weak_ptr<V>, Hash, Eq> map_;
		std::size_t sweepAt_ = 64;
	};
}
