// Info Lines: the player's own lines of text on the screen, written in the language of template.h. Profiles are text
// files in this mod's hud\<name>.txt, edited in the menu (or any editor: a changed file is read again). A profile that
// has no file yet is the built-in example; it is written only once it is edited.
//
// A native first-party mod: the public API (mirage/api.h) and SDK (mirage/sdk.hpp) only. What it drew is shared for the
// menu's self-test (mirage.shared("mirage-menu", "status.*")), which also asks it to evaluate a line
// (mirage/info_lines.eval) and to draw its editor's folded parts open (mirage/info_lines.open_for_test).
#include "template.h"

#include "mirage/sdk.hpp"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using hud::Value;

const char* const kExample =
R"(// Info Lines: your own lines on the screen. Edit them here (changes show right away) or in
// memenu\hud\<profile>.txt with any editor. "Values" and "Functions" below the editor list what you can show.
//
// == <place>, <option> <value>, ... ==   starts a section. Places: top left, top, top right, left, center, right,
//    bottom left, bottom, bottom right, at <x> <y> (pixels, or 50% for half the screen)
//    Options: scale 1.2  bg 0.5  padding 6  spacing 2  offset 10 40  width 160  align right
//             color #rrggbb  shadow off  outline on
// {speed}  {speed:.1f}  {x:>9.2f}  {health:03d}  {map:^12}    values, with fmt format specs
// {speed * 0.036:.1f}  {max(a, b)}  {round(z / 100, 1)}         math
// {if(speed > 700, "&(green)fast", "slow")}                    a choice ("strings" can hold {values} too)
// &(red) &(accent) &#ff8800 &#ff880080   text color from there on; &r goes back to the section's color
// Speed &> {speed}     text after &> goes in a second, right-aligned column ("&&" is a literal &)
// {bar(health / maxhealth, 80, "red")}  a bar     {time(gametime)}  1:02.35
// #if <condition> / #elif / #else / #endif   show lines only sometimes.  // starts a comment line

== top right, offset 0 40, bg 0.45, padding 6 ==
#if haspawn
&(gray)Speed &>{mix("white", "#40ff70", clamp((speed - 400) / 500, 0, 1))}{speed:.0f} &(gray)u/s
&(gray)Top speed &>{topspeed:.0f} &(gray)u/s
&(gray)Height &>{z:.0f}
&(gray)Move &>{statename}
#if health < maxhealth
&(gray)Health &>{bar(health / maxhealth, 70, if(health < 40, "red", "white"))}
#endif
#if reactionactive
&(accent)Reaction time &>{reaction:.0f}%
#endif
#else
&(gray)Info Lines: load a level
#endif
)";

// TdPawn.EMovement, by value (the SDK's names without MOVE_)
const char* MoveName(int m)
{
    static const char* const kNames[] = {
        "None", "Walking", "Falling", "Grabbing", "WallRunningRight", "WallRunningLeft", "WallClimbing", "SpringBoarding",
        "SpeedVaulting", "VaultOver", "GrabPullUp", "Jump", "WallRunJump", "GrabJump", "IntoGrab", "Crouch", "Slide",
        "Melee", "Snatch", "Barge", "Landing", "Climb", "IntoClimb", "WallKick", "180Turn", "180TurnInAir",
        "LayOnGround", "IntoZipLine", "ZipLine", "Balance", "LedgeWalk", "GrabTransfer", "MeleeAir", "DodgeJump",
        "WallRunDodgeJump", "Stumble", "Snatched", "StepUp", "RumpSlide", "Interact", "WallRun", "BotStop",
        "BotStartWalking", "BotStartRunning", "BotTurnRunning", "BotTurnStanding", "ExitCover", "Vertigo",
        "MeleeSlide", "WallClimbDodgeJump", "WallClimb180TurnJump", "WallClimbDodgeJumpLeft",
        "WallClimbDodgeJumpRight", "MeleeVault", "BotMeleeSecondSwing", "StumbleHard", "BotRoll", "BotFlip", nullptr,
        nullptr, "Swing", "Coil", "MeleeWallrun", "MeleeCrouch", "BotJumpShort", "BotJumpMedium", "BotJumpLong",
        "JumpIntoGrab", "StandGrabHeaveBot", "BotMeleeDodge", "FinishAttack", "MeleeBarge", "FallingUncontrolled",
        "SwingJump", "AnimationPlayback", "EnterCover", "Cover", "StumbleFalling", "SoftLanding",
        "HeadButtedByCeleste", nullptr, "AutoStepUp", "MeleeAirAbove", nullptr, "Block", "AirBarge",
    };
    if (m >= 0 && m < static_cast<int>(std::size(kNames)) && kNames[m]) return kNames[m];
    return "?";
}

