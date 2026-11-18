#include "theme.h"
#include "paths.h"

#include <Windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <spdlog/spdlog.h>
#include <toml.hpp>

namespace fs = std::filesystem;

namespace {

constexpr const char* kDefaultTheme = "MirrorsEdge";
const ImVec4 kRed{0.90f, 0.20f, 0.18f, 1.00f};

struct Theme {
    ImGuiStyle style;            // only the colors/rounding listed below are used
    theme::MenuColors menu;
};

// The structural style (padding/spacing) stays fixed: the clickgui layout relies on it, so themes
// can only change colors and rounding.
struct FloatVar { const char* key; float ImGuiStyle::* member; ImGuiStyleVar var; };
constexpr FloatVar kRounding[] = {
    {"WindowRounding",    &ImGuiStyle::WindowRounding,    ImGuiStyleVar_WindowRounding},
    {"ChildRounding",     &ImGuiStyle::ChildRounding,     ImGuiStyleVar_ChildRounding},
    {"PopupRounding",     &ImGuiStyle::PopupRounding,     ImGuiStyleVar_PopupRounding},
    {"FrameRounding",     &ImGuiStyle::FrameRounding,     ImGuiStyleVar_FrameRounding},
    {"ScrollbarRounding", &ImGuiStyle::ScrollbarRounding, ImGuiStyleVar_ScrollbarRounding},
    {"GrabRounding",      &ImGuiStyle::GrabRounding,      ImGuiStyleVar_GrabRounding},
    {"TabRounding",       &ImGuiStyle::TabRounding,       ImGuiStyleVar_TabRounding},
};

struct MenuColorVar { const char* key; ImVec4 theme::MenuColors::* member; };
constexpr MenuColorVar kMenuColors[] = {
    {"accent",      &theme::MenuColors::accent},
    {"row_bg",      &theme::MenuColors::rowBg},
    {"row_hover",   &theme::MenuColors::rowHover},
    {"text",        &theme::MenuColors::text},
    {"text_active", &theme::MenuColors::textActive},
    {"text_dim",    &theme::MenuColors::textDim},
    {"settings_bg", &theme::MenuColors::settingsBg},
};

// Colors written into generated theme files: the classic, long-stable ImGuiCol names the menu
// actually uses. (Any ImGuiCol name is accepted when *reading* a file.)
constexpr const char* kExportedColors[] = {
    "Text", "TextDisabled", "WindowBg", "PopupBg", "Border",
    "TitleBg", "TitleBgActive", "TitleBgCollapsed",
    "FrameBg", "FrameBgHovered", "FrameBgActive",
    "CheckMark", "SliderGrab", "SliderGrabActive",
    "Button", "ButtonHovered", "ButtonActive",
    "Header", "HeaderHovered", "HeaderActive", "Separator",
};

// What a theme file gives, validated: pushed onto ImGui's style stack every frame
struct Loaded {
    std::vector<std::pair<ImGuiCol, ImVec4>>     colors;
    std::vector<std::pair<ImGuiStyleVar, float>> vars;
    theme::MenuColors menu;
};

std::vector<std::string> g_names;
std::string              g_current = kDefaultTheme;
Loaded                   g_theme;
int                      g_pushedColors = 0, g_pushedVars = 0;

fs::path Dir() { return paths::Dir() / "themes"; }
fs::path ThemePath(const std::string& name) { return Dir() / (name + ".toml"); }

// ---- colors ---------------------------------------------------------------------------------
ImVec4 Mix(ImVec4 a, ImVec4 b, float t)
{
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
}
ImVec4 Scale(ImVec4 c, float f) { return { c.x * f, c.y * f, c.z * f, c.w }; }

int ColorIndex(const std::string& name)
{
    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        if (name == ImGui::GetStyleColorName(i)) return i;
    return -1;
}

// "#RRGGBB" or "#RRGGBBAA" (the '#' is optional)
bool ParseHex(std::string s, ImVec4& out)
{
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8) return false;
    for (char ch : s) if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
    std::uint32_t v = static_cast<std::uint32_t>(std::stoul(s, nullptr, 16));
    if (s.size() == 6) v = (v << 8) | 0xFFu;
    out = { static_cast<float>((v >> 24) & 0xFF) / 255.f, static_cast<float>((v >> 16) & 0xFF) / 255.f,
            static_cast<float>((v >> 8)  & 0xFF) / 255.f, static_cast<float>( v        & 0xFF) / 255.f };
    return true;
}

