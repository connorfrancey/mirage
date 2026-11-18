#include "mod_profile.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>
#include <mutex>

namespace mirage::mods::profile
{
	namespace
	{
		struct Window
		{
			Callback numbers;
			double windowMs = 0.0;				// In the second being counted
		};
		struct ModData
		{
			std::map<std::string, Window, std::less<>> callbacks;
			double windowMs = 0.0, msPerTick = 0.0;
			bool overBudget = false;
		};
		std::mutex g_lock;
		std::map<std::string, ModData, std::less<>> g_mods;
		double g_windowStart = -1.0;
		int g_ticks = 0;						// Engine ticks in the second being counted
	}

	void Record(std::string_view mod, std::string_view what, double ms)
	{
		std::lock_guard lock(g_lock);
		auto m = g_mods.find(mod);
		if (m == g_mods.end()) m = g_mods.emplace(std::string(mod), ModData{}).first;
		auto c = m->second.callbacks.find(what);
		if (c == m->second.callbacks.end()) {
			c = m->second.callbacks.emplace(std::string(what), Window{}).first;
			c->second.numbers.what = what;
		}
		Callback& n = c->second.numbers;
		++n.calls;
		n.totalMs += ms;
		n.maxMs = std::max(n.maxMs, ms);
		c->second.windowMs += ms;
		m->second.windowMs += ms;
	}

	void Tick(double seconds)
	{
		std::lock_guard lock(g_lock);
		if (g_windowStart < 0.0 || seconds < g_windowStart) {	// The first tick (or the clock started over): a second starts
			g_windowStart = seconds;
			g_ticks = 0;
			return;
		}
		++g_ticks;
		if (seconds - g_windowStart < 1.0) return;
		for (auto& [id, m] : g_mods) {
			m.msPerTick = m.windowMs / g_ticks;
			const bool over = m.msPerTick > kBudgetMs;
			if (over && !m.overBudget) {
				const auto slowest = std::max_element(m.callbacks.begin(), m.callbacks.end(),
													  [](const auto& a, const auto& b) { return a.second.windowMs < b.second.windowMs; });
				spdlog::warn("[mod {}] over its frame budget: {:.2f} ms of its code per engine tick (budget {:.1f} ms), most in {} "
							 "({:.2f} ms per tick)", id, m.msPerTick, kBudgetMs, slowest->first, slowest->second.windowMs / g_ticks);
			}
			m.overBudget = over;
			for (auto& [what, c] : m.callbacks) {
				c.numbers.lastSecondMs = c.windowMs;
				c.windowMs = 0.0;
			}
			m.windowMs = 0.0;
		}
		g_windowStart = seconds;
		g_ticks = 0;
	}

	std::vector<Mod> List()
	{
		std::vector<Mod> out;
		{
			std::lock_guard lock(g_lock);
			for (const auto& [id, m] : g_mods) {
				Mod& o = out.emplace_back();
				o.id = id;
				o.msPerTick = m.msPerTick;
				o.overBudget = m.overBudget;
				for (const auto& [what, c] : m.callbacks) o.callbacks.push_back(c.numbers);
			}
		}
		for (Mod& m : out)
			std::sort(m.callbacks.begin(), m.callbacks.end(), [](const Callback& a, const Callback& b) {
				return a.lastSecondMs != b.lastSecondMs ? a.lastSecondMs > b.lastSecondMs : a.totalMs > b.totalMs;
			});
		std::sort(out.begin(), out.end(), [](const Mod& a, const Mod& b) { return a.msPerTick > b.msPerTick; });
		return out;
	}

	void Forget(std::string_view mod)
	{
		std::lock_guard lock(g_lock);
		if (const auto it = g_mods.find(mod); it != g_mods.end()) g_mods.erase(it);
	}

	void Reset()
	{
		std::lock_guard lock(g_lock);
		g_mods.clear();
		g_windowStart = -1.0;
		g_ticks = 0;
	}
}
