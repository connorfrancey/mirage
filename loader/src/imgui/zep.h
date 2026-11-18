#pragma once
// Zep's ImGui backend (header-only, zep/imgui/*.h) against the menu's ImGui. ImGui 1.92 renamed ImFont::FontSize to
// LegacySize; the backend reads it once, into a variable it doesn't use.
// shortcut: a scoped macro over the include; drop it when Zep's backend follows ImGui 1.92.
#include <imgui.h>

#define FontSize LegacySize
#include <zep/imgui/editor_imgui.h>
#undef FontSize
#include <zep/range_markers.h>
