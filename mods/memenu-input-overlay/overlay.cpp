// Input Overlay: the keyboard and mouse on screen, like NohBoard. It shows what the game gets (the runtime's input
// monitor): your own input while you play, and the macro's while the TAS records or plays one back. Without the monitor
// it falls back to the physical keys. The layout is a file (input_layout.h) in this mod's input_layouts\ folder; the
// look is in the settings (mod.toml).
//
// A native first-party mod: built against the public API only (mirage/api.h, mirage/mirage.hpp).
#include "input_layout.h"

#include "mirage/mirage.hpp"

#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using inputlayout::Element;
using inputlayout::Kind;
using inputlayout::Layout;

const char* const kFolder = "input_layouts";

// ---- the fallback: physical keys -----------------------------------------------------------------------------------------
int VirtualKey(const std::string& name)
{
    if (name.size() == 1 && name[0] >= 'a' && name[0] <= 'z') return name[0] - 'a' + 'A';
    static const std::unordered_map<std::string, int> keys = {
        {"zero", '0'}, {"one", '1'}, {"two", '2'}, {"three", '3'}, {"four", '4'}, {"five", '5'}, {"six", '6'},
        {"seven", '7'}, {"eight", '8'}, {"nine", '9'}, {"spacebar", VK_SPACE}, {"leftshift", VK_LSHIFT},
        {"rightshift", VK_RSHIFT}, {"leftcontrol", VK_LCONTROL}, {"rightcontrol", VK_RCONTROL}, {"leftalt", VK_LMENU},
        {"rightalt", VK_RMENU}, {"tab", VK_TAB}, {"capslock", VK_CAPITAL}, {"escape", VK_ESCAPE}, {"enter", VK_RETURN},
        {"backspace", VK_BACK}, {"up", VK_UP}, {"down", VK_DOWN}, {"left", VK_LEFT}, {"right", VK_RIGHT},
        {"leftmousebutton", VK_LBUTTON}, {"rightmousebutton", VK_RBUTTON}, {"middlemousebutton", VK_MBUTTON},
        {"thumbmousebutton", VK_XBUTTON1}, {"thumbmousebutton2", VK_XBUTTON2},
    };
    if (name.size() >= 2 && name[0] == 'f' && name[1] >= '0' && name[1] <= '9') {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    const auto it = keys.find(name);
    return it == keys.end() ? 0 : it->second;
}

struct KeyState {
    bool held = false;
    double down = -1.0, up = -1.0;
    unsigned presses = 0;
};

// ---- colors (0xAABBGGRR, as the API draws them) ----------------------------------------------------------------------------
float Channel(uint32_t c, int shift) { return static_cast<float>((c >> shift) & 0xFF) / 255.f; }
uint32_t Pack(float r, float g, float b, float a)
{
    const auto u = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    return u(r) | (u(g) << 8) | (u(b) << 16) | (u(a) << 24);
}
uint32_t Mix(uint32_t a, uint32_t b, float t)
{
    const auto m = [&](int s) { return Channel(a, s) + (Channel(b, s) - Channel(a, s)) * t; };
    return Pack(m(0), m(8), m(16), m(24));
}
uint32_t Alpha(uint32_t c, float times) { return Pack(Channel(c, 0), Channel(c, 8), Channel(c, 16), Channel(c, 24) * times); }
uint32_t WithAlpha(uint32_t c, uint32_t alpha) { return (c & 0x00FFFFFFu) | (alpha << 24); }

class InputOverlay final : public mirage::Module {
public:
    InputOverlay()
        : Module("Input Overlay", "Visual", "Keyboard and mouse on screen (like NohBoard), also for TAS playback", 40)
    {}

    void OnEnable() override { loadedFrom_.clear(); }

    void OnRender() override
    {
        const MirageUi& ui = mirage::Ui();
        EnsureLayout();
        if (!Showing()) return;
        Read();
        const float unit = static_cast<float>(mirage::Get<int>("key_size"));
        const float width = layout_.Width() * unit, height = layout_.Height() * unit;
        const bool menu = mirage::Api().menu_open() != 0;

        uint32_t flags = MIRAGE_WINDOW_NO_TITLE | MIRAGE_WINDOW_NO_RESIZE | MIRAGE_WINDOW_NO_SCROLLBAR |
                         MIRAGE_WINDOW_NO_COLLAPSE | MIRAGE_WINDOW_AUTO_SIZE;
        if (!menu) flags |= MIRAGE_WINDOW_NO_INPUTS;            // Only movable while the menu is open
        float sw = 0.f, sh = 0.f;
        mirage::Api().screen_size(&sw, &sh);
        ui.window_next_pos(20.f, std::max(0.f, sh - height - 110.f), 0);
        const float backdrop = mirage::Get<float>("backdrop");
        ui.window_next_bg_alpha(menu ? std::max(backdrop, 0.25f) : backdrop);
        ui.style_push(MIRAGE_STYLE_WINDOW_PADDING, 6.f, 6.f);
        ui.style_push(MIRAGE_STYLE_WINDOW_BORDER, menu ? 1.f : 0.f, 0.f);
        if (ui.window_begin("Input overlay##inputoverlay", nullptr, flags)) {
            float ox = 0.f, oy = 0.f;
            ui.cursor_screen_pos(&ox, &oy);
            ui.dummy(width, height);
            if (menu && ui.item_hovered()) ui.tooltip("Drag to move it. The layout is in the module's settings");
            const float gap = static_cast<float>(mirage::Get<int>("spacing")) * 0.5f;
            for (const Element& e : layout_.elements) {
                const float ax = ox + e.x * unit + gap, ay = oy + e.y * unit + gap;
                const float bx = ox + (e.x + e.w) * unit - gap, by = oy + (e.y + e.h) * unit - gap;
                switch (e.kind) {
                    case Kind::Key:   DrawKey(e, ax, ay, bx, by); break;
                    case Kind::Mouse: DrawMouse(ax, ay, bx, by); break;
                    case Kind::Kps:   DrawKps(ax, ay, bx, by); break;
                }
            }
        }
        ui.window_end();
        ui.style_pop(2);
    }

    void OnPanel() override
    {
        const MirageUi& ui = mirage::Ui();
        const bool layout = mirage::Get<bool>("section_layout"), look = mirage::Get<bool>("section_look");
        const bool colors = mirage::Get<bool>("section_colors"), extras = mirage::Get<bool>("section_extras");
        mirage::SetVisible("show_when", layout);
        for (const char* k : {"key_size", "spacing", "rounding", "outline", "text_size", "fade", "backdrop"})
            mirage::SetVisible(k, look);
        for (const char* k : {"key_color", "pressed_color", "text_color", "pressed_text_color", "outline_color"})
            mirage::SetVisible(k, colors);
        for (const char* k : {"counts", "mouse_scale", "mouse_trail", "invert_y"}) mirage::SetVisible(k, extras);
        if (layout) DrawLayoutPicker(ui);
    }

private:
    // ---- what to show ---------------------------------------------------------------------------------------------------
    bool Showing() const
    {
        const int when = mirage::Get<int>("show_when");     // Always, Not during a TAS, Only during a TAS
        const bool session = mirage::Api().tas_session() != 0;
        if (when == 1 && session) return false;
        if (when == 2 && !session) return false;
        return !layout_.elements.empty();
    }

    void Read()
    {
        const MirageApi& api = mirage::Api();
        keys_.clear();
        engine_ = false;
        for (const Element& e : layout_.elements)
            for (const std::string& k : e.keys) {
                MirageKeyState s{sizeof(MirageKeyState)};
                if (api.input_key_state(k.c_str(), &s)) {
                    engine_ = true;
                    keys_[k] = {s.held != 0, s.down, s.up, s.presses};
                } else if (const int vk = VirtualKey(k)) {
                    keys_[k].held = (GetAsyncKeyState(vk) & 0x8000) != 0;   // Physical keys: only what is held
                }
            }
        now_ = api.input_now();
        if (engine_) {
            api.input_mouse(0.05f, &mouseX_, &mouseY_);
            kps_ = api.input_kps();
        }
    }

    const KeyState* Find(const std::string& name) const
    {
        const auto it = keys_.find(name);
        return it == keys_.end() ? nullptr : &it->second;
    }

    // ---- drawing --------------------------------------------------------------------------------------------------------
    float TextSize() const
    {
        const int t = mirage::Get<int>("text_size");
        return t > 0 ? static_cast<float>(t) : std::max(9.f, static_cast<float>(mirage::Get<int>("key_size")) * 0.36f);
    }

    void CenteredText(float ax, float ay, float bx, float by, float size, uint32_t col, const char* text, float dy = 0.f) const
    {
        float tw = 0.f, th = 0.f;
        mirage::Api().text_size_sized(text, size, &tw, &th);
        mirage::Ui().window_draw_text_sized((ax + bx - tw) * 0.5f, (ay + by - th) * 0.5f + dy, col, size, text);
    }

    void Box(float ax, float ay, float bx, float by, uint32_t fill) const
    {
        const MirageUi& ui = mirage::Ui();
        const float r = static_cast<float>(mirage::Get<int>("rounding"));
        ui.window_draw_rect(ax, ay, bx, by, fill, r, 0.f);
        const float outline = mirage::Get<float>("outline");
        if (outline > 0.f) ui.window_draw_rect(ax, ay, bx, by, static_cast<uint32_t>(mirage::Get<int>("outline_color")), r, outline);
    }

    // How lit a key is: 1 while held, then fading out over the fade time. The wheel's "keys" are only down for an
    // instant: they get at least a short flash.
    float Lit(const Element& e) const
    {
        float lit = 0.f;
        const double fadeS = mirage::Get<int>("fade") / 1000.0;
        for (const std::string& name : e.keys) {
            const KeyState* k = Find(name);
            if (!k) continue;
            if (k->held) return 1.f;
            if (k->down < 0.0) continue;
            const bool wheel = name == "mousescrollup" || name == "mousescrolldown";
            const double release = wheel ? std::max(k->up, k->down + 0.12) : k->up;
            if (now_ < release) return 1.f;
            const double since = now_ - release;
            if (fadeS > 0.0 && since < fadeS) lit = std::max(lit, 1.f - static_cast<float>(since / fadeS));
        }
        return lit;
    }

    uint32_t Color(const char* key) const { return static_cast<uint32_t>(mirage::Get<int>(key)); }

    void DrawKey(const Element& e, float ax, float ay, float bx, float by) const
    {
        const float lit = Lit(e);
        Box(ax, ay, bx, by, Mix(Color("key_color"), Color("pressed_color"), lit));
        const uint32_t text = Mix(Color("text_color"), Color("pressed_text_color"), lit);
        const float size = std::min(TextSize(), (by - ay) * 0.8f);
        const bool showCount = mirage::Get<bool>("counts") && engine_ && by - ay > size * 1.9f;
        CenteredText(ax, ay, bx, by, size, text, e.label.c_str(), showCount ? -size * 0.3f : 0.f);
        if (showCount) {
            unsigned presses = 0;
            for (const std::string& name : e.keys)
                if (const KeyState* k = Find(name)) presses += k->presses;
            char n[16];
            std::snprintf(n, sizeof n, "%u", presses);
            CenteredText(ax, ay, bx, by, size * 0.6f, WithAlpha(text, 0xA0), n, size * 0.55f);
        }
    }

    // The mouse's movement: a dot pushed away from the middle the way it moves, and how far (with a line to it)
    void DrawMouse(float ax, float ay, float bx, float by) const
    {
        const MirageUi& ui = mirage::Ui();
        Box(ax, ay, bx, by, Color("key_color"));
        const float cx = (ax + bx) * 0.5f, cy = (ay + by) * 0.5f;
        const float radius = std::min(bx - ax, by - ay) * 0.5f - 6.f;
        if (radius <= 2.f) return;
        const uint32_t faint = WithAlpha(Color("text_color"), 0x40);
        ui.window_draw_line(cx - radius, cy, cx + radius, cy, faint, 1.f);
        ui.window_draw_line(cx, cy - radius, cx, cy + radius, faint, 1.f);
        if (!engine_) {
            CenteredText(ax, ay, bx, by, TextSize() * 0.7f, WithAlpha(faint, 0xFF), "mouse");
            return;
        }
        float dx = mouseX_, dy = mirage::Get<bool>("invert_y") ? mouseY_ : -mouseY_;   // The engine's Y goes up
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len > 0.f) {    // Big movements squash toward the edge instead of leaving the pad
            const float reach = std::tanh(len * mirage::Get<float>("mouse_scale") / 60.f) * radius;
            dx = dx / len * reach;
            dy = dy / len * reach;
        }
        const uint32_t pressed = Color("pressed_color");
        if (mirage::Get<bool>("mouse_trail") && len > 0.f) ui.window_draw_line(cx, cy, cx + dx, cy + dy, Alpha(pressed, 0.7f), 2.f);
        ui.window_draw_circle(cx + dx, cy + dy, std::max(3.f, radius * 0.14f), len > 0.f ? pressed : WithAlpha(faint, 0xFF), 0.f);
    }

    void DrawKps(float ax, float ay, float bx, float by) const
    {
        Box(ax, ay, bx, by, Color("key_color"));
        const float size = std::min(TextSize(), (by - ay) * 0.45f);
        char n[16];
        std::snprintf(n, sizeof n, "%d", engine_ ? kps_ : 0);
        CenteredText(ax, ay, bx, by, size * 0.6f, WithAlpha(Color("text_color"), 0x90), "KPS", -size * 0.45f);
        CenteredText(ax, ay, bx, by, size, Color("text_color"), n, size * 0.3f);
    }

    // ---- layouts --------------------------------------------------------------------------------------------------------
    // The built-in layouts are written as files the first time, so they can be copied and edited
    static void WriteBuiltins()
    {
        const auto have = mirage::ListFiles(kFolder, ".json");
        for (const Layout& l : inputlayout::Builtins()) {
            const std::string name = l.name + ".json";
            if (std::find(have.begin(), have.end(), name) == have.end())
                mirage::WriteFile(std::string(kFolder) + "/" + name, inputlayout::ToJson(l));
        }
    }

    // Loads the picked layout when another is picked, and again when its file changes (looked at once a second)
    void EnsureLayout()
    {
        const std::string picked = mirage::Get<std::string>("layout");
        const std::string want = picked.empty() ? "Movement" : picked;
        const float now = mirage::Ui().time();
        if (want == loadedFrom_ && now < nextLook_) return;
        nextLook_ = now + 1.f;
        const std::string path = std::string(kFolder) + "/" + want + ".json";
        const uint64_t stamp = mirage::Api().file_stamp(mirage::Mod(), path.c_str());
        if (want == loadedFrom_ && stamp == loadedStamp_) return;
        loadedFrom_ = want;
        loadedStamp_ = stamp;

        std::string text;
        if (!mirage::ReadFile(path, text)) {
            // No such file (deleted, or the first start): the built-in layout of that name, else Movement
            WriteBuiltins();
            const auto builtins = inputlayout::Builtins();
            layout_ = builtins.front();
            for (const Layout& l : builtins)
                if (l.name == want) layout_ = l;
            error_ = layout_.name == want ? "" : "no " + want + ".json: showing Movement";
            return;
        }
        Layout l;
        std::string error;
        if (inputlayout::Parse(text, l, &error)) {
            layout_ = std::move(l);
            error_.clear();
        } else {
            error_ = want + ".json: " + error + " (showing the last good one)";
            if (layout_.elements.empty()) layout_ = inputlayout::Builtins().front();
        }
    }

    void DrawLayoutPicker(const MirageUi& ui)
    {
        const float now = ui.time();
        if (names_.empty() || now > namesAt_ + 2.f) {
            if (names_.empty()) WriteBuiltins();
            names_.clear();
            for (const std::string& f : mirage::ListFiles(kFolder, ".json")) names_.push_back(f.substr(0, f.size() - 5));
            namesAt_ = now;
        }
        const std::string picked = mirage::Get<std::string>("layout");
        const std::string current = picked.empty() ? "Movement" : picked;
        // The layouts as a combo: the files' names, the picked one first in view
        std::string items;
        int index = 0;
        for (std::size_t i = 0; i < names_.size(); ++i) {
            items += names_[i];
            items.push_back('\0');
            if (names_[i] == current) index = static_cast<int>(i);
        }
        items.push_back('\0');
        const float width = ui.content_width();
        ui.next_width(width);
        if (!names_.empty() && ui.combo("##layout", &index, items.c_str())) mirage::Set("layout", names_[static_cast<std::size_t>(index)]);
        if (ui.item_hovered()) ui.tooltip("Layouts are the .json files in this mod's input_layouts\\ folder: copy one, move its keys, rename it");
        if (!error_.empty()) {
            ui.color_push(MIRAGE_COLOR_TEXT, Pack(1.f, 0.6f, 0.35f, 1.f));
            ui.text_wrapped(error_.c_str());
            ui.color_pop(1);
        }
        const float third = (width - 2.f * 8.f) / 3.f;
        if (ui.button("Open folder", third, 0.f)) {
            WriteBuiltins();
            mirage::Api().file_open_folder(mirage::Mod(), kFolder);
        }
        ui.same_line(0.f, -1.f);
        if (ui.button("Reload", third, 0.f)) {
            loadedFrom_.clear();
            namesAt_ = -10.f;
        }
        if (ui.item_hovered()) ui.tooltip("Read the layout file again (edits are also picked up by themselves)");
        ui.same_line(0.f, -1.f);
        if (ui.button("Reset counts", third, 0.f)) mirage::Api().input_reset_counts();
        if (ui.item_hovered()) ui.tooltip("The press counts on the keys (Extras > Press counts)");
        MirageKeyState probe{sizeof(MirageKeyState)};
        if (!mirage::Api().input_key_state("w", &probe))
            ui.text_disabled("Reading the keyboard itself: no mouse movement, counts or TAS playback");
    }

    Layout layout_;
    std::string loadedFrom_, error_;
    uint64_t loadedStamp_ = 0;
    float nextLook_ = 0.f;
    std::vector<std::string> names_;
    float namesAt_ = -10.f;

    // Read on the render thread each frame
    std::unordered_map<std::string, KeyState> keys_;
    float mouseX_ = 0.f, mouseY_ = 0.f;
    int kps_ = 0;
    double now_ = 0.0;
    bool engine_ = false;
};
MIRAGE_MODULE(InputOverlay)

} // namespace

MIRAGE_MOD()
