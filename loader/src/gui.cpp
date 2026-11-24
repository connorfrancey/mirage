#include "gui.h"
#include "config.h"
#include "core/keybinds.h"
#include "core/keys.h"
#include "core/module.h"
#include "core/notify.h"
#include "core/setting.h"
#include "theme.h"

#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <mutex>

#include <string>
#include <imgui.h>

namespace
{
    ImVec4 Mix(ImVec4 a, ImVec4 b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    }
    ImU32 U32(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
    void Approach(float& v, float target, float dt) { v += (target - v) * std::min(1.f, dt * 14.f); }

    void DrawSettings(Module& m, float w)
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();

        // Content goes on channel 1, the background rect (sized after layout) on channel 0.
        dl->ChannelsSplit(2);
        dl->ChannelsSetCurrent(1);
        ImGui::BeginGroup();
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.f, 6.f});
        ImGui::Indent(10.f);
        ImGui::Dummy({0.f, 2.f});

        const float itemW = w - 20.f;
        for (ctl::Setting* s : m.Settings())
            if (s->Visible()) s->Show(itemW);
        ImGui::PushItemWidth(itemW);
        m.DrawPanel(itemW);
        ImGui::PopItemWidth();

        keybinds::Button(m.IsAction() ? "Hotkey" : "Toggle key", m.key, itemW);

        ImGui::Dummy({0.f, 2.f});
        ImGui::Unindent(10.f);
        ImGui::PopStyleVar();
        ImGui::EndGroup();

        const float endY = ImGui::GetItemRectMax().y;
        dl->ChannelsSetCurrent(0);
        dl->AddRectFilled(start, {start.x + w, endY}, U32(theme::Menu().settingsBg));
        dl->ChannelsMerge();
    }

    void DrawModule(Module& m, float dt)
    {
        float& hover = m.hover;
        const theme::MenuColors& tc = theme::Menu();

        ImGui::PushID(&m);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = tc.rowHeight;

        // The title opens the settings (right click closes them); the switch on the right turns the module on and off,
        // or runs it for an action.
        const float switchW = m.IsAction() ? 36.f : 30.f, switchH = 16.f;
        const float titleW = std::max(1.f, w - switchW - 16.f);

        ImGui::InvisibleButton("##title", {titleW, h}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) m.expanded = !m.expanded;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) m.expanded = false;
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        {
            ImGui::SetTooltip("%s%s%s", m.Name().c_str(), m.Description().empty() ? "" : "\n", m.Description().c_str());
        }

        ImGui::SameLine(0.f, 0.f);
        ImGui::InvisibleButton("##switch", {w - titleW, h});
        const bool switchHovered = ImGui::IsItemHovered();
        hovered |= switchHovered;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        {
            m.Toggle();
            if (m.IsAction()) m.anim = 1.f; // flash on click
        }
        if (switchHovered && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip(m.IsAction() ? "Run" : m.Enabled() ? "Turn off" : "Turn on");

        Approach(m.anim, (!m.IsAction() && m.Enabled()) ? 1.f : 0.f, dt);
        Approach(hover, hovered ? 1.f : 0.f, dt);

        ImVec4 bg = Mix(tc.rowBg, tc.rowHover, hover);
        bg = Mix(bg, Mix(tc.rowBg, tc.accent, 0.18f), m.anim);
        dl->AddRectFilled(p, {p.x + w, p.y + h}, U32(bg));
        dl->AddRectFilled(p, {p.x + 3.f * m.anim, p.y + h}, U32(tc.accent));

        const ImVec4 txt = Mix(tc.text, tc.textActive, std::max(m.anim, hover * 0.6f));
        const float ty = p.y + (h - ImGui::GetFontSize()) * 0.5f;
        dl->PushClipRect(p, {p.x + std::max(1.f, titleW - 8.f), p.y + h}, true);
        dl->AddText({p.x + 10.f, ty}, U32(txt), m.Name().c_str());
        dl->PopClipRect();
        const float nameEnd = p.x + 10.f + ImGui::CalcTextSize(m.Name().c_str()).x;

        // Switch: a pill whose knob slides right (accent) when on. Actions get a small "Run" pill instead.
        const ImVec2 s0{p.x + w - switchW - 8.f, p.y + (h - switchH) * 0.5f};

        // Left of the switch: a chevron (settings open or closed), and the module's key when it has one
        const ImU32 dim = U32(Mix(tc.textDim, tc.text, hover * 0.5f));
        const float cx = s0.x - 10.f, cy = p.y + h * 0.5f, c = 3.5f;
        if (m.expanded) dl->AddTriangleFilled({cx - c, cy - c * 0.6f}, {cx + c, cy - c * 0.6f}, {cx, cy + c * 0.8f}, dim);
        else dl->AddTriangleFilled({cx - c * 0.6f, cy - c}, {cx - c * 0.6f, cy + c}, {cx + c * 0.8f, cy}, dim);
        if (m.key) {
            const std::string k = keys::Name(m.key);
            const ImVec2 ks = ImGui::CalcTextSize(k.c_str());
            const float kx = cx - 10.f - ks.x;
            if (kx > nameEnd + 8.f) dl->AddText({kx, ty}, dim, k.c_str());   // Only where the name leaves room
        }
        const ImVec2 s1{s0.x + switchW, s0.y + switchH};
        const float r = switchH * 0.5f;
        const ImVec4 hoverTint = Mix(tc.textDim, tc.text, switchHovered ? 0.5f : 0.f);
        if (m.IsAction())
        {
            dl->AddRectFilled(s0, s1, U32(Mix(Mix(tc.rowBg, hoverTint, 0.35f), tc.accent, m.anim)), r);
            const ImVec2 ts = ImGui::CalcTextSize("Run");
            dl->AddText({s0.x + (switchW - ts.x) * 0.5f, s0.y + (switchH - ts.y) * 0.5f}, U32(tc.text), "Run");
        }
        else
        {
            dl->AddRectFilled(s0, s1, U32(Mix(Mix(tc.rowBg, hoverTint, 0.45f), tc.accent, m.anim)), r);
            const float kx = s0.x + r + (switchW - 2.f * r) * m.anim;
            dl->AddCircleFilled({kx, s0.y + r}, r - 2.5f, U32(Mix(tc.textDim, tc.textActive, m.anim)));
        }

        if (m.expanded) DrawSettings(m, w);
        ImGui::PopID();
    }
}

