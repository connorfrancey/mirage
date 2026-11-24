#include "keys.h"

#include <Windows.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>

namespace {
    bool Held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

    std::string KeyName(int vk)
    {
        switch (vk) {   // GetKeyNameText doesn't know the mouse
            case VK_LBUTTON:  return "Left Mouse";
            case VK_RBUTTON:  return "Right Mouse";
            case VK_MBUTTON:  return "Middle Mouse";
            case VK_XBUTTON1: return "Mouse 4";
            case VK_XBUTTON2: return "Mouse 5";
            default: break;
        }
        LONG lparam = static_cast<LONG>(MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC)) << 16;
        switch (vk)
        {
            // Extended keys need bit 24 for GetKeyNameText
            case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
            case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
            case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK:
            case VK_RCONTROL: case VK_RMENU:
                lparam |= (1 << 24);
        }
        char buf[64] {};
        if (GetKeyNameTextA(lparam, buf, sizeof buf) > 0) return buf;
        return "VK " + std::to_string(vk);
    }
}

bool keys::IsModifier(int vk)
{
    switch (vk) {
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
            return true;
        default:
            return false;
    }
}

int keys::HeldMods()
{
    return (Held(VK_CONTROL) ? kCtrl : 0) | (Held(VK_SHIFT) ? kShift : 0) | (Held(VK_MENU) ? kAlt : 0);
}

std::string keys::Name(int binding)
{
    if (binding == 0) return "None";
    std::string out;
    if (binding & kCtrl) out += "Ctrl+";
    if (binding & kShift) out += "Shift+";
    if (binding & kAlt) out += "Alt+";
    return out + KeyName(Vk(binding));
}

bool keys::Down(int binding)
{
    if (binding == 0 || !Held(Vk(binding))) return false;
    const int mods = Mods(binding);
    return (HeldMods() & mods) == mods;
}

std::optional<keys::Chord> keys::ParseChord(std::string_view text)
{
    std::string key(text);
    std::ranges::transform(key, key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    int modifiers = 0;
    std::string_view part = key;
    for (auto plus = part.find('+'); plus != part.npos; plus = part.find('+')) {
        const auto prefix = part.substr(0, plus);
        const int bit = prefix == "ctrl" || prefix == "control" ? kCtrl :
            prefix == "shift" ? kShift : prefix == "alt" ? kAlt : 0;
        if (!bit || (modifiers & bit)) return std::nullopt;
        modifiers |= bit;
        part.remove_prefix(plus + 1);
    }
    if (part.size() == 1 && part[0] >= 'a' && part[0] <= 'z')
        return Chord{std::string(part), modifiers, 'A' + part[0] - 'a'};
    constexpr std::array<std::string_view, 10> digits{"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine"};
    for (int i = 0; i < static_cast<int>(digits.size()); ++i) {
        if (part == digits[i] || (part.size() == 1 && part[0] == '0' + i))
            return Chord{std::string(digits[i]), modifiers, '0' + i};
        if (part.starts_with("numpad") && part.substr(6) == digits[i])
            return Chord{std::string(part), modifiers, VK_NUMPAD0 + i};
    }
    if (part.size() > 1 && part.front() == 'f' && part[1] != '0') {
        int n = 0;
        const auto end = part.data() + part.size();
        const auto parsed = std::from_chars(part.data() + 1, end, n);
        if (parsed.ec == std::errc{} && parsed.ptr == end && n >= 1 && n <= 24)
            return Chord{std::string(part), modifiers, VK_F1 + n - 1};
    }
    if (part == "esc") part = "escape";
    if (part == "space") part = "spacebar";
    struct Named { std::string_view key; int vk; };
    static constexpr Named names[]{
        {"spacebar", VK_SPACE}, {"escape", VK_ESCAPE}, {"enter", VK_RETURN}, {"tab", VK_TAB},
        {"backspace", VK_BACK}, {"pause", VK_PAUSE}, {"capslock", VK_CAPITAL},
        {"left", VK_LEFT}, {"right", VK_RIGHT}, {"up", VK_UP}, {"down", VK_DOWN},
        {"home", VK_HOME}, {"end", VK_END}, {"pageup", VK_PRIOR}, {"pagedown", VK_NEXT},
        {"insert", VK_INSERT}, {"delete", VK_DELETE}, {"numlock", VK_NUMLOCK}, {"scrolllock", VK_SCROLL},
        {"leftcontrol", VK_LCONTROL}, {"rightcontrol", VK_RCONTROL}, {"leftshift", VK_LSHIFT},
        {"rightshift", VK_RSHIFT}, {"leftalt", VK_LMENU}, {"rightalt", VK_RMENU},
        {"leftmousebutton", VK_LBUTTON}, {"rightmousebutton", VK_RBUTTON}, {"middlemousebutton", VK_MBUTTON},
        {"thumbmousebutton", VK_XBUTTON1}, {"thumbmousebutton2", VK_XBUTTON2}, {"mousescrollup", 0}, {"mousescrolldown", 0},
        {"tilde", VK_OEM_3}, {"semicolon", VK_OEM_1}, {"equals", VK_OEM_PLUS}, {"comma", VK_OEM_COMMA},
        {"underscore", VK_OEM_MINUS}, {"period", VK_OEM_PERIOD}, {"slash", VK_OEM_2},
        {"leftbracket", VK_OEM_4}, {"backslash", VK_OEM_5}, {"rightbracket", VK_OEM_6}, {"quote", VK_OEM_7},
        {"multiply", VK_MULTIPLY}, {"add", VK_ADD}, {"subtract", VK_SUBTRACT}, {"decimal", VK_DECIMAL}, {"divide", VK_DIVIDE},
    };
    for (const auto& name : names)
        if (part == name.key) return Chord{std::string(part), modifiers, name.vk};
    constexpr std::array padButtons{"a", "b", "x", "y", "start", "back", "leftshoulder", "rightshoulder",
        "lefttrigger", "righttrigger", "leftthumbstick", "rightthumbstick", "dpad_up", "dpad_down", "dpad_left", "dpad_right"};
    if (part.starts_with("xboxtypes_") && std::ranges::find(padButtons, part.substr(10)) != padButtons.end())
        return Chord{std::string(part), modifiers, 0};
    return std::nullopt;
}