// ---- The values -------------------------------------------------------------------------------------------------------
struct GameData {       // Copied on the game thread
    bool hasPawn = false;
    double x = 0, y = 0, z = 0, vx = 0, vy = 0, vz = 0;
    double topSpeed = 0, topHeight = 0;
    double pitch = 0, yaw = 0;
    int health = 0, maxHealth = 100, move = 0;
    double reaction = 0, jumpZ = 0;
    bool reactionActive = false, onGround = false;
    std::string map;
    bool loading = false;
    double gameSpeed = 1, gameTime = 0, realTime = 0;
    unsigned deaths = 0, loads = 0;
    unsigned long long ticks = 0;
};

struct Frame {          // Everything a line can show, for one frame
    GameData game;
    double fps = 0, frameMs = 0;
    bool menuOpen = false;
    std::tm local{};
    double runTime = 0, runRta = 0;
};

struct VarDef {
    const char* name;
    const char* group;
    const char* help;
    std::function<Value(const Frame&)> get;
};

double Hypot(double a, double b) { return std::sqrt(a * a + b * b); }

const std::vector<VarDef>& Vars()
{
    using F = const Frame&;
    static const std::vector<VarDef> vars = {
        {"x", "Player", "Position X (game units, 1 = 1 cm)", [](F f) { return Value::Num(f.game.x); }},
        {"y", "Player", "Position Y", [](F f) { return Value::Num(f.game.y); }},
        {"z", "Player", "Position Z (height)", [](F f) { return Value::Num(f.game.z); }},
        {"speed", "Player", "Horizontal speed (units/s)", [](F f) { return Value::Num(Hypot(f.game.vx, f.game.vy)); }},
        {"speed3d", "Player", "Speed in every direction (units/s)",
         [](F f) { return Value::Num(std::sqrt(f.game.vx * f.game.vx + f.game.vy * f.game.vy + f.game.vz * f.game.vz)); }},
        {"vspeed", "Player", "Vertical speed (units/s, up is positive)", [](F f) { return Value::Num(f.game.vz); }},
        {"kmh", "Player", "Horizontal speed in km/h", [](F f) { return Value::Num(Hypot(f.game.vx, f.game.vy) * 0.036); }},
        {"ms", "Player", "Horizontal speed in m/s", [](F f) { return Value::Num(Hypot(f.game.vx, f.game.vy) * 0.01); }},
        {"topspeed", "Player", "Highest horizontal speed of the last 2.75 s", [](F f) { return Value::Num(f.game.topSpeed); }},
        {"topz", "Player", "Highest Z of the last 2.75 s", [](F f) { return Value::Num(f.game.topHeight); }},
        {"pitch", "Player", "Looking up (+) or down (-), degrees", [](F f) { return Value::Num(f.game.pitch); }},
        {"yaw", "Player", "Facing, degrees (0-360)", [](F f) { return Value::Num(f.game.yaw); }},
        {"health", "Player", "Health", [](F f) { return Value::Num(f.game.health); }},
        {"maxhealth", "Player", "Full health", [](F f) { return Value::Num(f.game.maxHealth); }},
        {"state", "Player", "Movement state, as a number", [](F f) { return Value::Num(f.game.move); }},
        {"statename", "Player", "Movement state (Walking, Falling, WallRunningLeft, ...)",
         [](F f) { return Value::Str(MoveName(f.game.move)); }},
        {"onground", "Player", "Standing or running on the ground", [](F f) { return Value::Bool(f.game.onGround); }},
        {"reaction", "Player", "Reaction time meter, 0-100", [](F f) { return Value::Num(f.game.reaction); }},
        {"reactionactive", "Player", "Reaction time is on", [](F f) { return Value::Bool(f.game.reactionActive); }},
        {"jumpz", "Player", "Z where the last jump started", [](F f) { return Value::Num(f.game.jumpZ); }},
        {"sincejump", "Player", "Height above the last jump's start", [](F f) { return Value::Num(f.game.z - f.game.jumpZ); }},
        {"haspawn", "Player", "There is a player (false in menus and while loading)", [](F f) { return Value::Bool(f.game.hasPawn); }},

        {"map", "World", "Level package (edge_p, ...)", [](F f) { return Value::Str(f.game.map); }},
        {"loading", "World", "A level is loading", [](F f) { return Value::Bool(f.game.loading); }},
        {"gamespeed", "World", "Game speed (1 = normal)", [](F f) { return Value::Num(f.game.gameSpeed); }},
        {"gametime", "World", "Seconds of game time in this level (slows down with the game)", [](F f) { return Value::Num(f.game.gameTime); }},
        {"realtime", "World", "Seconds of real time in this level", [](F f) { return Value::Num(f.game.realTime); }},
        {"deaths", "World", "Deaths since the game started", [](F f) { return Value::Num(f.game.deaths); }},
        {"loads", "World", "Save states loaded", [](F f) { return Value::Num(f.game.loads); }},
        {"ticks", "World", "Engine ticks since the menu loaded", [](F f) { return Value::Num(static_cast<double>(f.game.ticks)); }},

        {"runtime", "Speedrun", "Speedrun timer, loads removed (seconds: use time(runtime))", [](F f) { return Value::Num(f.runTime); }},
        {"runrta", "Speedrun", "Speedrun timer, real time (seconds)", [](F f) { return Value::Num(f.runRta); }},

        {"fps", "Menu", "Frames per second", [](F f) { return Value::Num(f.fps); }},
        {"frametime", "Menu", "Milliseconds per frame", [](F f) { return Value::Num(f.frameMs); }},
        {"menuopen", "Menu", "The menu is open", [](F f) { return Value::Bool(f.menuOpen); }},
        {"clock", "Menu", "Time of day, 14:05:09", [](F f) {
             char b[16];
             std::strftime(b, sizeof b, "%H:%M:%S", &f.local);
             return Value::Str(b);
         }},
        {"date", "Menu", "Today, 2026-11-09", [](F f) {
             char b[16];
             std::strftime(b, sizeof b, "%Y-%m-%d", &f.local);
             return Value::Str(b);
         }},
        {"hour", "Menu", "Hour of the day (0-23)", [](F f) { return Value::Num(f.local.tm_hour); }},
        {"minute", "Menu", "Minute (0-59)", [](F f) { return Value::Num(f.local.tm_min); }},
        {"second", "Menu", "Second (0-59)", [](F f) { return Value::Num(f.local.tm_sec); }},
    };
    return vars;
}

