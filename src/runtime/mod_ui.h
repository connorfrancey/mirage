#pragma once
// The mod API's UI layer (MirageUi, api/mirage/api.h): the menu's ImGui through a C table. Render thread.
#include "mirage/api.h"

#include <imgui.h>

namespace mirage::mods::ui
{
	const MirageUi* Table();
	ImGuiKey KeyFromVk(int vk);		// A virtual-key code as ImGui's key (ImGuiKey_None: not one it has)
	bool MenuVisible();				// The menu is open (mods.cpp)

	// Around a mod's render-thread callback: what it left open (a window, a color, an ID) is closed after it, so a
	// mistake in a mod never breaks the menu's own frame
	class Guard
	{
	public:
		Guard();
		~Guard();
		Guard(const Guard&) = delete;
		Guard& operator=(const Guard&) = delete;

	private:
		void* state_ = nullptr;
		int windows_ = 0, colors_ = 0;
	};
}
