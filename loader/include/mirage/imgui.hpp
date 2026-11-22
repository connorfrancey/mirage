#pragma once
/** @file
 * @ingroup mirage_cpp_api
 * @brief ImGui context binding for native Mirage mods.
 */
// The menu's ImGui in a native mod built against the same ImGui version (MirageApi::imgui_context): for UI beyond
// MirageUi. Call mirage::UseImGui() at the top of each render-thread callback (on_render, on_panel) and draw with
// ImGui as usual while it returns true. It ties the mod to the menu's ImGui version: the runtime refuses another one.
#include "mirage/mirage.hpp"

#include <imgui.h>

namespace mirage
{
	/// @brief Select the loader's ImGui context and allocators for this mod.
	/// @return True when ImGui is ready to draw, or false when the loader rejects or cannot supply the context.
	/// @warning Call from each render-thread UI callback. The loader owns the context.
	/// @see MirageApi::imgui_context
	inline bool UseImGui()
	{
		static ImGuiContext* ctx = nullptr;
		static ImGuiMemAllocFunc alloc = nullptr;
		static ImGuiMemFreeFunc release = nullptr;
		static void* user = nullptr;
		if (!ctx) {
			void* a = nullptr;
			void* f = nullptr;
			ctx = static_cast<ImGuiContext*>(Api().imgui_context(IMGUI_VERSION, sizeof(ImGuiIO), sizeof(ImGuiStyle),
																 sizeof(ImDrawVert), &a, &f, &user));
			if (!ctx) return false;
			alloc = reinterpret_cast<ImGuiMemAllocFunc>(a);
			release = reinterpret_cast<ImGuiMemFreeFunc>(f);
		}
		ImGui::SetAllocatorFunctions(alloc, release, user);
		ImGui::SetCurrentContext(ctx);
		return true;
	}
}