const VarDef* FindVar(std::string_view name)
{
    static const std::unordered_map<std::string_view, const VarDef*> byName = [] {
        std::unordered_map<std::string_view, const VarDef*> m;
        for (const VarDef& v : Vars()) m.emplace(v.name, &v);
        return m;
    }();
    const auto it = byName.find(name);
    return it == byName.end() ? nullptr : it->second;
}

// Colors: the API packs them 0xAABBGGRR, the template language 0xRRGGBBAA
hud::Color FromApi(uint32_t c)
{
    return ((c & 0xFFu) << 24) | (((c >> 8) & 0xFFu) << 16) | (((c >> 16) & 0xFFu) << 8) | ((c >> 24) & 0xFFu);
}
uint32_t ToApi(hud::Color c, float alpha = 1.f)
{
    const auto a = std::clamp(static_cast<int>(static_cast<float>(c & 0xFF) * alpha), 0, 255);
    return ((c >> 24) & 0xFFu) | (((c >> 16) & 0xFFu) << 8) | (((c >> 8) & 0xFFu) << 16) | (static_cast<uint32_t>(a) << 24);
}
uint32_t Rgba(int r, int g, int b, int a = 255)
{
    return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
}

class FrameContext final : public hud::Context {
public:
    explicit FrameContext(const Frame& f) : f_(f) {}

    bool Variable(std::string_view name, Value& out) const override
    {
        const VarDef* v = FindVar(name);
        if (!v) return false;
        out = v->get(f_);
        return true;
    }

    bool Call(std::string_view name, const std::vector<Value>& args, Value& out, std::string& error) const override
    {
        if (name != "on") return false;
        if (args.size() != 1 || args[0].kind != Value::Kind::Text) {
            error = "on(\"Module name\") takes the name of a module";
            return true;
        }
        const int state = mirage::Api().module_state(args[0].text.c_str());
        if (state < 0) error = "there is no module '" + args[0].text + "'";
        else out = Value::Bool(state == 1);
        return true;
    }

    bool NamedColor(std::string_view name, hud::Color& out) const override
    {
        const MirageApi& api = mirage::Api();
        if (name == "accent") out = FromApi(api.theme_color(MIRAGE_THEME_ACCENT));
        else if (name == "text") out = FromApi(api.theme_color(MIRAGE_THEME_TEXT));
        else if (name == "dim") out = FromApi(api.theme_color(MIRAGE_THEME_TEXT_DIM));
        else return false;
        return true;
    }

private:
    const Frame& f_;
};

// Highest value of the last 2.75 s (like the Speedometer's top speed)
struct Peak {
    double value = 0.0, at = -1e9;
    bool any = false;
    void Update(double v, double now)
    {
        if (now - at > 2.75) any = false;
        if (!any || v > value) { value = v; at = now; any = true; }
    }
};

