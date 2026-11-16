#include "runtime/actions.h"

#include "runtime/mod_game.h"
#include "runtime/mods.h"
#if MIRAGE_FEATURE_INPUT
#include "tas/input_monitor.h"
#endif

#include <spdlog/spdlog.h>

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

		void OnKey(const std::string& name, bool down)
		{
			for (auto& [mod, action] : mods::ActionsForKey(name)) g_pending.push_back({std::move(mod), std::move(action), down});
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