// A TOML number, integer or float
bool ToFloat(const toml::value& v, float& out)
{
    if (v.is_floating()) out = static_cast<float>(v.as_floating());
    else if (v.is_integer()) out = static_cast<float>(v.as_integer());
    else return false;
    return true;
}

// [r,g,b] / [r,g,b,a] (0..1) or a hex string
bool ToColor(const toml::value& v, ImVec4& out)
{
    if (v.is_string()) return ParseHex(v.as_string(), out);
    if (!v.is_array() || (v.size() != 3 && v.size() != 4)) return false;
    float c[4] = {0.f, 0.f, 0.f, 1.f};
    for (size_t i = 0; i < v.size(); ++i) {
        if (!ToFloat(v.as_array()[i], c[i])) return false;
        c[i] = std::clamp(c[i], 0.f, 1.f);
    }
    out = {c[0], c[1], c[2], c[3]};
    return true;
}

// ---- built-in themes ------------------------------------------------------------------------
void CommonStyle(ImGuiStyle& s)
{
    s.WindowRounding   = 4.f;
    s.PopupRounding    = 4.f;
    s.FrameRounding    = 3.f;
    s.GrabRounding     = 3.f;
    s.WindowBorderSize = 0.f;
    s.FramePadding     = {6.f, 4.f};
    s.WindowTitleAlign = {0.5f, 0.5f};
}

Theme MakeDark(ImVec4 accent)
{
    Theme t;
    ImGui::StyleColorsDark(&t.style);
    CommonStyle(t.style);

    ImVec4* c = t.style.Colors;
    c[ImGuiCol_WindowBg]         = {0.07f, 0.07f, 0.08f, 0.96f};
    c[ImGuiCol_PopupBg]          = {0.09f, 0.09f, 0.10f, 0.98f};
    c[ImGuiCol_TitleBg]          = Scale(accent, 0.60f);
    c[ImGuiCol_TitleBgCollapsed] = Scale(accent, 0.60f);
    c[ImGuiCol_TitleBgActive]    = accent;
    c[ImGuiCol_FrameBg]          = {0.16f, 0.16f, 0.18f, 1.f};
    c[ImGuiCol_FrameBgHovered]   = {0.22f, 0.22f, 0.25f, 1.f};
    c[ImGuiCol_FrameBgActive]    = {0.26f, 0.26f, 0.30f, 1.f};
    c[ImGuiCol_CheckMark]        = accent;
    c[ImGuiCol_SliderGrab]       = accent;
    c[ImGuiCol_SliderGrabActive] = Mix(accent, {1.f, 1.f, 1.f, 1.f}, 0.25f);
    c[ImGuiCol_Button]           = {0.16f, 0.16f, 0.18f, 1.f};
    c[ImGuiCol_ButtonHovered]    = Scale(accent, 0.60f);
    c[ImGuiCol_ButtonActive]     = accent;
    c[ImGuiCol_Header]           = Scale(accent, 0.60f);
    c[ImGuiCol_HeaderHovered]    = Scale(accent, 0.78f);
    c[ImGuiCol_HeaderActive]     = accent;

    t.menu = {};
    t.menu.accent = accent;
    return t;
}

Theme MakeLight(ImVec4 accent)
{
    Theme t;
    ImGui::StyleColorsLight(&t.style);
    CommonStyle(t.style);

    const ImVec4 white{1.f, 1.f, 1.f, 1.f};
    ImVec4* c = t.style.Colors;
    c[ImGuiCol_Text]             = {0.10f, 0.10f, 0.12f, 1.f};
    c[ImGuiCol_WindowBg]         = {0.95f, 0.95f, 0.96f, 0.97f};
    c[ImGuiCol_PopupBg]          = {0.98f, 0.98f, 0.99f, 0.98f};
    c[ImGuiCol_TitleBg]          = Mix(accent, white, 0.20f);
    c[ImGuiCol_TitleBgCollapsed] = Mix(accent, white, 0.20f);
    c[ImGuiCol_TitleBgActive]    = accent;
    c[ImGuiCol_FrameBg]          = {0.86f, 0.86f, 0.89f, 1.f};
    c[ImGuiCol_FrameBgHovered]   = {0.80f, 0.80f, 0.85f, 1.f};
    c[ImGuiCol_FrameBgActive]    = {0.74f, 0.74f, 0.80f, 1.f};
    c[ImGuiCol_CheckMark]        = accent;
    c[ImGuiCol_SliderGrab]       = accent;
    c[ImGuiCol_SliderGrabActive] = Scale(accent, 0.85f);
    c[ImGuiCol_Button]           = {0.84f, 0.84f, 0.87f, 1.f};
    c[ImGuiCol_ButtonHovered]    = Mix(accent, white, 0.35f);
    c[ImGuiCol_ButtonActive]     = accent;
    c[ImGuiCol_Header]           = Mix(accent, white, 0.35f);
    c[ImGuiCol_HeaderHovered]    = Mix(accent, white, 0.20f);
    c[ImGuiCol_HeaderActive]     = accent;

    t.menu.accent     = accent;
    t.menu.rowBg      = {0.90f, 0.90f, 0.92f, 1.f};
    t.menu.rowHover   = {0.84f, 0.84f, 0.88f, 1.f};
    t.menu.text       = {0.25f, 0.25f, 0.30f, 1.f};
    t.menu.textActive = {0.05f, 0.05f, 0.07f, 1.f};
    t.menu.textDim    = {0.45f, 0.45f, 0.50f, 1.f};
    t.menu.settingsBg = {0.93f, 0.93f, 0.95f, 1.f};
    return t;
}