const char* const kFolder = "hud";
std::string PathOf(const std::string& name) { return std::string(kFolder) + "/" + name + ".txt"; }

double SharedNumber(const char* mod, const char* key)
{
    char v[48]{};
    mirage::Api().share_get(mod, key, v, sizeof v);
    return std::atof(v);
}
std::string SharedText(const char* mod, const char* key)
{
    char v[512]{};
    mirage::Api().share_get(mod, key, v, sizeof v);
    return v;
}
void Share(const char* key, const std::string& value) { mirage::Api().share_set(mirage::Mod(), key, value.c_str()); }

class InfoLines final : public mirage::Module {
public:
    InfoLines() : Module("Info Lines", "Visual", "Your own lines of text on the screen: live values, math, colors, conditions", 30)
    {
        mirage::On(MIRAGE_EVENT_LEVEL_LOAD, [this](const MirageEvent& e) { loading_ = e.value == 0; });
        mirage::On(MIRAGE_EVENT_DEATH, [this](const MirageEvent& e) { if (e.value == 1) ++deaths_; });
        mirage::On(MIRAGE_EVENT_TICK, [this](const MirageEvent&) { ++ticks_; });
    }

    void OnEnable() override
    {
        std::lock_guard lock(gameMutex_);
        game_ = {};
        topSpeed_ = {};
        topHeight_ = {};
    }

    void OnDisable() override { frames_ = 0; }

    // Game thread: copy what the lines can show
    void OnTick() override
    {
        GameData d;
        auto* pawn = mirage::sdk::Pawn();
        auto* controller = mirage::sdk::Controller();
        auto* world = mirage::sdk::World();
        if (pawn && controller) {
            d.hasPawn = true;
            d.x = pawn->Location.X;
            d.y = pawn->Location.Y;
            d.z = pawn->Location.Z;
            d.vx = pawn->Velocity.X;
            d.vy = pawn->Velocity.Y;
            d.vz = pawn->Velocity.Z;
            d.pitch = static_cast<std::int16_t>(controller->Rotation.Pitch & 0xFFFF) / 65536.0 * 360.0;
            d.yaw = static_cast<double>(controller->Rotation.Yaw & 0xFFFF) / 65536.0 * 360.0;
            d.health = pawn->Health;
            d.maxHealth = pawn->MaxHealth > 0 ? pawn->MaxHealth : 100;
            d.move = static_cast<int>(pawn->MovementState.GetValue());
            d.reaction = std::min(100.0, static_cast<double>(controller->ReactionTimeEnergy));
            d.reactionActive = controller->bReactionTime;
            d.onGround = pawn->Physics == Classes::EPhysics::PHYS_Walking;
            d.jumpZ = pawn->LastJumpLocation.Z;
        }
        if (world) {
            d.gameSpeed = world->TimeDilation;
            d.gameTime = world->TimeSeconds;
            d.realTime = world->RealTimeSeconds;
            if (d.hasPawn) {
                topSpeed_.Update(Hypot(d.vx, d.vy), d.realTime);
                topHeight_.Update(d.z, d.realTime);
            }
        }
        if (!d.hasPawn) {
            topSpeed_ = {};
            topHeight_ = {};
        }
        d.topSpeed = topSpeed_.value;
        d.topHeight = topHeight_.value;
        d.map = mirage::sdk::MapName();
        d.loading = loading_;
        d.deaths = deaths_;
        d.ticks = ticks_;
        d.loads = static_cast<unsigned>(SharedNumber("mirage", "save_state.loads"));    // The menu's Save State

        std::lock_guard lock(gameMutex_);
        game_ = std::move(d);
    }

    void OnRender() override
    {
        Sync();
        const Frame frame = MakeFrame();
        FrameContext ctx(frame);
        std::vector<hud::Error> errors;
        const std::vector<hud::RenderedSection> sections = profile_.Render(ctx, &errors);
        renderErrors_ = errors;

        std::size_t lines = 0;
        const bool hidden = mirage::Get<bool>("hide_in_menu") && frame.menuOpen;
        for (const auto& s : sections) {
            lines += s.lines.size();
            if (!hidden) DrawSection(s);
        }
        if (mirage::Get<bool>("show_errors") && !hidden) DrawErrorNote();

        // For the self-test: what it drew, and a line it was asked to evaluate with this frame's values
        ++frames_;
        std::vector<hud::Error> all = profile_.Errors();
        all.insert(all.end(), errors.begin(), errors.end());
        Share("status.frames", std::to_string(frames_));
        Share("status.sections", std::to_string(sections.size()));
        Share("status.lines", std::to_string(lines));
        Share("status.profile", loadedName_);
        Share("status.errors", std::to_string(all.size()));
        Share("status.first_error", all.empty() ? std::string() : "line " + std::to_string(all[0].line) + ": " + all[0].message);
        const std::string eval = SharedText("mirage", "info_lines.eval");
        if (!eval.empty()) {
            std::vector<hud::Error> evalErrors;
            Share("eval.text", hud::PlainText(hud::RenderLine(eval, ctx, 0xFFFFFFFF, &evalErrors)));
            Share("eval.error", evalErrors.empty() ? std::string() : evalErrors[0].message);
            Share("eval.of", eval);
        }
    }

