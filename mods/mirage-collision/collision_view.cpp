// Collision view: trigger, kill, checkpoint, blocking and movement volumes drawn as solid, translucent geometry, and the
// actors placed with them (player starts, pickups, AI-only volumes and areas, navigation points), and the static
// collision of the level. The volumes are read from the SDK on the game thread (debug_world, static_world) and drawn by
// the runtime's Direct3D renderer (MirageApi::world_draw_*). This module owns the settings and wires the two together.
//
// A native first-party mod: the public API and SDK only. For the self-test it shares what it sees
// (mirage.shared("mirage-collision", "inspect" / "static"), JSON).
#include "compat.h"
#include "debug_world.h"
#include "static_world.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

const MirageUi& Ui() { return mirage::Ui(); }

// API draw colors are 0xAABBGGRR; the readers' are 0xAARRGGBB
uint32_t ToDraw(uint32_t argb, float alpha = 1.f)
{
    const auto a = static_cast<uint32_t>(std::clamp(alpha, 0.f, 1.f) * 255.f + 0.5f);
    return ((argb >> 16) & 0xFF) | (argb & 0xFF00) | ((argb & 0xFF) << 16) | (a << 24);
}
uint32_t Rgba(int r, int g, int b, int a = 255)
{
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
}
// A color setting (0xAABBGGRR) as the readers' 0xAARRGGBB, opaque
uint32_t SettingArgb(const std::string& key)
{
    const auto c = static_cast<uint32_t>(mirage::Get<int>(key));
    return 0xFF000000u | ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
}

// One kind of volume (or static mesh): its settings' keys and its short name in the grid
struct Layer {
    const char* key;            // <key> shown, <key>_color its color
    const char* label;
    const char* tooltip;
};
// In the order of debugworld::Category
constexpr Layer kLayers[debugworld::kCategoryCount] = {
    {"triggers", "Triggers", ""},
    {"kill_zones", "Kill zones", ""},
    {"checkpoints", "Checkpoints", ""},
    {"blocking", "Blocking", ""},
    {"movement", "Movement", "Ladders, ziplines, swing/balance/ledge volumes, water, gravity, fall height, teleporters"},
    {"other_misc", "Misc", "Every other kind of volume (physics force field excludes, plain volumes, ...)"},
    {"cylinders", "Cylinders", "Cylinder collision of pawns and other moving actors (looked up live)"},
    {"spawns", "Spawns", "Player starts: level starts, time trial and tutorial starts"},
    {"pickups", "Pickups", "Weapon pickups and stash points (where they are placed, not dropped weapons)"},
    {"ai", "AI only", "Volumes and areas only AI uses: AI blocking volumes, path limits, cover groups, grenade target areas"},
    {"navigation", "Navigation", "The AI's path network: path nodes and move hints (vault, wallrun, jump, slide)"},
    {"other_post_process", "Post process", "Post process, color scale, light and reflection volumes"},
    {"other_audio", "Audio", "Reverb and other audio volumes"},
    {"other_fog", "Fog", "Fog volumes and their density shapes"},
    {"other_streaming", "Streaming", "Level streaming volumes, cull distance and portal volumes"},
};
// In the order of rstatic::Category
constexpr Layer kStaticLayers[rstatic::kCategoryCount] = {
    {"static_architecture", "Architecture", "Static mesh actors: the walls, floors, roofs and rails of the level"},
    {"static_movers", "Movers", "Doors, elevators and moving platforms (InterpActors), drawn where they are now"},
    {"static_props", "Props", "Physics objects and other dynamic meshes (KActors), drawn where they are now"},
    {"static_invisible", "Invisible", "Meshes the game doesn't draw but that still block you: invisible walls"},
    {"static_other", "Other", "Static meshes of any other kind of actor"},
    {"static_bounds", "Bounds", "Meshes without simple collision: their bounding box, only roughly where they block"},
};

std::string Trimmed(const char* text)
{
    std::string f = text;
    f.erase(0, f.find_first_not_of(' '));
    f.erase(f.find_last_not_of(' ') + 1);
    std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return f;
}