namespace
{
    char g_search[64] = "";
    bool g_focusSearch = false;

    std::string Lower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // `query` lower case, not empty: the module's name, description, tab or one of its settings has it
    bool Matches(const Module& m, const std::string& query)
    {
        if (Lower(m.Name() + "\n" + m.Description() + "\n" + m.Tab()).find(query) != std::string::npos) return true;
        for (const ctl::Setting* s : m.Settings())
            if (Lower(s->Name()).find(query) != std::string::npos) return true;
        return false;
    }

    std::mutex g_testColumnLock;
    std::string g_testColumn;       // gui::OpenColumnForTest
    std::string g_category;         // The user's selection, independent of the category requested by a self-test.
    const modules::Tab* g_shownCategory = nullptr;

    bool HasVisibleModules(const modules::Tab& tab)
    {
        return std::any_of(tab.modules.begin(), tab.modules.end(), [](const Module* m) { return !m->Hidden(); });
    }

    void DrawSearch(float dt, const std::string& query)
    {
        bool any = false;
        for (const auto& tab : modules::Tabs())
        {
            bool heading = false;
            for (Module* m : tab->modules)
            {
                if (m->Hidden() || !Matches(*m, query)) continue;
                if (!heading)
                {
                    ImGui::SeparatorText(tab->name.c_str());
                    heading = true;
                }
                DrawModule(*m, dt);
                any = true;
            }
        }
        if (!any) ImGui::TextDisabled("No matching modules.");
    }

