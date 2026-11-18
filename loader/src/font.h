#pragma once
#include <cstddef>

struct ImFont;

// The menu's fonts, built into the DLL (cmake/Fonts.cmake generates the data). A size is 0 when the build had no such
// font: ImGui's built-in one is used then.
namespace font {

const unsigned char* MenuData();
std::size_t MenuSize();

// Monospace (Cousine): the console. Mono() is the loaded font (hooks.cpp), or ImGui's built-in one, which is
// monospace too; nullptr before ImGui is set up.
const unsigned char* MonoData();
std::size_t MonoSize();
inline ImFont* g_mono = nullptr;
inline ImFont* Mono() { return g_mono; }

// Font Awesome 6 Free Solid: merged into the menu font, so ICON_FA_* (IconsFontAwesome6.h) can be put in any label;
// Icons() is true when it was. Without it the build had no icon font: labels with icons show their text only.
const unsigned char* IconData();
// JetBrains Mono Nerd Font (loader/fonts/): the script editor's. Editor() is the loaded font, or Mono() without it.
const unsigned char* EditorData();
std::size_t EditorSize();
inline ImFont* g_editor = nullptr;
inline ImFont* Editor() { return g_editor ? g_editor : g_mono; }
std::size_t IconSize();
inline bool g_icons = false;
inline bool Icons() { return g_icons; }

} // namespace font