void ShadowText(float x, float y, uint32_t color, const char* text)
{
    mirage::Api().draw_text(x + 1.f, y + 1.f, Rgba(0, 0, 0, 220), text);
    mirage::Api().draw_text(x, y, color, text);
}

float TextWidth(const char* text)
{
    float w = 0.f, h = 0.f;
    mirage::Api().text_size(text, &w, &h);
    return w;
}

class CollisionView final : public mirage::Module {
public:
    CollisionView() : Module("Collision View", "Debug", "Draw collision and trigger volumes as solid geometry") { game::Hooks(); }

    void OnEnable() override
    {
        debugrender::SetActive(true);
        debugworld::RequestRebuild();
    }

    void OnDisable() override
    {
        game::RunOnGameThread([this] {
            debugrender::SetActive(false);
            staticworld::Stop();
            staticShown_ = false;
        });
    }

    void OnTick() override
    {
        if (!Enabled()) return;
        debugrender::SetStyle(RenderStyle());
        const auto cfg = WorldConfig();
        if (cfg.filter != lastFilter_) {
            lastFilter_ = cfg.filter;
            debugworld::RequestRebuild();
        }
        debugworld::Update(cfg);

        // Static geometry: scanned and built only while it is shown
        if (mirage::Get<bool>("static_on")) {
            staticworld::Update(StaticConfig());
            staticShown_ = true;
        } else if (staticShown_) {
            staticworld::Stop();
            staticShown_ = false;
        }
        ShareForTest();
    }

    void OnRender() override
    {
        info_ = debugworld::GetInfo();
        if (mirage::Get<bool>("labels")) DrawLabels();
        if (mirage::Get<bool>("stats_line")) DrawStatsLine();
        if (mirage::Get<bool>("look_at")) DrawLookAt();
        if (mirage::Get<bool>("inside_list")) DrawInsideList();
    }

    void OnPanel() override
    {
        panelFrame_ = Ui().time();
        const float width = Ui().content_width();
        const bool show = mirage::Get<bool>("section_show"), display = mirage::Get<bool>("section_display");
        const bool inspect = mirage::Get<bool>("section_inspect"), stat = mirage::Get<bool>("section_static");
        const bool cache = mirage::Get<bool>("section_cache"), advanced = mirage::Get<bool>("section_advanced");
        // The layers are drawn in the grids, not as settings of their own
        for (const Layer& l : kLayers) {
            mirage::SetVisible(l.key, false);
            mirage::SetVisible(std::string(l.key) + "_color", false);
        }
        for (const Layer& l : kStaticLayers) {
            mirage::SetVisible(l.key, false);
            mirage::SetVisible(std::string(l.key) + "_color", false);
        }
        mirage::SetVisible("other_volumes", false);
        for (const char* k : {"style", "opacity", "depth", "max_distance", "fade", "labels"}) mirage::SetVisible(k, display);
        mirage::SetVisible("label_distance", display && mirage::Get<bool>("labels"));
        for (const char* k : {"look_at", "highlight", "inside_list", "own_cylinder"}) mirage::SetVisible(k, inspect);
        for (const char* k : {"static_on", "non_colliding", "static_style", "static_range", "static_budget"}) mirage::SetVisible(k, stat);
        mirage::SetVisible("static_opacity", stat && mirage::Get<int>("static_style") != 0);
        for (const char* k : {"update_rate", "fov_offset", "aspect_override", "use_cache", "scan_cached", "stats_line"})
            mirage::SetVisible(k, advanced);

        if (show) {
            Ui().separator();
            Ui().text_disabled("Show");
            DrawLayers();
            DrawFilter(width);
        }
        if (inspect) {
            Ui().separator();
            Ui().text_disabled("Inspect");
            DrawInspectInfo();
        }
        if (stat) {
            Ui().separator();
            Ui().text_disabled("Static geometry");
            DrawStaticLayers();
            DrawStaticFilter(width);
            DrawStaticStatus();
        }
        if (cache) {
            Ui().separator();
            Ui().text_disabled("Cache & baking");
            DrawCacheStatus(width);
        }
        if (advanced) {
            if (Ui().button("Rebuild now", width, 0.f)) debugworld::RequestRebuild();
            if (Ui().item_hovered()) Ui().tooltip("Rebuild the drawn scene from the remembered volumes");
        }
    }

private:
    bool PanelOpen() const { return Ui().time() - panelFrame_ < 0.5f; }

