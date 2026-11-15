#include "runtime/detours.h"

#include "core/hooking.h"
#include "runtime/detour_guard.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>
#include <mutex>

namespace mirage::detours
{
	namespace
	{
		struct Entry
		{
			int id;
			void* detour;
			int priority;
			int order;
			void** original;
			std::string owner;
			void* thunk;					// The guard in front of the detour (detour_guard.h): what the chain calls
		};
		struct Target
		{
			std::vector<Entry> chain;		// Highest priority first, then oldest
			void* trampoline = nullptr;		// The hook's way to the real function
		};

		std::mutex g_lock;
		std::map<void*, Target> g_targets;
		std::map<int, std::pair<std::string, void*>> g_owners;		// Every detour's owner and target, by id
		int g_nextId = 1, g_order = 0;

		std::string Describe(const Target& t)
		{
			std::string s;
			for (const Entry& e : t.chain) s += (s.empty() ? "" : " -> ") + e.owner + " (" + std::to_string(e.priority) + ")";
			return s;
		}

		// The hook on `target` to the chain's first detour, every *original set: a detour's to the next, the last's to
		// the real function. "" when done.
		std::string Relink(void* target, Target& t)
		{
			// A hook can't be moved to another detour: it is made again (the trampoline changes with it)
			if (t.trampoline) {
				hooking::Remove(target);
				t.trampoline = nullptr;
			}
			if (t.chain.empty()) return {};
			void* trampoline = nullptr;
			const hooking::Status created = hooking::Create(target, t.chain.front().thunk, &trampoline, "mod detour");
			if (!created) return created.alreadyHooked ? "the loader hooks this function itself" : created.error;
			t.trampoline = trampoline;
			for (std::size_t i = 0; i < t.chain.size(); ++i)
				*t.chain[i].original = i + 1 < t.chain.size() ? t.chain[i + 1].thunk : trampoline;
			if (const hooking::Status enabled = hooking::Enable(target); !enabled) {
				hooking::Remove(target);
				t.trampoline = nullptr;
				return enabled.error;
			}
			return {};
		}
	}

	int Add(void* target, void* detour, int priority, void** original, const std::string& owner, std::string* error)
	{
		if (!target || !detour || !original) {
			if (error) *error = "a target, a detour and a place for the original are needed";
			return 0;
		}
		std::lock_guard lock(g_lock);
		Target& t = g_targets[target];
		if (std::any_of(t.chain.begin(), t.chain.end(), [&](const Entry& e) { return e.detour == detour; })) {
			if (error) *error = "this detour is on the target already";
			return 0;
		}
		const Target before = t;
		const int id = g_nextId++;
		void* thunk = guard::Make(detour, original, id);
		if (!thunk) {
			if (error) *error = "no memory for the detour's guard";
			if (t.chain.empty()) g_targets.erase(target);
			return 0;
		}
		t.chain.push_back({id, detour, priority, g_order++, original, owner, thunk});
		g_owners[id] = {owner, target};
		std::stable_sort(t.chain.begin(), t.chain.end(), [](const Entry& a, const Entry& b) {
			return a.priority != b.priority ? a.priority > b.priority : a.order < b.order;
		});
		if (const std::string why = Relink(target, t); !why.empty()) {
			t.chain = before.chain;
			Relink(target, t);
			if (t.chain.empty()) g_targets.erase(target);
			if (error) *error = why;
			spdlog::error("Detours: {} can't detour {}: {}", owner, target, why);
			return 0;
		}
		if (t.chain.size() > 1)
			spdlog::warn("Detours: {} mods detour {}: {} (in that order)", t.chain.size(), target, Describe(t));
		else
			spdlog::info("Detours: {} detours {}", owner, target);
		return id;
	}

	void Remove(int id)
	{
		std::lock_guard lock(g_lock);
		for (auto it = g_targets.begin(); it != g_targets.end(); ++it) {
			Target& t = it->second;
			const auto e = std::find_if(t.chain.begin(), t.chain.end(), [&](const Entry& x) { return x.id == id; });
			if (e == t.chain.end()) continue;
			spdlog::info("Detours: {} removed its detour of {}", e->owner, it->first);
			guard::Disable(e->thunk);
			*e->original = nullptr;
			t.chain.erase(e);
			if (const std::string why = Relink(it->first, t); !why.empty())
				spdlog::error("Detours: the chain on {} couldn't be made again: {}", it->first, why);
			if (t.chain.empty()) g_targets.erase(it);
			return;
		}
	}

	void RemoveAll()
	{
		std::lock_guard lock(g_lock);
		for (auto& [target, t] : g_targets) {
			t.chain.clear();
			Relink(target, t);
		}
		g_targets.clear();
	}

	std::vector<std::string> Chain(void* target)
	{
		std::lock_guard lock(g_lock);
		std::vector<std::string> out;
		if (const auto it = g_targets.find(target); it != g_targets.end())
			for (const Entry& e : it->second.chain) out.push_back(e.owner + " (" + std::to_string(e.priority) + ")");
		return out;
	}

	std::vector<Fault> TakeFaults()
	{
		std::vector<Fault> out;
		const auto faults = guard::TakeFaults();
		if (faults.empty()) return out;
		std::lock_guard lock(g_lock);
		for (const guard::Fault& f : faults) {
			Fault x;
			x.id = f.id;
			x.address = f.address;
			x.code = f.code;
			if (const auto it = g_owners.find(f.id); it != g_owners.end()) {
				x.owner = it->second.first;
				x.target = it->second.second;
			}
			out.push_back(std::move(x));
		}
		return out;
	}
}