// Catppuccin (https://catppuccin.com/palette, MIT): the four flavours' official colors, as 0xRRGGBB. The accent is
// Mauve, Catppuccin's default. Text stays readable on everything it can sit on: accent-filled backgrounds (buttons,
// headers, titles) are the accent mixed toward the base, never the pure pastel.
struct Catppuccin {
    std::uint32_t accent, text, subtext1, subtext0, overlay1, overlay0, surface2, surface1, surface0, base, mantle, crust;
    bool light;
};
constexpr Catppuccin kLatte     {0x8839ef, 0x4c4f69, 0x5c5f77, 0x6c6f85, 0x8c8fa1, 0x9ca0b0, 0xacb0be, 0xbcc0cc, 0xccd0da, 0xeff1f5, 0xe6e9ef, 0xdce0e8, true};
constexpr Catppuccin kFrappe    {0xca9ee6, 0xc6d0f5, 0xb5bfe2, 0xa5adce, 0x838ba7, 0x737994, 0x626880, 0x51576d, 0x414559, 0x303446, 0x292c3c, 0x232634, false};
constexpr Catppuccin kMacchiato {0xc6a0f6, 0xcad3f5, 0xb8c0e0, 0xa5adcb, 0x8087a2, 0x6e738d, 0x5b6078, 0x494d64, 0x363a4f, 0x24273a, 0x1e2030, 0x181926, false};
constexpr Catppuccin kMocha     {0xcba6f7, 0xcdd6f4, 0xbac2de, 0xa6adc8, 0x7f849c, 0x6c7086, 0x585b70, 0x45475a, 0x313244, 0x1e1e2e, 0x181825, 0x11111b, false};

ImVec4 Rgb(std::uint32_t v, float a = 1.f)
{
    return { static_cast<float>((v >> 16) & 0xFF) / 255.f, static_cast<float>((v >> 8) & 0xFF) / 255.f,
             static_cast<float>(v & 0xFF) / 255.f, a };
}

Theme MakeCatppuccin(const Catppuccin& p)
{
    Theme t;
    if (p.light) ImGui::StyleColorsLight(&t.style);
    else         ImGui::StyleColorsDark(&t.style);
    CommonStyle(t.style);

    const ImVec4 accent = Rgb(p.accent), base = Rgb(p.base);
    const auto tint = [&](float amount) { return Mix(base, accent, amount); };   // amount of accent over the base
    ImVec4* c = t.style.Colors;
    c[ImGuiCol_Text]             = Rgb(p.text);
    c[ImGuiCol_TextDisabled]     = Rgb(p.overlay0);
    c[ImGuiCol_WindowBg]         = Rgb(p.base, 0.97f);
    c[ImGuiCol_PopupBg]          = Rgb(p.mantle, 0.98f);
    c[ImGuiCol_Border]           = Rgb(p.surface1, 0.80f);
    c[ImGuiCol_TitleBg]          = Rgb(p.mantle);
    c[ImGuiCol_TitleBgCollapsed] = Rgb(p.mantle);
    c[ImGuiCol_TitleBgActive]    = tint(0.45f);
    c[ImGuiCol_FrameBg]          = Rgb(p.surface0);
    c[ImGuiCol_FrameBgHovered]   = Rgb(p.surface1);
    c[ImGuiCol_FrameBgActive]    = Rgb(p.surface2);
    c[ImGuiCol_CheckMark]        = accent;
    c[ImGuiCol_SliderGrab]       = accent;
    c[ImGuiCol_SliderGrabActive] = Mix(accent, Rgb(p.text), 0.30f);
    c[ImGuiCol_Button]           = Rgb(p.surface0);
    c[ImGuiCol_ButtonHovered]    = tint(0.35f);
    c[ImGuiCol_ButtonActive]     = tint(0.55f);
    c[ImGuiCol_Header]           = tint(0.30f);
    c[ImGuiCol_HeaderHovered]    = tint(0.40f);
    c[ImGuiCol_HeaderActive]     = tint(0.55f);
    c[ImGuiCol_Separator]        = Rgb(p.surface1);

    t.menu.accent     = accent;
    t.menu.rowBg      = Rgb(p.mantle);
    t.menu.rowHover   = Rgb(p.surface0);
    t.menu.text       = Rgb(p.subtext1);
    t.menu.textActive = Rgb(p.text);
    t.menu.textDim    = Rgb(p.overlay1);
    t.menu.settingsBg = Rgb(p.crust);
    return t;
}