    debugworld::Config WorldConfig() const
    {
        debugworld::Config c;
        c.fade = mirage::Get<bool>("fade");
        c.filter = Trimmed(filter_);
        c.ownCylinder = mirage::Get<bool>("own_cylinder");
        c.highlight = mirage::Get<bool>("highlight");
        const bool inspect = mirage::Get<bool>("section_inspect");
        c.inspect = mirage::Get<bool>("look_at") || c.highlight || mirage::Get<bool>("inside_list") || inspect;
        c.nearby = inspect;
        c.style = static_cast<debugworld::DrawStyle>(mirage::Get<int>("style"));
        c.maxDistance = mirage::Get<float>("max_distance");
        c.updateHz = mirage::Get<float>("update_rate");
        c.labels = mirage::Get<bool>("labels");
        c.labelDistance = mirage::Get<float>("label_distance");
        c.useCache = mirage::Get<bool>("use_cache");
        c.scanCached = mirage::Get<bool>("scan_cached");
        const bool others = mirage::Get<bool>("other_volumes");
        for (int i = 0; i < debugworld::kCategoryCount; ++i) {
            const auto idx = static_cast<std::size_t>(i);
            c.enabled[idx] = mirage::Get<bool>(kLayers[idx].key) && (!debugworld::IsOtherVolume(i) || others);
            c.colors[idx] = SettingArgb(std::string(kLayers[idx].key) + "_color");
        }
        return c;
    }

    debugrender::Style RenderStyle() const
    {
        debugrender::Style s;
        s.opacity = mirage::Get<float>("opacity");
        s.depth = mirage::Get<int>("depth") == 0 ? debugrender::Depth::Scene : debugrender::Depth::Overlay;
        s.fovOffset = mirage::Get<float>("fov_offset");
        s.staticOpacity = mirage::Get<int>("static_style") == 0 ? 0.f : mirage::Get<float>("static_opacity");
        s.aspect = mirage::Get<float>("aspect_override");
        return s;
    }

    staticworld::Config StaticConfig() const
    {
        staticworld::Config c;
        for (int i = 0; i < rstatic::kCategoryCount; ++i) {
            const auto idx = static_cast<std::size_t>(i);
            c.enabled[idx] = mirage::Get<bool>(kStaticLayers[idx].key);
            c.colors[idx] = SettingArgb(std::string(kStaticLayers[idx].key) + "_color");
        }
        c.nonColliding = mirage::Get<bool>("non_colliding");
        const int style = mirage::Get<int>("static_style");
        c.fill = style != 0;
        c.outline = style != 2;
        c.maxDistance = mirage::Get<float>("static_range");
        c.fade = mirage::Get<bool>("fade");
        c.filter = Trimmed(staticFilter_);
        c.vertexBudget = static_cast<std::size_t>(mirage::Get<int>("static_budget")) * 1000;
        return c;
    }

    // For the self-test: the look-at, the nearby volumes and the static geometry's state (twice a second)
    void ShareForTest()
    {
        if (shareTick_++ % 30 != 0) return;
        using nlohmann::json;
        const auto info = debugworld::GetInfo();
        json near = json::array();
        for (const auto& n : info.nearby) near.push_back({{"name", n.name}, {"distance", n.distance}, {"x", n.x}, {"y", n.y}, {"z", n.z}});
        json behind = json::array();
        for (const auto& [name, at] : info.look.behind) behind.push_back({{"name", name}, {"at", at}});
        json live = json::array();
        for (const auto& name : debugworld::LiveOwners(64)) live.push_back(name);
        const json inspect = {{"nearby", near}, {"live", live},
                              {"look", {{"valid", info.look.valid}, {"name", info.look.name}, {"distance", info.look.distance},
                                        {"behind", behind}}}};
        mirage::Api().share_set(mirage::Mod(), "inspect", inspect.dump().c_str());
        const auto st = staticworld::GetInfo();
        json found = json::array(), drawn = json::array();
        for (int c = 0; c < rstatic::kCategoryCount; ++c) {
            found.push_back(st.found[static_cast<std::size_t>(c)]);
            drawn.push_back(st.drawn[static_cast<std::size_t>(c)]);
        }
        const json stat = {{"map", st.map}, {"scanning", st.scanning}, {"meshes", st.meshes}, {"vertices", st.vertices},
                           {"build_ms", st.buildMs}, {"budget_hit", st.budgetHit}, {"tick_ms", st.tickAvgMs},
                           {"tick_max_ms", st.tickMaxMs}, {"found", found}, {"drawn", drawn}};
        mirage::Api().share_set(mirage::Mod(), "static", stat.dump().c_str());
    }