    void OnPanel() override
    {
        mirage::SetVisible("profile", false);     // Picked with the editor's list
        DrawEditor(mirage::Ui().content_width());
    }

private:
    static bool OpenForTest() { return SharedText("mirage", "info_lines.open_for_test") == "1"; }

    // ---- Profile files ------------------------------------------------------------------------------------------------
    void Load(const std::string& name)
    {
        loadedName_ = name;
        std::string text;
        fromFile_ = mirage::ReadFile(PathOf(name), text);
        text_ = fromFile_ ? text : std::string(kExample);
        stamp_ = fromFile_ ? mirage::Api().file_stamp(mirage::Mod(), PathOf(name).c_str()) : 0;
        dirty_ = false;
        Reparse();
    }

    void Reparse()
    {
        profile_ = hud::Profile::Parse(text_);
        renderErrors_.clear();
    }

    bool Save()
    {
        if (!mirage::WriteFile(PathOf(loadedName_), text_)) {
            mirage::Api().notify(mirage::Mod(), ("Info Lines: could not save " + PathOf(loadedName_)).c_str());
            mirage::Warn("could not write " + PathOf(loadedName_));
            return false;
        }
        stamp_ = mirage::Api().file_stamp(mirage::Mod(), PathOf(loadedName_).c_str());
        fromFile_ = true;
        dirty_ = false;
        return true;
    }

    // Once a frame: the picked profile is the loaded one, edits are saved after a moment, a file changed outside is
    // read again
    void Sync()
    {
        std::string picked = mirage::Get<std::string>("profile");
        if (picked.empty()) {
            picked = "default";
            mirage::Set("profile", picked);
        }
        if (loadedName_ != picked) {
            if (dirty_) Save();
            Load(picked);
        }
        const float now = mirage::Ui().time();
        if (dirty_ && now - lastEdit_ > 0.8f) Save();
        if (!dirty_ && now - lastCheck_ > 1.f) {
            lastCheck_ = now;
            const uint64_t t = mirage::Api().file_stamp(mirage::Mod(), PathOf(loadedName_).c_str());
            if (t && (!fromFile_ || t != stamp_)) {
                Load(loadedName_);
                mirage::Info(PathOf(loadedName_) + " changed, read again");
            }
        }
    }

    std::vector<std::string> ProfileNames() const
    {
        std::vector<std::string> names{"default"};
        for (const std::string& f : mirage::ListFiles(kFolder, ".txt")) names.push_back(f.substr(0, f.size() - 4));
        names.push_back(loadedName_);
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        return names;
    }

    Frame MakeFrame()
    {
        Frame f;
        {
            std::lock_guard lock(gameMutex_);
            f.game = game_;
        }
        const MirageApi& api = mirage::Api();
        f.fps = api.frame_rate();
        f.frameMs = f.fps > 0.0 ? 1000.0 / f.fps : 0.0;
        f.menuOpen = api.menu_open() != 0;
        const std::time_t t = std::time(nullptr);
        localtime_s(&f.local, &t);
        f.runTime = SharedNumber("memenu-speedrun", "run.lrt");     // The Speedrun Timer mod's run
        f.runRta = SharedNumber("memenu-speedrun", "run.rta");
        return f;
    }

    // ---- Drawing ------------------------------------------------------------------------------------------------------
    static float TextWidth(const std::string& text, float size)
    {
        float w = 0.f, h = 0.f;
        mirage::Api().text_size_sized(text.c_str(), size, &w, &h);
        return w;
    }

