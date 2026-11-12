#pragma once
#include "core/cache.h"
#include "core/cache_evict.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

// cache::Adaptive: an Adaptive Replacement Cache (ARC, Megiddo & Modha). Like cache::Lru it holds at most `capacity`
// values, but it balances itself between recency and frequency: values used once (T1) and values used again (T2) each
// have a "ghost" list of keys recently dropped (B1, B2). A miss that hits a ghost list says which side was cut too
// short, and the target size of T1 (p) moves towards it. A scan of many one-off keys can't wipe out the values that
// are used again and again (which an LRU loses), and a working set that shifts is followed without tuning.
//
//   cache::Adaptive<std::string, Mesh> meshes{"Meshes", 256};
//   const Mesh& m = meshes.Get(key, [&] { return Build(key); });
//
// Get/Find are O(1). One owner thread, like cache::Lru; under the eviction manager (it gives back T1/T2 values the
// way ARC would replace them). Ghost lists hold keys only.
namespace cache
{
	template <class K, class V, class Hash = std::hash<K>, class Eq = std::equal_to<K>>
	class Adaptive : public Evictor::Member
	{
	public:
		Adaptive(std::string name, std::size_t capacity)
			: Evictor::Member(name), stats_(&Register(std::move(name), Scope::Adaptive)), c_(std::max<std::size_t>(capacity, 1))
		{}

		static constexpr std::size_t kEntryBytes = sizeof(std::pair<K, V>) + sizeof(K) + 8 * sizeof(void*);
		std::size_t Bytes() const override { return (t1_.size() + t2_.size()) * kEntryBytes + (b1_.size() + b2_.size()) * (sizeof(K) + 6 * sizeof(void*)); }
		std::size_t Trim(std::size_t bytes) override
		{
			std::size_t freed = 0;
			while (freed < bytes && (!t1_.empty() || !t2_.empty())) {
				Replace(false);
				freed += kEntryBytes;
			}
			while (freed < bytes && (!b1_.empty() || !b2_.empty())) {		// Then the ghosts' keys
				DropGhost(b1_.size() >= b2_.size() ? b1_ : b2_);
				freed += sizeof(K) + 6 * sizeof(void*);
			}
			UpdateSize();
			return freed;
		}
		std::uint64_t Hits() const override { return stats_->hits.load(std::memory_order_relaxed); }

		template <class F>
		V& Get(const K& key, F&& compute)
		{
			if (V* v = Lookup(key)) {
				stats_->hits.fetch_add(1, std::memory_order_relaxed);
				return *v;
			}
			V value = Compute(*stats_, std::forward<F>(compute));
			return Admit(key, std::move(value));
		}

		V* Find(const K& key)
		{
			V* v = Lookup(key);
			(v ? stats_->hits : stats_->misses).fetch_add(1, std::memory_order_relaxed);
			return v;
		}

		bool Contains(const K& key) const
		{
			const auto it = index_.find(key);
			return it != index_.end() && (it->second.list == L::T1 || it->second.list == L::T2);
		}

		void Clear()
		{
			t1_.clear(); t2_.clear(); b1_.clear(); b2_.clear(); index_.clear();
			p_ = 0;
			stats_->invalidations.fetch_add(1, std::memory_order_relaxed);
			UpdateSize();
		}

		std::size_t Size() const { return t1_.size() + t2_.size(); }
		std::size_t Capacity() const { return c_; }
		std::size_t RecencyTarget() const { return p_; }		// ARC's p: the share of capacity for once-used values
		std::size_t Recent() const { return t1_.size(); }
		std::size_t Frequent() const { return t2_.size(); }

	private:
		enum class L : std::uint8_t { T1, T2, B1, B2 };
		using Entry = std::pair<K, V>;
		using Values = std::list<Entry>;
		using Keys = std::list<K>;
		struct Where
		{
			L list;
			typename Values::iterator value;	// T1/T2
			typename Keys::iterator ghost;		// B1/B2
		};

		V* Lookup(const K& key)
		{
			if (!opt::kEnabled) return nullptr;
			auto it = index_.find(key);
			if (it == index_.end() || it->second.list == L::B1 || it->second.list == L::B2) return nullptr;
			// A hit in T1 or T2: now frequently used, the most recent of T2
			Values& from = it->second.list == L::T1 ? t1_ : t2_;
			t2_.splice(t2_.begin(), from, it->second.value);
			it->second.list = L::T2;
			return &it->second.value->second;
		}

		V& Admit(const K& key, V value)
		{
			auto it = index_.find(key);
			if (it != index_.end() && (it->second.list == L::B1 || it->second.list == L::B2)) {
				// A ghost hit: that side was cut too short. Move p towards it, make room, and keep the value as frequent.
				const bool inB1 = it->second.list == L::B1;
				if (inB1) p_ = std::min(c_, p_ + std::max<std::size_t>(b2_.size() / std::max<std::size_t>(b1_.size(), 1), 1));
				else p_ -= std::min(p_, std::max<std::size_t>(b1_.size() / std::max<std::size_t>(b2_.size(), 1), 1));
				(inB1 ? b1_ : b2_).erase(it->second.ghost);
				index_.erase(it);
				if (t1_.size() + t2_.size() >= c_) Replace(!inB1);
				t2_.emplace_front(key, std::move(value));
				index_[key] = {L::T2, t2_.begin(), {}};
				UpdateSize();
				return t2_.front().second;
			}
			if (it != index_.end()) {			// Already held (a compute that raced a Put): replace the value
				it->second.value->second = std::move(value);
				return it->second.value->second;
			}
			// New: make room as ARC does
			const std::size_t l1 = t1_.size() + b1_.size();
			const std::size_t total = l1 + t2_.size() + b2_.size();
			if (l1 >= c_) {
				if (t1_.size() < c_) {
					DropGhost(b1_);
					Replace(false);
				} else {
					DropValue(t1_);
				}
			} else if (total >= c_) {
				if (total >= 2 * c_) DropGhost(b2_);
				if (t1_.size() + t2_.size() >= c_) Replace(false);
			}
			t1_.emplace_front(key, std::move(value));
			index_[key] = {L::T1, t1_.begin(), {}};
			UpdateSize();
			return t1_.front().second;
		}

		// Moves the LRU value of T1 or T2 to its ghost list (T1 when it is over its target p)
		void Replace(bool inB2)
		{
			const bool fromT1 = !t1_.empty() && (t1_.size() > p_ || (inB2 && t1_.size() == p_) || t2_.empty());
			Values& from = fromT1 ? t1_ : t2_;
			if (from.empty()) return;
			Keys& ghosts = fromT1 ? b1_ : b2_;
			const K key = from.back().first;
			from.pop_back();
			ghosts.push_front(key);
			index_[key] = {fromT1 ? L::B1 : L::B2, {}, ghosts.begin()};
			stats_->evictions.fetch_add(1, std::memory_order_relaxed);
		}

		void DropValue(Values& list)
		{
			if (list.empty()) return;
			index_.erase(list.back().first);
			list.pop_back();
			stats_->evictions.fetch_add(1, std::memory_order_relaxed);
		}

		void DropGhost(Keys& list)
		{
			if (list.empty()) return;
			index_.erase(list.back());
			list.pop_back();
		}

		void UpdateSize() { stats_->entries.store(t1_.size() + t2_.size(), std::memory_order_relaxed); }

		Stats* stats_;
		std::size_t c_, p_ = 0;
		Values t1_, t2_;		// Front: most recent
		Keys b1_, b2_;
		std::unordered_map<K, Where, Hash, Eq> index_;
	};
}