    // ---- On screen ---------------------------------------------------------------------------------------------------
    // Top left of the screen: what the renderer drew and how (for checking the renderer, off by default)
    static void DrawStatsLine()
    {
        const auto st = debugrender::GetStats();
        const char* how = st.path == debugrender::Path::InScene ? "in-scene (depth)" :
                          st.path == debugrender::Path::Overlay ? "overlay" : "-";
        char line[200];
        std::snprintf(line, sizeof line, "Collision view: %zu tris, %zu lines, %zu shapes, build %.1f ms, %s%s%s",
                      st.triangles, st.lines, st.shapes, st.buildMs, how,
                      st.overlayFallback ? " [FALLBACK: scene pass not found]" : "", st.drawn ? "" : " (not drawing yet)");
        mirage::Api().draw_text(11.f, 65.f, Rgba(0, 0, 0, 200), line);
        mirage::Api().draw_text(10.f, 64.f, st.overlayFallback ? Rgba(255, 190, 90) : Rgba(200, 200, 210), line);
    }

    uint32_t LayerColor(int category, float alpha = 1.f) const
    {
        const auto idx = static_cast<std::size_t>(std::clamp(category, 0, debugworld::kCategoryCount - 1));
        return ToDraw(SettingArgb(std::string(kLayers[idx].key) + "_color"), alpha);
    }

    // Under the crosshair: what the view ray hits first
    void DrawLookAt() const
    {
        const auto& look = info_.look;
        if (!look.valid) return;
        char line2[96];
        std::snprintf(line2, sizeof line2, "%s  |  %.0f away  |  %.0f across", debugworld::CategoryName(look.category),
                      look.distance, look.size);
        float sw = 0.f, sh = 0.f, lw = 0.f, lh = 0.f;
        mirage::Api().screen_size(&sw, &sh);
        mirage::Api().text_size("A", &lw, &lh);
        const char* name = look.name.empty() ? "(unnamed)" : look.name.c_str();
        const float y = sh * 0.5f + 28.f;
        ShadowText(sw * 0.5f - TextWidth(name) * 0.5f, y, LayerColor(look.category), name);
        ShadowText(sw * 0.5f - TextWidth(line2) * 0.5f, y + lh, Rgba(220, 220, 225), line2);
        if (!look.behind.empty()) {
            std::string more = "behind: " + look.behind[0].first;
            if (look.behind.size() > 1) more += " +" + std::to_string(look.behind.size() - 1);
            ShadowText(sw * 0.5f - TextWidth(more.c_str()) * 0.5f, y + 2.f * lh, Rgba(160, 160, 170), more.c_str());
        }
    }

    // Top left: the volumes the player is in
    void DrawInsideList() const
    {
        if (info_.inside.empty()) return;
        float lw = 0.f, lh = 0.f;
        mirage::Api().text_size("A", &lw, &lh);
        float y = mirage::Get<bool>("stats_line") ? 86.f : 64.f;
        ShadowText(10.f, y, Rgba(200, 200, 210), "Inside:");
        for (const auto& [name, category] : info_.inside) {
            y += lh;
            char line[160];
            std::snprintf(line, sizeof line, "%s  (%s)", name.empty() ? "(unnamed)" : name.c_str(), debugworld::CategoryName(category));
            ShadowText(18.f, y, LayerColor(category), line);
        }
    }