    void DrawSection(const hud::RenderedSection& s) const
    {
        if (s.lines.empty()) return;
        const hud::SectionStyle& st = s.style;
        float W = 0.f, H = 0.f;
        mirage::Api().screen_size(&W, &H);
        const float scale = st.scale * mirage::Get<float>("scale");
        const float size = FontSize() * scale;
        const float gap = size * 0.8f;

        const auto width = [&](const std::vector<hud::Segment>& segs) {
            float w = 0.f;
            for (const auto& seg : segs)
                w += seg.bar ? seg.width * scale : TextWidth(seg.text, size);
            return w;
        };

        struct Measured { float left, right; };
        std::vector<Measured> m;
        m.reserve(s.lines.size());
        float plainMax = 0.f, leftMax = 0.f, rightMax = 0.f;
        bool anyRight = false;
        for (const auto& line : s.lines) {
            const Measured mm{width(line.left), width(line.right)};
            m.push_back(mm);
            if (line.hasRight) {
                anyRight = true;
                leftMax = std::max(leftMax, mm.left);
                rightMax = std::max(rightMax, mm.right);
            } else {
                plainMax = std::max(plainMax, mm.left);
            }
        }
        const float contentW = std::max({plainMax, anyRight ? leftMax + gap + rightMax : 0.f, st.minWidth * scale});
        const float spacing = st.spacing * scale;
        const float contentH = static_cast<float>(s.lines.size()) * size + static_cast<float>(s.lines.size() - 1) * spacing;
        const float pad = st.padding * scale;
        const float boxW = contentW + 2.f * pad, boxH = contentH + 2.f * pad;

        const float margin = 10.f;
        float x = margin, y = margin;
        using A = hud::Anchor;
        switch (st.anchor) {
            case A::Top: case A::Center: case A::Bottom: x = (W - boxW) * 0.5f; break;
            case A::TopRight: case A::Right: case A::BottomRight: x = W - boxW - margin; break;
            default: break;
        }
        switch (st.anchor) {
            case A::Left: case A::Center: case A::Right: y = (H - boxH) * 0.5f; break;
            case A::BottomLeft: case A::Bottom: case A::BottomRight: y = H - boxH - margin; break;
            default: break;
        }
        if (st.anchor == A::At) {
            x = st.xPercent ? W * st.x / 100.f : st.x;
            y = st.yPercent ? H * st.y / 100.f : st.y;
        }
        x = std::floor(x + st.offsetX);
        y = std::floor(y + st.offsetY);

        if (st.background > 0.f)
            mirage::Api().draw_rect_rounded(x, y, x + boxW, y + boxH, Rgba(0, 0, 0, static_cast<int>(st.background * 255.f)),
                                            4.f * scale, 0.f);

        const float x0 = x + pad;
        float ly = y + pad;
        for (std::size_t i = 0; i < s.lines.size(); ++i) {
            const auto& line = s.lines[i];
            float lx = x0;
            if (!line.hasRight) {
                if (st.align == hud::Align::Center) lx = x0 + (contentW - m[i].left) * 0.5f;
                else if (st.align == hud::Align::Right) lx = x0 + contentW - m[i].left;
            }
            DrawSegments(line.left, lx, ly, size, scale, st);
            if (line.hasRight) DrawSegments(line.right, x0 + contentW - m[i].right, ly, size, scale, st);
            ly += size + spacing;
        }
    }

    // The menu's text size (the API draws with the menu's font)
    static float FontSize()
    {
        float w = 0.f, h = 0.f;
        mirage::Ui().text_size("A", &w, &h);
        return h > 0.f ? h : 16.f;
    }