std::vector<std::pair<std::string, Theme>> Builtins()
{
    return {
        {"MirrorsEdge", MakeDark({0.90f, 0.20f, 0.18f, 1.f})},
        {"Midnight",    MakeDark({0.30f, 0.52f, 1.00f, 1.f})},
        {"Emerald",     MakeDark({0.15f, 0.78f, 0.48f, 1.f})},
        {"Light",       MakeLight({0.85f, 0.25f, 0.22f, 1.f})},
        {"CatppuccinLatte",     MakeCatppuccin(kLatte)},
        {"CatppuccinFrappe",    MakeCatppuccin(kFrappe)},
        {"CatppuccinMacchiato", MakeCatppuccin(kMacchiato)},
        {"CatppuccinMocha",     MakeCatppuccin(kMocha)},
    };
}

// ---- Theme -> TOML text (hand-formatted so the files stay readable) ---------------------------
std::string Num(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    std::string s = buf;
    while (s.size() > 3 && s.back() == '0' && s[s.size() - 2] != '.') s.pop_back();
    return s;
}
std::string Arr(ImVec4 c) { return "[" + Num(c.x) + ", " + Num(c.y) + ", " + Num(c.z) + ", " + Num(c.w) + "]"; }

std::string ToTomlText(const Theme& t)
{
    std::ostringstream o;
    o << "# Mirror's Edge menu theme (TOML).\n"
         "# Colors:   ImGuiCol names without the prefix, as [r, g, b, a] in 0..1 or \"#RRGGBB[AA]\".\n"
         "# Rounding: WindowRounding, FrameRounding, ...\n"
         "# Menu:     the custom-drawn parts: module rows, accent bar, settings panels.\n"
         "# Anything missing falls back to the default dark theme. Spacing/padding can't be themed.\n"
         "\n[Colors]\n";
    for (const char* name : kExportedColors)
        o << name << " = " << Arr(t.style.Colors[ColorIndex(name)]) << "\n";
    o << "\n[Rounding]\n";
    for (const auto& r : kRounding)
        o << r.key << " = " << Num(t.style.*r.member) << "\n";
    o << "\n[Menu]\n";
    for (const auto& cv : kMenuColors)
        o << cv.key << " = " << Arr(t.menu.*cv.member) << "\n";
    o << "row_height = " << Num(t.menu.rowHeight) << "\n";
    return o.str();
}

// A built-in theme as a file would give it (the fallback when a file is missing or broken)
Loaded FromTheme(const Theme& t)
{
    Loaded out;
    for (const char* name : kExportedColors)
        out.colors.emplace_back(ColorIndex(name), t.style.Colors[ColorIndex(name)]);
    for (const auto& r : kRounding)
        out.vars.emplace_back(r.var, t.style.*r.member);
    out.menu = t.menu;
    return out;
}