    void DrawBrowser(float dt)
    {
        const modules::Tab* active = nullptr;
        const modules::Tab* first = nullptr;
        const modules::Tab* test = nullptr;
        std::string testCategory;
        {
            std::lock_guard lock(g_testColumnLock);
            testCategory = g_testColumn;
        }
        for (const auto& tab : modules::Tabs())
        {
            if (!HasVisibleModules(*tab)) continue;
            if (!first) first = tab.get();
            if (tab->name == g_category) active = tab.get();
            if (tab->name == testCategory) test = tab.get();
        }
        if (!first)
        {
            ImGui::TextDisabled("No modules available.");
            return;
        }
        if (!active)
        {
            active = first;
            g_category = first->name;
        }

        ImGui::SetNextItemWidth(-FLT_MIN);
        if (g_focusSearch)
        {
            ImGui::SetKeyboardFocusHere();
            g_focusSearch = false;
        }
        const bool searchChanged = ImGui::InputTextWithHint("##search", "Search modules and settings (Ctrl+F)",
            g_search, sizeof g_search, ImGuiInputTextFlags_EscapeClearsAll);

        const float sidebar = std::clamp(ImGui::GetContentRegionAvail().x * 0.26f, 88.f, 160.f);
        if (ImGui::BeginChild("##categories", {sidebar, 0.f}, ImGuiChildFlags_Borders))
        {
            for (const auto& tab : modules::Tabs())
            {
                if (!HasVisibleModules(*tab)) continue;
                ImGui::PushID(tab.get());
                const bool selected = test ? tab.get() == test : !g_search[0] && tab.get() == active;
                if (ImGui::Selectable(tab->name.c_str(), selected))
                {
                    g_category = tab->name;
                    active = tab.get();
                    g_search[0] = '\0';
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        const modules::Tab* shown = test ? test : g_search[0] ? nullptr : active;
        if (searchChanged || shown != g_shownCategory) ImGui::SetNextWindowScroll({0.f, 0.f});
        g_shownCategory = shown;
        if (ImGui::BeginChild("##module-list", {0.f, 0.f}, ImGuiChildFlags_Borders))
        {
            if (shown)
            {
                for (Module* m : shown->modules)
                    if (!m->Hidden()) DrawModule(*m, dt);
            }
            else
            {
                DrawSearch(dt, Lower(g_search));
            }
        }
        ImGui::EndChild();
    }
}

void gui::ApplyStyle()
{
    theme::ApplyBaseStyle(); // fixed layout + default colors; TOML themes are pushed on top each frame
    theme::Apply(theme::Current()); // load the selected theme file (menu colors + style colors and rounding)
}

bool gui::CaptureKey(int vk)
{
    return keybinds::Capture(vk);
}

// Middle mouse / Mouse 4 / Mouse 5: bound like keys (the menu gets no key message for them)
static void PollMouseButtons()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    for (const int vk : keybinds::PollMouseButtons(pid == GetCurrentProcessId()))
        if (!keybinds::Capture(vk)) modules::OnKey(vk);
}

void gui::Render()
{
    PollMouseButtons();
    modules::Render(); // HUD modules draw even when the menu is hidden
    config::AutoSave();    // Cheap unless a second has passed; writes only a changed config
    notify::Draw();

    if (!visible)
    {
        keybinds::Cancel(); // closing the menu cancels a pending key binding
        return;
    }

    theme::Begin();
    const bool shortcut = ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F);
    if (shortcut)
    {
        g_focusSearch = true;
        ImGui::SetNextWindowFocus();
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 room{std::max(1.f, viewport->WorkSize.x - 16.f), std::max(1.f, viewport->WorkSize.y - 16.f)};
    ImGui::SetNextWindowPos({viewport->WorkPos.x + 24.f, viewport->WorkPos.y + 24.f}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({std::min(610.f, room.x), std::min(540.f, room.y)}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({std::min(360.f, room.x), std::min(240.f, room.y)}, room);

    if (ImGui::Begin("Mirage", nullptr, ImGuiWindowFlags_NoCollapse))
    {
        const ImVec2 pos = ImGui::GetWindowPos();
        const ImVec2 size = ImGui::GetWindowSize();
        const ImVec2 clamped{
            std::clamp(pos.x, viewport->WorkPos.x, viewport->WorkPos.x + std::max(0.f, viewport->WorkSize.x - size.x)),
            std::clamp(pos.y, viewport->WorkPos.y, viewport->WorkPos.y + std::max(0.f, viewport->WorkSize.y - size.y))
        };
        if (clamped.x != pos.x || clamped.y != pos.y) ImGui::SetWindowPos(clamped);
        DrawBrowser(ImGui::GetIO().DeltaTime);
    }
    ImGui::End();
    theme::End();


    keybinds::EndFrame(); // clicking anywhere except a bind button cancels a pending binding
}

void gui::OpenColumnForTest(const char* tab)
{
    std::lock_guard lock(g_testColumnLock);
    g_testColumn = tab ? tab : "";
}