    // Labels over the projected center of each volume: a small dark plate with the layer's color on its edge, above a
    // dot on the point. The nearest go first, the rest only where they don't overlap one already placed; the volume
    // under the crosshair always gets its label, outlined.
    void DrawLabels() const
    {
        const auto scene = debugrender::CurrentScene();
        if (!scene || scene->labels.empty()) return;
        const rmath::Camera cam = debugrender::CurrentCamera();
        float sw = 0.f, sh = 0.f, bw = 0.f, base = 0.f;
        mirage::Api().screen_size(&sw, &sh);
        mirage::Api().text_size("A", &bw, &base);
        const float range = std::max(mirage::Get<float>("label_distance"), 1.f);

        struct Item {
            const rmesh::Label* label;
            float x, y, distance;
            bool look;
        };
        std::vector<Item> items;
        items.reserve(scene->labels.size());
        for (const auto& label : scene->labels) {
            float x = 0.f, y = 0.f;
            if (!debugrender::WorldToScreen(label.pos, x, y)) continue;
            if (x < -60.f || y < -30.f || x > sw + 60.f || y > sh + 30.f) continue;
            const bool look = info_.look.valid && label.text == info_.look.name;
            items.push_back({&label, x, y, rmath::Length(label.pos - cam.pos), look});
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.look != b.look ? a.look : a.distance < b.distance; });

