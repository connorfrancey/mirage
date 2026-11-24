#include "runtime/actions.h"

#include "runtime/mod_game.h"
#include "runtime/mods.h"
#include "core/keys.h"
#if MIRAGE_FEATURE_INPUT
#include "tas/input_monitor.h"
#endif

#include <spdlog/spdlog.h>

#include <array>
#include <map>
#include <vector>

namespace mirage::actions
{
	namespace
	{
		struct Pending
		{
			std::string mod, action;
			bool down;
		};
		std::vector<Pending> g_pending;		// Game thread (the monitor and the tick both run there)
		unsigned g_delivered = 0;
		std::array<bool, 6> g_modifiers{};
		std::map<std::string, std::vector<std::pair<std::string, std::string>>> g_held;

		void OnKey(const std::string& name, bool down)
		{
			constexpr std::array<std::string_view, 6> modifierKeys{
				"leftcontrol", "rightcontrol", "leftshift", "rightshift", "leftalt", "rightalt"};
			for (std::size_t i = 0; i < modifierKeys.size(); ++i)
				if (name == modifierKeys[i]) g_modifiers[i] = down;
			if (down) {
				const int mods = ((g_modifiers[0] || g_modifiers[1]) ? keys::kCtrl : 0) |
					((g_modifiers[2] || g_modifiers[3]) ? keys::kShift : 0) |
					((g_modifiers[4] || g_modifiers[5]) ? keys::kAlt : 0);
				auto matches = mods::ActionsForKey(name, mods);
				for (const auto& [mod, action] : matches) g_pending.push_back({mod, action, true});
				if (!matches.empty()) g_held.insert_or_assign(name, std::move(matches));
			} else if (auto held = g_held.extract(name); !held.empty()) {
				for (auto& [mod, action] : held.mapped())
					g_pending.push_back({std::move(mod), std::move(action), false});
			}
		}
	}

	void Install()
	{
#if MIRAGE_FEATURE_INPUT
		tas::input::SetKeyListener(&OnKey);
#endif
	}

	void Tick()
	{
		if (g_pending.empty()) return;
		std::vector<Pending> now;
		now.swap(g_pending);
		for (const Pending& p : now) {
			mods::DispatchAction(p.mod, p.action, p.down);
			++g_delivered;
		}
	}

	unsigned Delivered() { return g_delivered; }
}