    static void DrawSegments(const std::vector<hud::Segment>& segs, float x, float y, float size, float scale,
                             const hud::SectionStyle& st)
    {
        const MirageApi& api = mirage::Api();
        x = std::floor(x);
        for (const auto& seg : segs) {
            if (seg.bar) {
                const float w = seg.width * scale, h = std::max(3.f, size * 0.55f);
                const float by = std::floor(y + (size - h) * 0.5f);
                api.draw_rect_rounded(x, by, x + w, by + h, Rgba(0, 0, 0, 140), 2.f, 0.f);
                if (seg.fraction > 0.f)
                    api.draw_rect_rounded(x + 1.f, by + 1.f, x + 1.f + (w - 2.f) * seg.fraction, by + h - 1.f, ToApi(seg.color), 1.5f, 0.f);
                x += w;
                continue;
            }
            const char* text = seg.text.c_str();
            const float alpha = static_cast<float>(seg.color & 0xFF) / 255.f;
            const uint32_t dark = Rgba(0, 0, 0, static_cast<int>(200.f * alpha));
            if (st.outline) {
                const float d[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
                for (const auto& o : d) api.draw_text_sized(x + o[0], y + o[1], dark, size, text);
            } else if (st.shadow) {
                api.draw_text_sized(x + 1.f, y + 1.f, dark, size, text);
            }
            api.draw_text_sized(x, y, ToApi(seg.color), size, text);
            x += TextWidth(seg.text, size);
        }
    }
    // A note at the top of the screen while the profile has errors (it may be edited outside the menu)
    void DrawErrorNote() const
    {
        const std::size_t count = profile_.Errors().size() + renderErrors_.size();
        if (count == 0) return;
        const hud::Error& e = profile_.Errors().empty() ? renderErrors_.front() : profile_.Errors().front();
        const std::string text = "Info Lines (" + loadedName_ + "): " + std::to_string(count) + " problem" +
                                 (count == 1 ? "" : "s") + ", line " + std::to_string(e.line) + ": " + e.message;
        const MirageApi& api = mirage::Api();
        float W = 0.f, H = 0.f, tw = 0.f, th = 0.f;
        api.screen_size(&W, &H);
        api.text_size(text.c_str(), &tw, &th);
        const float x = std::floor((W - tw) * 0.5f);
        api.draw_text(x + 1.f, 9.f, Rgba(0, 0, 0, 200), text.c_str());
        api.draw_text(x, 8.f, Rgba(255, 110, 100), text.c_str());
    }

    // ---- The editor (in the module's panel) ---------------------------------------------------------------------------
    void DrawEditor(float width)
    {
        const MirageUi& ui = mirage::Ui();
        Share("status.editor_frames", std::to_string(++editorFrames_));
        Sync();
        const uint32_t dim = mirage::Api().theme_color(MIRAGE_THEME_TEXT_DIM);

        // Profile: pick one, or make a new one from this one
        const std::vector<std::string> names = ProfileNames();
        std::string items;
        int index = 0;
        for (std::size_t i = 0; i < names.size(); ++i) {
            items += names[i];
            items.push_back('\0');
            if (names[i] == loadedName_) index = static_cast<int>(i);
        }
        items.push_back('\0');
        ui.next_width(width - 110.f);
        if (ui.combo("##profile", &index, items.c_str())) mirage::Set("profile", names[static_cast<std::size_t>(index)]);
        if (ui.item_hovered()) ui.tooltip("Profile: hud\\<name>.txt in this mod's folder");
        ui.same_line(0.f, -1.f);
        if (ui.button("New", 48.f, 0.f)) {
            int n = 2;
            while (std::find(names.begin(), names.end(), "profile " + std::to_string(n)) != names.end()) ++n;
            const std::string copyOf = text_;
            if (dirty_) Save();
            loadedName_ = "profile " + std::to_string(n);
            text_ = copyOf;
            mirage::Set("profile", loadedName_);
            Save();
            mirage::Api().notify(mirage::Mod(), ("Info Lines: new profile '" + loadedName_ + "'").c_str());
        }
        if (ui.item_hovered()) ui.tooltip("A new profile, starting as a copy of this one");
        ui.same_line(0.f, -1.f);
        if (ui.button("Folder", ui.content_width(), 0.f)) mirage::Api().file_open_folder(mirage::Mod(), kFolder);
        if (ui.item_hovered()) ui.tooltip("Open the profiles' folder: edit them with any editor, they are read again when saved");

        // The text: edited in a buffer with room to grow (made larger as it fills)
        if (buffer_.size() < text_.size() + 4096) buffer_.resize(text_.size() + 16384);
        std::copy(text_.begin(), text_.end(), buffer_.begin());
        buffer_[text_.size()] = '\0';
        ui.font_push(MIRAGE_FONT_MONO);
        const float lineH = ui.line_height();
        const bool edited = ui.input_text("##text", buffer_.data(), static_cast<uint32_t>(buffer_.size()), MIRAGE_INPUT_MULTILINE,
                                          lineH * static_cast<float>(mirage::Get<int>("editor_lines")) + 8.f) != 0;
        ui.font_pop();
        if (edited) {
            text_ = buffer_.data();
            Reparse();
            dirty_ = true;
            lastEdit_ = ui.time();
        }

        if (dirty_) ui.text_colored(dim, "Saving...");
        else if (fromFile_) ui.text_colored(dim, ("Saved: hud\\" + loadedName_ + ".txt").c_str());
        else ui.text_colored(dim, "The built-in example (saved to a file once you edit it)");
        if (!Enabled()) ui.text_colored(Rgba(255, 191, 89), "Off: turn it on to see the lines and the values");

        // Problems, with their line
        std::vector<hud::Error> problems = profile_.Errors();
        problems.insert(problems.end(), renderErrors_.begin(), renderErrors_.end());
        if (problems.empty()) {
            ui.text_colored(Rgba(115, 217, 128), "No problems");
        } else {
            ui.color_push(MIRAGE_COLOR_TEXT, Rgba(255, 115, 102));
            for (std::size_t i = 0; i < problems.size() && i < 10; ++i) {
                const auto& p = problems[i];
                ui.text_wrapped(((p.line ? "line " + std::to_string(p.line) + ": " : std::string()) + p.message).c_str());
            }
            ui.color_pop(1);
            if (problems.size() > 10) ui.text_disabled(("... and " + std::to_string(problems.size() - 10) + " more").c_str());
        }

        if (ui.button("Back to the example", width, 0.f)) {
            text_ = kExample;
            Reparse();
            dirty_ = true;
            lastEdit_ = ui.time();
        }
        if (ui.item_hovered()) ui.tooltip("Replace this profile's text with the built-in example");

        DrawValues(width);
        DrawReference();
    }

    void DrawValues(float width)
    {
        const MirageUi& ui = mirage::Ui();
        if (OpenForTest()) ui.next_open(1, 1);
        if (!ui.tree_node("Values", 0)) return;
        const Frame frame = MakeFrame();
        ui.text_disabled("Click a name to copy {name}");
        const char* heading = nullptr;
        if (ui.table_begin("##values", 2, MIRAGE_TABLE_ROW_BG | MIRAGE_TABLE_SIZING_FIXED, width - 20.f, 0.f)) {
            ui.table_column("name", MIRAGE_COLUMN_FIXED, 110.f);
            ui.table_column("value", MIRAGE_COLUMN_STRETCH, 0.f);
            for (const VarDef& v : Vars()) {
                if (!heading || std::string_view(heading) != v.group) {
                    heading = v.group;
                    ui.table_next_row();
                    ui.table_next_column();
                    ui.text_colored(mirage::Api().theme_color(MIRAGE_THEME_ACCENT), heading);
                    ui.table_next_column();
                }
                ui.table_next_row();
                ui.table_next_column();
                if (ui.selectable(v.name, 0, 0.f, 0.f)) {
                    ui.clipboard_set((std::string("{") + v.name + "}").c_str());
                    mirage::Api().notify(mirage::Mod(), (std::string("Copied {") + v.name + "}").c_str());
                }
                if (ui.item_hovered()) ui.tooltip(v.help);
                ui.table_next_column();
                std::string text, error;
                hud::FormatValue(v.get(frame), {}, text, error);
                ui.text(Enabled() ? text.c_str() : "-");
            }
            ui.table_end();
        }
        ui.tree_pop();
    }

    static void DrawReference()
    {
        const MirageUi& ui = mirage::Ui();
        if (OpenForTest()) ui.next_open(1, 1);
        if (ui.tree_node("Functions", 0)) {
            ui.text(
                "if(cond, a, b)        a when cond is true, else b\n"
                "abs floor ceil sqrt sign (x)\n"
                "min(a, b, ...)  max(a, b, ...)  clamp(x, lo, hi)\n"
                "round(x[, decimals])  lerp(a, b, t)\n"
                "time(seconds[, decimals])   1:02.35\n"
                "upper(s) lower(s) len(s)  str(x[, \"spec\"])\n"
                "bar(fraction[, width[, color]])\n"
                "color(\"red\")  rgb(r, g, b[, a])  mix(c1, c2, t)\n"
                "on(\"Module name\")     that module is on");
            ui.tree_pop();
        }
        if (OpenForTest()) ui.next_open(1, 1);
        if (ui.tree_node("Formatting", 0)) {
            ui.text(
                "{value:spec} takes fmt's format specs:\n"
                "  {speed:.1f}   one decimal       {x:>9.2f}  right in 9\n"
                "  {health:03d}  zero padded       {map:^12}  centered\n"
                "  {speed:+.0f}  with a sign       {n:x}      hex\n"
                "Operators: + - * / %  < <= > >= == !=  && || !\n"
                "Colors: &(name) &#rrggbb &#rrggbbaa  &r resets\n"
                "&> starts the right column, && is a literal &\n"
                "{{ and }} are literal braces");
            std::string names;
            for (const std::string& n : hud::ColorNames()) names += n + " ";
            ui.text_wrapped(("Color names: " + names + "accent text dim").c_str());
            ui.tree_pop();
        }
    }

    std::mutex gameMutex_;
    GameData game_;                     // Written by OnTick
    Peak topSpeed_, topHeight_;         // Game thread
    bool loading_ = false;              // Events (game thread)
    unsigned deaths_ = 0;
    unsigned long long ticks_ = 0;

    // Render thread (OnRender and the editor)
    std::string loadedName_;
    std::string text_;
    std::vector<char> buffer_;
    hud::Profile profile_;
    std::vector<hud::Error> renderErrors_;
    bool fromFile_ = false, dirty_ = false;
    float lastEdit_ = 0.f, lastCheck_ = 0.f;
    uint64_t stamp_ = 0;
    unsigned long long frames_ = 0, editorFrames_ = 0;
};
MIRAGE_MODULE(InfoLines)

} // namespace

MIRAGE_MOD()