        const MirageApi& api = mirage::Api();
        struct Box { float x0, y0, x1, y1; };
        std::vector<Box> placed;
        constexpr int kMaxShown = 60;
        for (const Item& it : items) {
            if (static_cast<int>(placed.size()) >= kMaxShown) break;
            const float t = std::clamp(it.distance / range, 0.f, 1.f);
            const float size = it.look ? base : base * (1.f - 0.2f * t);
            const float alpha = it.look ? 1.f : (t < 0.6f ? 1.f : std::max(0.25f, 1.f - (t - 0.6f) / 0.4f * 0.75f));
            const char* text = it.label->text.c_str();
            float tw = 0.f, th = 0.f;
            api.text_size_sized(text, size, &tw, &th);
            constexpr float padX = 5.f, padY = 2.f, bar = 3.f, gap = 6.f;
            const float w = tw + 2.f * padX + bar, h = th + 2.f * padY;
            const Box b{std::round(it.x - w * 0.5f), std::round(it.y - gap - h), std::round(it.x - w * 0.5f) + w, std::round(it.y - gap - h) + h};
            bool overlaps = false;
            for (const Box& r : placed)
                if (b.x0 < r.x1 + 2.f && b.x1 > r.x0 - 2.f && b.y0 < r.y1 + 2.f && b.y1 > r.y0 - 2.f) {
                    overlaps = true;
                    break;
                }
            if (overlaps && !it.look) continue;
            placed.push_back(b);
            const uint32_t color = ToDraw(it.label->color, alpha);
            api.draw_circle_filled(it.x, it.y, 2.5f, color);
            api.draw_line(it.x, it.y - 2.5f, it.x, b.y1, ToDraw(it.label->color, alpha * 0.6f), 1.f);
            api.draw_rect_rounded(b.x0, b.y0, b.x1, b.y1, Rgba(12, 12, 14, static_cast<int>(170.f * alpha)), 3.f, 0.f);
            api.draw_rect_rounded(b.x0, b.y0, b.x0 + bar, b.y1, color, 0.f, 0.f);
            if (it.look) api.draw_rect_rounded(b.x0, b.y0, b.x1, b.y1, Rgba(255, 255, 255, 230), 3.f, 1.f);
            api.draw_text_sized(b.x0 + bar + padX, b.y0 + padY, Rgba(240, 240, 245, static_cast<int>(255.f * alpha)), size, text);
        }
    }

    // ---- The panel -----------------------------------------------------------------------------------------------------
    // One layer: its color, shown or not, and how many are drawn of how many there are
    static void LayerCell(const Layer& l, std::size_t drawn, std::size_t total, const char* totalOf)
    {
        Ui().id_push(l.key);
        const std::string colorKey = std::string(l.key) + "_color";
        auto c = static_cast<uint32_t>(mirage::Get<int>(colorKey));
        if (Ui().color_edit("##color", &c)) mirage::Set(colorKey, static_cast<int>(c));
        Ui().same_line(0.f, 4.f);
        int32_t shown = mirage::Get<bool>(l.key) ? 1 : 0;
        if (Ui().checkbox(l.label, &shown)) mirage::Set(l.key, shown != 0);
        if (l.tooltip[0] && Ui().item_hovered()) Ui().tooltip(l.tooltip);
        if (total > 0) {
            Ui().same_line(0.f, -1.f);
            char n[48];
            std::snprintf(n, sizeof n, "%zu/%zu", drawn, total);
            Ui().text_disabled(n);
            if (Ui().item_hovered()) {
                char tip[160];
                std::snprintf(tip, sizeof tip, "%zu drawn (in range and matching the filter) of %zu %s", drawn, total, totalOf);
                Ui().tooltip(tip);
            }
        }
        Ui().id_pop();
    }

    void DrawLayers()
    {
        if (Ui().table_begin("##layers", 2, 0, 0.f, 0.f)) {
            for (int i = 0; i < debugworld::kCategoryCount; ++i) {
                const auto idx = static_cast<std::size_t>(i);
                if (debugworld::IsOtherVolume(i) && i != static_cast<int>(debugworld::Category::Volume)) continue;
                Ui().table_next_column();
                if (i != static_cast<int>(debugworld::Category::Volume)) {
                    LayerCell(kLayers[idx], info_.drawn[idx], info_.remembered[idx],
                              i == static_cast<int>(debugworld::Category::Cylinder) ? "found nearby" : "in this level");
                    continue;
                }
                // The group switch, with the counts of all its kinds together
                int32_t on = mirage::Get<bool>("other_volumes") ? 1 : 0;
                if (Ui().checkbox("Other volumes##group", &on)) mirage::Set("other_volumes", on != 0);
                if (Ui().item_hovered())
                    Ui().tooltip("Every other kind of volume, in the groups below: post process, audio, fog, streaming and the rest");
                std::size_t drawn = 0, remembered = 0;
                for (int j = 0; j < debugworld::kCategoryCount; ++j)
                    if (debugworld::IsOtherVolume(j)) {
                        drawn += info_.drawn[static_cast<std::size_t>(j)];
                        remembered += info_.remembered[static_cast<std::size_t>(j)];
                    }
                if (remembered > 0) {
                    Ui().same_line(0.f, -1.f);
                    char n[48];
                    std::snprintf(n, sizeof n, "%zu/%zu", drawn, remembered);
                    Ui().text_disabled(n);
                }
            }
            Ui().table_end();
        }
        // The kinds of other volumes, indented under the group
        Ui().indent(0.f);
        Ui().disabled_begin(!mirage::Get<bool>("other_volumes"));
        if (Ui().table_begin("##othervolumes", 2, 0, 0.f, 0.f)) {
            for (int i = 0; i < debugworld::kCategoryCount; ++i) {
                if (!debugworld::IsOtherVolume(i)) continue;
                const auto idx = static_cast<std::size_t>(i);
                Ui().table_next_column();
                LayerCell(kLayers[idx], info_.drawn[idx], info_.remembered[idx], "in this level");
            }
            Ui().table_end();
        }
        Ui().disabled_end();
        Ui().unindent(0.f);
    }

    void DrawFilter(float width)
    {
        Ui().next_width(width);
        Ui().input_text_hint("##filter", "Filter by name (e.g. kill)", filter_, sizeof filter_, 0);
        if (Ui().item_hovered())
            Ui().tooltip("Only volumes whose name contains this. Names are the level's actor names: TdKillVolume, TdTriggerVolume, BlockingVolume, ...");
    }

    void DrawInspectInfo()
    {
        const auto& look = info_.look;
        char line[200];
        if (look.valid) {
            Ui().text_disabled("Looking at");
            Ui().same_line(0.f, -1.f);
            Ui().text(look.name.empty() ? "(unnamed)" : look.name.c_str());
            std::snprintf(line, sizeof line, "%s, %.0f away, %.0f across", debugworld::CategoryName(look.category), look.distance, look.size);
            Ui().text_disabled(line);
            std::snprintf(line, sizeof line, "hit at %.0f, %.0f, %.0f", look.x, look.y, look.z);
            Ui().text_disabled(line);
            if (Ui().item_clicked(0)) {
                std::snprintf(line, sizeof line, "%.0f %.0f %.0f", look.x, look.y, look.z);
                Ui().clipboard_set(line);
            }
            if (Ui().item_hovered()) Ui().tooltip("Click to copy");
            for (const auto& [name, at] : look.behind) {
                std::snprintf(line, sizeof line, "  behind it: %s (%.0f)", name.c_str(), at);
                Ui().text_disabled(line);
            }
        } else Ui().text_disabled("Not looking at a volume");
        if (info_.inside.empty()) Ui().text_disabled("Not inside any volume");
        else {
            std::snprintf(line, sizeof line, "Inside %zu volume%s", info_.inside.size(), info_.inside.size() == 1 ? "" : "s");
            Ui().text_disabled(line);
        }

        // The closest volumes: click one to turn toward it
        if (info_.nearby.empty()) return;
        Ui().spacing();
        Ui().text_disabled("Nearby (click to face it)");
        float dw = 0.f, dh = 0.f;
        Ui().text_size("00000", &dw, &dh);
        if (!Ui().table_begin("##nearby", 2, MIRAGE_TABLE_ROW_BG, 0.f, 0.f)) return;
        Ui().table_column("name", MIRAGE_COLUMN_STRETCH, 0.f);
        Ui().table_column("dist", MIRAGE_COLUMN_FIXED, dw);
        for (std::size_t i = 0; i < info_.nearby.size(); ++i) {
            const auto& n = info_.nearby[i];
            Ui().table_next_row();
            Ui().table_next_column();
            Ui().id_push_int(static_cast<int32_t>(i));
            Ui().color_push(MIRAGE_COLOR_TEXT, LayerColor(n.category));
            if (Ui().selectable(n.name.empty() ? "(unnamed)" : n.name.c_str(), 0, 0.f, 0.f)) debugworld::FaceTowards(n.x, n.y, n.z);
            Ui().color_pop(1);
            if (Ui().item_hovered()) {
                std::snprintf(line, sizeof line, "%s at %.0f, %.0f, %.0f", debugworld::CategoryName(n.category), n.x, n.y, n.z);
                Ui().tooltip(line);
            }
            Ui().id_pop();
            Ui().table_next_column();
            if (n.distance <= 0.f) Ui().text_disabled("in");
            else {
                std::snprintf(line, sizeof line, "%.0f", n.distance);
                Ui().text_disabled(line);
            }
        }
        Ui().table_end();
    }

    void DrawStaticLayers()
    {
        const auto info = staticworld::GetInfo();
        const bool on = mirage::Get<bool>("static_on");
        Ui().disabled_begin(!on);
        if (Ui().table_begin("##static", 2, 0, 0.f, 0.f)) {
            for (int i = 0; i < rstatic::kCategoryCount; ++i) {
                const auto idx = static_cast<std::size_t>(i);
                Ui().table_next_column();
                LayerCell(kStaticLayers[idx], info.drawn[idx], on ? info.found[idx] : 0, "in this level");
            }
            Ui().table_end();
        }
        Ui().disabled_end();
    }

    void DrawStaticFilter(float width)
    {
        Ui().next_width(width);
        Ui().input_text_hint("##staticfilter", "Filter meshes by name", staticFilter_, sizeof staticFilter_, 0);
        if (Ui().item_hovered()) Ui().tooltip("Only meshes whose actor or mesh name contains this (e.g. door, rail, glass)");
    }

    static void DrawStaticStatus()
    {
        const auto info = staticworld::GetInfo();
        char line[200];
        if (info.scanning) {
            std::snprintf(line, sizeof line, "Reading the level... %.0f%% (%zu meshes so far)", info.scanProgress * 100.f, info.meshes);
            Ui().text_wrapped(line);
        } else if (!info.map.empty()) {
            std::snprintf(line, sizeof line, "%zu meshes; drawing %zu k vertices, built in %.1f ms on a worker thread", info.meshes,
                          info.vertices / 1000, info.buildMs);
            Ui().text_wrapped(line);
        }
        if (!info.map.empty()) {
            std::snprintf(line, sizeof line, "Game thread: %.2f ms a tick on average (slowest %.1f ms)", info.tickAvgMs, info.tickMaxMs);
            Ui().text_disabled(line);
        }
        if (info.budgetHit) Ui().text_colored(Rgba(255, 191, 77), "Vertex limit reached: the farthest meshes are left out");
    }

    // This level's cache in one line, the bake's progress, and the buttons for both
    void DrawCacheStatus(float width)
    {
        const auto info = debugworld::GetInfo();
        const float half = (width - 8.f) * 0.5f;
        char line[200];
        std::snprintf(line, sizeof line, "%s: %zu volumes, %s", info.map.empty() ? "No level" : info.map.c_str(), info.shapes,
                      info.cacheDirty ? "not saved yet" : info.fromDisk ? "cached" : "this session");
        Ui().text_disabled(line);
        if (Ui().item_hovered()) {
            std::snprintf(line, sizeof line, "%zu scans, the last took %.1f ms", info.scans, info.lastScanMs);
            Ui().tooltip(line);
        }
        if (info.baking) {
            std::snprintf(line, sizeof line, "%zu / %zu checkpoints", info.bakeDone, info.bakeTotal);
            Ui().progress_bar(info.bakeTotal ? static_cast<float>(info.bakeDone) / static_cast<float>(info.bakeTotal) : 0.f, width, 0.f, line);
        }
        if (!info.bakeStatus.empty()) {
            Ui().text_wrapped(((info.baking ? "" : "Last bake: ") + info.bakeStatus).c_str());
            if (info.bakeFailed) {
                std::snprintf(line, sizeof line, "%zu checkpoint(s) did not load and were skipped", info.bakeFailed);
                Ui().text_colored(Rgba(255, 153, 77), line);
            }
        }
        if (info.baking) {
            if (Ui().button("Cancel baking", width, 0.f)) debugworld::CancelBake();
            if (Ui().item_hovered()) Ui().tooltip("Saves this checkpoint and takes you back to the level you were in");
        } else {
            if (Ui().button("Bake missing", half, 0.f)) debugworld::BakeAllLevels(true);
            if (Ui().item_hovered())
                Ui().tooltip("Loads every checkpoint of each level not baked yet, in story order, and caches its volumes. A few seconds per checkpoint; you are taken back here afterwards");
            Ui().same_line(0.f, -1.f);
            if (Ui().button("Rebake all", half, 0.f)) debugworld::BakeAllLevels(false);
            if (Ui().item_hovered()) Ui().tooltip("The same for every level, baked or not");
        }
        if (Ui().button("Rescan", half, 0.f)) debugworld::RescanNow();
        if (Ui().item_hovered()) Ui().tooltip("Look for volumes in this level again now");
        Ui().same_line(0.f, -1.f);
        if (Ui().button(confirmClear_ ? "Sure?" : "Clear level", half, 0.f)) {
            if (confirmClear_) debugworld::ClearCache();
            confirmClear_ = !confirmClear_;
        }
        if (!Ui().item_hovered()) confirmClear_ = false;
        else Ui().tooltip("Forget this level's volumes (and its cache file), then scan it again");
    }

    bool confirmClear_ = false;
    bool staticShown_ = false;          // Game thread: static geometry was being drawn
    unsigned shareTick_ = 0;
    char staticFilter_[64] = {};
    char filter_[64] = {};
    float panelFrame_ = -10.f;
    debugworld::Info info_;             // Render thread: this frame's copy
    std::string lastFilter_;            // Game thread: the filter the scene was built with
};
MIRAGE_MODULE(CollisionView)

} // namespace

MIRAGE_MOD()