// ---- TOML file -> what the style stack and the menu need ---------------------------------------
// Throws on a file that isn't valid TOML. Only known color names, rounding vars and menu keys with values of the right
// type are kept; the rest is logged and ignored (no layout keys, no fonts).
Loaded ParseTheme(const fs::path& path, const std::string& label)
{
    const toml::value doc = toml::parse(path);
    Loaded out;

    for (const auto& [key, _] : doc.as_table())
        if (key != "Colors" && key != "Rounding" && key != "Menu")
            spdlog::warn("Theme '{}': key '{}' ignored (supported: Colors, Rounding, Menu)", label, key);

    if (doc.contains("Colors") && doc.at("Colors").is_table())
        for (const auto& [key, value] : doc.at("Colors").as_table()) {
            ImVec4 col;
            const int idx = ColorIndex(key);
            if (idx < 0) { spdlog::warn("Theme '{}': unknown color '{}'", label, key); continue; }
            if (!ToColor(value, col)) { spdlog::warn("Theme '{}': bad value for color '{}'", label, key); continue; }
            out.colors.emplace_back(idx, col);
        }

    if (doc.contains("Rounding") && doc.at("Rounding").is_table())
        for (const auto& [key, value] : doc.at("Rounding").as_table()) {
            const auto r = std::find_if(std::begin(kRounding), std::end(kRounding), [&](const FloatVar& v) { return key == v.key; });
            float f;
            if (r == std::end(kRounding) || !ToFloat(value, f)) { spdlog::warn("Theme '{}': rounding '{}' ignored", label, key); continue; }
            out.vars.emplace_back(r->var, std::max(0.f, f));
        }

    if (doc.contains("Menu") && doc.at("Menu").is_table())
        for (const auto& [key, value] : doc.at("Menu").as_table()) {
            if (key == "row_height") {
                if (float f; ToFloat(value, f)) out.menu.rowHeight = std::clamp(f, 18.f, 48.f);
                continue;
            }
            const auto cv = std::find_if(std::begin(kMenuColors), std::end(kMenuColors),
                                         [&](const MenuColorVar& v) { return key == v.key; });
            ImVec4 col;
            if (cv == std::end(kMenuColors) || !ToColor(value, col)) {
                spdlog::warn("Theme '{}': menu.{} ignored", label, key);
                continue;
            }
            out.menu.*(cv->member) = col;
        }
    return out;
}

} // namespace

// -------------------------------------------------------------------------------------------------

void theme::Init()
{
    std::error_code ec;
    fs::create_directories(Dir(), ec);
    for (const auto& [name, t] : Builtins()) {
        const auto path = ThemePath(name);
        if (fs::exists(path)) continue;                    // never overwrite user edits
        std::ofstream out(path);
        if (out && (out << ToTomlText(t))) spdlog::info("Wrote built-in theme {}", path.string());
        else                               spdlog::warn("Could not write theme {}", path.string());
    }
    Rescan();
}

void theme::Rescan()
{
    g_names.clear();
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(Dir(), ec))
        if (e.is_regular_file() && e.path().extension() == ".toml")
            g_names.push_back(e.path().stem().string());
    std::sort(g_names.begin(), g_names.end());
}

bool theme::Apply(const std::string& name)
{
    Loaded t;
    bool ok = false;
    const auto path = ThemePath(name);
    if (fs::exists(path)) {
        try {
            t = ParseTheme(path, name);
            ok = true;
        } catch (const std::exception& e) {
            spdlog::error("Theme '{}' failed to load, using the default: {}", name, e.what());
        }
    } else {
        spdlog::warn("Theme '{}' not found, using the default", name);
    }

    if (!ok) t = FromTheme(MakeDark(kRed));   // built-in fallback, no file needed

    g_current = ok ? name : kDefaultTheme;
    g_theme   = std::move(t);
    if (ok) spdlog::info("Theme '{}' applied", name);
    return ok;
}

void theme::ApplyBaseStyle()
{
    Theme base = MakeDark(kRed);
    base.style.WindowPadding = {0.f, 0.f};     // the clickgui layout needs flush rows
    base.style.ItemSpacing   = {0.f, 0.f};
    ImGui::GetStyle() = base.style;
}

void theme::Begin()
{
    for (const auto& [idx, col] : g_theme.colors) ImGui::PushStyleColor(idx, col);
    for (const auto& [var, f] : g_theme.vars) ImGui::PushStyleVar(var, f);
    g_pushedColors = static_cast<int>(g_theme.colors.size());
    g_pushedVars   = static_cast<int>(g_theme.vars.size());
}

void theme::End()
{
    if (g_pushedColors) ImGui::PopStyleColor(g_pushedColors);
    if (g_pushedVars)   ImGui::PopStyleVar(g_pushedVars);
    g_pushedColors = g_pushedVars = 0;
}

void theme::OpenFolder()
{
    ShellExecuteW(nullptr, L"open", Dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

const std::vector<std::string>& theme::Names() { return g_names; }
const std::string& theme::Current() { return g_current; }
const theme::MenuColors& theme::Menu() { return g_theme.menu; }

int theme::CurrentIndex()
{
    const auto it = std::find(g_names.begin(), g_names.end(), g_current);
    return it == g_names.end() ? -1 : static_cast<int>(it - g_names.begin());
}
