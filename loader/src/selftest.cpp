#include "selftest.h"
#include "diag.h"

#include "config.h"
#include "core/devmode.h"
#include "core/module.h"
#include "core/notify.h"
#include "core/scheduler.h"
#include "core/cache.h"
#include "core/setting.h"
#include "gui.h"
#include "hooks.h"
#include "log.h"
#include "paths.h"
#include "unload.h"
#if MIRAGE_GAME
#include "game/game.h"
#include "game/uobject.h"
#include "game/levels.h"
#include "game/object_cache.h"
#include "game/player_state.h"
#include "runtime/mod_game.h"
#include "runtime/call.h"
#include "runtime/engine_calls.h"
#include "runtime/fhooks.h"
#include "runtime/file_redirect.h"
#include "runtime/texture_overlays.h"
#include "core/cache_evict.h"
#include "core/cache_sched.h"
#include "game/object_query.h"
#include "runtime/class_source.h"
#include "runtime/classes.h"
#include "runtime/layers.h"
#include "runtime/maps.h"
#include "runtime/routes.h"
#include <imgui.h>
#include "runtime/inject.h"
#include "runtime/kismet.h"
#include "runtime/mod_internal.h"
#include "runtime/physics.h"
#include "runtime/kismet_trace.h"
#include "runtime/kismet_layers.h"
#include "runtime/detours.h"
#include "runtime/symbols.h"
#include "runtime/natives.h"
#include "runtime/script_patch.h"
#include "runtime/reflect.h"
#include "runtime/layout.h"
#include "runtime/lua_vm.h"
#include "core/events.h"
#include "core/tasks.h"
#include "runtime/crash_reporter.h"
#include "crashhandler/window.h"
#include "crashhandler/symbols.h"
#include "runtime/mod_dotnet.h"
#include "runtime/mem/arena.h"
#include "runtime/mem/manager.h"
#include "runtime/mem/pool.h"
#include "runtime/memory.h"
#include "runtime/mods.h"
#include "runtime/resolve.h"
#include "runtime/packages.h"
#if MIRAGE_FEATURE_MOD_MANAGER
#include "features/misc/mod_manager.h"
#endif
#include <shlobj.h>
#if MIRAGE_FEATURE_INPUT
#include "tas/engine.h"
#include "tas/input_monitor.h"
#endif

#include "sdk/SDK.hpp"
#include <cmath>
#include <cstdio>
#endif

#include <Windows.h>
#include <nlohmann/json.hpp>
#include "game/object_ref.h"
#include "util/float_util.h"
#include "runtime/world_events.h"
#include "runtime/changes.h"
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>
#include <string>
#include <thread>
#include <vector>

namespace selftest {
namespace {

using json = nlohmann::ordered_json;

struct Options {
    std::vector<std::string> modTests;   // "Mod tests": only these mods' tests (all when empty): `mirage mod test`
    std::string map = "edge_p";
    int checkpoint = 2;         // 0/1 = start of the level: the Prologue's intro cutscene holds the player there
    unsigned ticks = 10;        // Engine ticks (and frames) each module stays on
    std::set<std::string> only, skip;
    bool quit = true;
    // The engine's frame rate cap (62) off while testing, and TAS playbacks that don't depend on pacing as fast as they
    // go: the waits count frames and ticks, so they pass as fast as the PC draws. Off: the game as players run it.
    bool fast = true;
    bool keepLevel = false;     // Run from the menu: test in the level the player is in (no level load)
};

struct Step {
    std::string name;
    bool ok = true;
    std::vector<std::string> problems, notes;
    double seconds = 0.0;
};

bool g_requested = false;
Options g_options;
std::vector<Step> g_steps;
std::string g_current;          // Step in progress (written to the result, so a crash shows where it was)
ULONGLONG g_started = 0;
bool g_aborted = false;         // Unload requested (or Stop) in the middle: stop testing

// Runs from the menu (developer mode, Start): on a thread of their own, published for the Test module's window
std::atomic<bool> g_stop{false};        // Stop(): the waits give up, the run ends and puts everything back
std::atomic<bool> g_restoring{false};   // Putting settings back: waits work even after Stop or an unload request
std::atomic<bool> g_interactive{false}; // A run from the menu is going
std::thread g_worker;
std::mutex g_publishMutex;
Progress g_published;
int g_runs = 0;

// Buttons that would open windows on the desktop, delete files or run for minutes: never pressed
const std::set<std::string> kNoPress = {"Open log file", "Open themes folder", "Open save states folder",
                                        "Clear saved state"};   // Load State is tested next and needs it

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string Trim(const std::string& s)
{
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

std::set<std::string> SplitList(const std::string& s)
{
    std::set<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        if (auto t = Trim(item); !t.empty()) out.insert(t);
    return out;
}

std::filesystem::path ResultPath() { return paths::Dir() / "selftest-result.json"; }

// Copies the steps for the Test module's window (any thread reads them through Snapshot)
void Publish()
{
    std::lock_guard lock(g_publishMutex);
    g_published.seconds = static_cast<double>(GetTickCount64() - g_started) / 1000.0;
    g_published.steps.clear();
    for (const Step& st : g_steps) {
        StepView v;
        v.name = st.name;
        v.seconds = st.seconds;
        v.problems = st.problems;
        v.notes = st.notes;
        const bool current = &st == &g_steps.back() && st.name == g_current;
        v.state = !st.ok ? StepView::State::Failed : current ? StepView::State::Running : StepView::State::Passed;
        g_published.steps.push_back(std::move(v));
    }
}

void WriteResult(bool finished)
{
    Publish();
    json root;
    root["finished"] = finished;
    bool passed = !g_steps.empty();
    for (const Step& s : g_steps) passed &= s.ok;
    root["passed"] = finished && passed;
    root["current"] = g_current;
    root["seconds"] = static_cast<double>(GetTickCount64() - g_started) / 1000.0;
    json steps = json::array();
    for (const Step& s : g_steps) {
        json j;
        j["name"] = s.name;
        j["ok"] = s.ok;
        j["seconds"] = s.seconds;
        j["problems"] = s.problems;
        j["notes"] = s.notes;
        steps.push_back(std::move(j));
    }
    root["steps"] = std::move(steps);
    const auto path = ResultPath();
    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << root.dump(2);
    }
    // `mirage game-test` reads the file several times a second: a replace while it is open fails, so try again (the
    // last write, finished = true, once got lost that way and the run waited for nothing)
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (MoveFileExA(tmp.c_str(), path.string().c_str(), MOVEFILE_REPLACE_EXISTING)) return;
        Sleep(25);
    }
    spdlog::warn("Self-test: {} couldn't be written ({})", path.string(), GetLastError());
}

// ---- Waiting --------------------------------------------------------------------------------------------------------
// Unload requested or Stop() pressed: the test ends. Not while settings are put back afterwards (that must finish).
bool Interrupted()
{
    return !g_restoring && (g_stop || unload::Requested());
}

// Sleeps in small steps; false if unload was requested meanwhile (the test stops then).
bool Wait(double seconds)
{
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.0);
    while (GetTickCount64() < end) {
        if (Interrupted()) { g_aborted = true; return false; }
        Sleep(1);
    }
    return true;
}

// Waits until `cond` holds or `seconds` pass. True if it held.
bool WaitFor(double seconds, const std::function<bool()>& cond)
{
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000.0);
    while (GetTickCount64() < end) {
        if (cond()) return true;
        if (Interrupted()) { g_aborted = true; return false; }
        Sleep(1);
    }
    return cond();
}

#if MIRAGE_GAME
unsigned long long Ticks() { return game::GetStatus().ticks; }
#endif

// Waits until `n` more frames are drawn and the game ticked `n` more times (the slower of the two decides; ticks don't
// count while a level loads). Waiting for ticks and frames instead of seconds keeps the test short on a fast machine
// and fair on a slow one. A stall of `timeout` seconds is logged as an error, which fails the step it happens in.
bool WaitTicks(unsigned n, double timeout = 10.0)
{
    const unsigned frames0 = hooks::EndSceneCalls();
#if MIRAGE_GAME
    const auto ticks0 = Ticks();
    const auto done = [&] {
        const bool ticked = !game::Ready() || game::IsLoading() || Ticks() >= ticks0 + n;
        return hooks::EndSceneCalls() >= frames0 + n && ticked;
    };
#else
    const auto done = [&] { return hooks::EndSceneCalls() >= frames0 + n; };
#endif
    if (WaitFor(timeout, done) || g_aborted) return true;
    spdlog::error("Stall: {} frames and ticks did not come within {:.0f} s (frames {} -> {})", n, timeout, frames0,
                  hooks::EndSceneCalls());
    return false;
}

// Work handed to another thread that may outlive the wait for it: callers pass closures that capture their own stack
// by reference, so a job the wait gave up on (a level load stops the game thread's queue) must never run later.
// Abandoning takes the lock, so a job is either finished before its wait returns or never runs at all.
struct Job {
    std::mutex mutex;
    bool done = false, abandoned = false;
};

template <class Post>
bool RunAndWait(Post&& post, std::function<void()> fn, double timeout)
{
    auto job = std::make_shared<Job>();
    post([job, fn = std::move(fn)] {
        std::lock_guard lock(job->mutex);
        if (job->abandoned) return;
        fn();
        job->done = true;
    });
    if (WaitFor(timeout, [&] { std::lock_guard lock(job->mutex); return job->done; })) return true;
    std::lock_guard lock(job->mutex);
    job->abandoned = !job->done;
    return job->done;
}

// Runs `fn` on the render thread and waits for it (like a click in the menu). False if no frame came to run it.
bool OnRenderThread(std::function<void()> fn, double timeout = 5.0)
{
    return RunAndWait([](std::function<void()> f) { hooks::RunOnRenderThread(std::move(f)); }, std::move(fn), timeout);
}

#if MIRAGE_GAME
std::atomic<int> g_loadsFinished{0};
void OnLevelLoad(bool finished) { if (finished) ++g_loadsFinished; }

// Waits for a level load that may be running (or about to start) to finish and the game to tick again.
bool SettleAfterLoad(double seconds)
{
    if (!WaitFor(seconds, [] { return !game::IsLoading(); })) return false;
    const auto t = Ticks();
    return WaitFor(seconds, [&] { return Ticks() > t + 5; });
}

// Runs `fn` on the game thread (in the next engine tick) and waits for it. False if the game didn't tick.
bool OnGameThread(std::function<void()> fn, double timeout = 5.0)
{
    return RunAndWait([](std::function<void()> f) { game::RunOnGameThread(std::move(f)); }, std::move(fn), timeout);
}

// Why the player can't be moved right now ("" when they can): no pawn, a cutscene, input held back by the level's
// scripting, frozen. After a load the game holds the player for a moment; a count of ticks is no measure of that (with
// the frame rate cap off, 30 ticks pass in a tenth of a second), this is.
std::string PlayerNotReady()
{
    std::string why = "the game thread did not answer";
    OnGameThread([&] {
        auto* pawn = game::Pawn();
        auto* pc = game::Controller();
        if (!pawn || !pc) why = "no player";
        else if (pawn->bCutsceneIsSkippable || pc->bCinematicMode) why = "in a cutscene";
        else if (pc->bIgnoreMoveInput) why = "the level holds back the player's input";
        else if (pc->bFrozen) why = "the player is frozen";
        else why.clear();
    }, 1.0);
    return why;
}

// Waits until the player can be moved, then a quarter second more (the level's own scripts settle). The reason when not.
std::string WaitPlayerReady(double timeout = 30.0)
{
    std::string why;
    if (!WaitFor(timeout, [&] { why = PlayerNotReady(); return why.empty(); })) return why;
    Wait(0.25);
    return PlayerNotReady();
}

// Evaluates `check` on the game thread: "" when it holds, else what is wrong.
std::string Verify(const std::function<std::string()>& check)
{
    std::string result = "the game thread did not run the check";
    OnGameThread([&] { result = check(); });
    return result;
}

using Classes::ATdPlayerController;
using Classes::ATdPlayerPawn;

// The player, for checks: both or a problem text.
template <class F>
std::string WithPlayer(F&& f)
{
    ATdPlayerPawn* pawn = game::Pawn();
    ATdPlayerController* pc = game::Controller();
    if (!pawn || !pc) return "no player to check";
    return f(pawn, pc);
}

std::string Expect(bool ok, const char* what) { return ok ? std::string() : std::string(what); }

float Distance(const Classes::FVector& a, const Classes::FVector& b)
{
    const float dx = a.X - b.X, dy = a.Y - b.Y, dz = a.Z - b.Z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

Classes::FVector g_savedAt{};   // Where Save State was taken (Load State must come back to it)

// What a module must do to the game, beyond not breaking anything. Each part optional:
//   setup     game thread, before it is switched on / triggered
//   whileOn   game thread, half way through (actions: after the trigger)
//   afterOff  game thread, after it is switched off again
struct Expectation {
    std::function<void()> setup;
    std::function<std::string()> whileOn, afterOff;
};

// A value a mod shares (mirage.share), as a number
int SharedInt(const char* mod, const char* key)
{
    char v[32]{};
    mirage::mods::game_api::ShareGet(mod, key, v, sizeof v);
    return std::atoi(v);
}

Expectation ExpectationFor(Module& m)
{
    const std::string& name = m.Name();
    if (name == "God Mode")
        return {{},
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController* c) {
                    return Expect(c->bGodMode && p->Health == p->MaxHealth, "bGodMode not set or health not full"); }); },
                [] { return WithPlayer([](ATdPlayerPawn*, ATdPlayerController* c) {
                    return Expect(!c->bGodMode, "bGodMode still set after switching off"); }); }};
    if (name == "No Clip")
        return {{},
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController*) {
                    return Expect(!p->bCollideWorld && p->Physics == Classes::EPhysics::PHYS_None,
                                  "the player still collides or has physics"); }); },
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController*) {
                    return Expect(p->bCollideWorld, "collision not back on after switching off"); }); }};
    if (name == "Flight")
        return {{},
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController*) {
                    return Expect(p->Physics == Classes::EPhysics::PHYS_None, "the player still has physics"); }); },
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController*) {
                    return Expect(p->Physics != Classes::EPhysics::PHYS_None, "physics not back after switching off"); }); }};
    if (name == "Game Speed") {
        auto* slider = dynamic_cast<ctl::Slider*>(m.FindSetting("Multiplier"));
        return {[slider] { if (slider) slider->Set(0.5f); },
                [] { auto* w = game::World();
                     return Expect(w && std::fabs(w->TimeDilation - 0.5f) < 0.01f, "TimeDilation is not the multiplier (0.5)"); },
                [] { auto* w = game::World();
                     return Expect(w && std::fabs(w->TimeDilation - 1.f) < 0.01f, "TimeDilation not back to 1"); }};
    }
    if (name == "Info Lines") {
        // A native first-party mod (mods/memenu-info-lines): what it drew comes from what it shares (status.*), a line
        // is evaluated by asking it (mirage/info_lines.eval -> eval.*). The built-in example (a profile without a file),
        // never the player's own: it must parse and evaluate without a problem, draw lines with a player in the level,
        // and its values must be the game's (x through fmt).
        static std::string savedProfile;
        static bool savedExpanded = false;
        static int editorFramesBefore = 0;
        static constexpr const char* kTestProfile = "selftest (built-in example)";
        static constexpr const char* kLine = "{x:.0f}|{statename}|{map}";
        const auto text = [](const char* key) {
            char v[512]{};
            mirage::mods::game_api::ShareGet("memenu-info-lines", key, v, sizeof v);
            return std::string(v);
        };
        // The setting is read on the render thread: changed there too.
        return {[&m] {
                    editorFramesBefore = SharedInt("memenu-info-lines", "status.editor_frames");
                    mirage::mods::Share("info_lines.open_for_test", "1");
                    mirage::mods::Share("info_lines.eval", kLine);
                    hooks::RunOnRenderThread([&m] {
                        if (auto* t = dynamic_cast<ctl::Text*>(m.FindSetting("Profile"))) {
                            savedProfile = t->Get();
                            t->Set(kTestProfile);
                        }
                        savedExpanded = m.expanded;
                        m.expanded = true;                      // Its editor is drawn while the menu is open
                    });
                },
                [text] { return WithPlayer([text](ATdPlayerPawn* p, ATdPlayerController*) -> std::string {
                    WaitFor(2.0, [text] { return text("status.profile") == kTestProfile && text("eval.of") == kLine; });
                    if (SharedInt("memenu-info-lines", "status.frames") == 0) return "it never drew";
                    if (text("status.profile") != kTestProfile) return "it shows profile '" + text("status.profile") + "', not the test's";
                    if (SharedInt("memenu-info-lines", "status.errors") != 0) return "the example has problems: " + text("status.first_error");
                    if (SharedInt("memenu-info-lines", "status.lines") == 0) return "the example drew no lines with a player in the level";
                    if (!text("eval.error").empty()) return "evaluating values: " + text("eval.error");
                    const std::string t = text("eval.text");
                    const std::size_t bar = t.find('|');
                    const double x = std::atof(t.substr(0, bar).c_str());
                    if (std::fabs(x - p->Location.X) > 100.0)
                        return "{x} is " + t.substr(0, bar) + ", the player is at X " + std::to_string(p->Location.X);
                    if (t.find("|?") != std::string::npos || t.empty() || t.back() == '|') return "a value is missing: '" + t + "'";
                    return std::string(); }); },
                [&m] {
                    hooks::RunOnRenderThread([&m] {
                        if (auto* t = dynamic_cast<ctl::Text*>(m.FindSetting("Profile"))) t->Set(savedProfile);
                        m.expanded = savedExpanded;
                    });
                    mirage::mods::Share("info_lines.open_for_test", "");
                    mirage::mods::Share("info_lines.eval", "");
                    if (SharedInt("memenu-info-lines", "status.editor_frames") == editorFramesBefore)
                        return std::string("its editor was never drawn");
                    return std::string();
                }};
    }
#if MIRAGE_FEATURE_SCRIPT_EDITOR
    if (name == "Script Editor") {
        static std::uint64_t framesBefore = 0;
        return {[] { framesBefore = script_editor::Frames(); },
                [] { return script_editor::Frames() == framesBefore ? std::string("its window was never drawn") : std::string(); }};
    }
#endif
#if MIRAGE_FEATURE_MOD_MANAGER
    if (name == "Mod Manager") {
        // Its window is drawn while the menu is open, and lists every mod found (the first-party ones at least)
        static std::uint64_t framesBefore = 0;
        return {[] { framesBefore = mod_manager::Frames(); },
                [] {
                    if (mod_manager::Frames() == framesBefore) return std::string("its window was never drawn");
                    const std::size_t found = mirage::mods::List().size();
                    if (found == 0) return std::string("no mods found (the first-party ones are always there)");
                    if (mod_manager::Rows() != found)
                        return "it listed " + std::to_string(mod_manager::Rows()) + " mods of " + std::to_string(found);
                    return std::string();
                },
                {}};
    }
#endif
    // Save State, Load State, Save States, Refill Reaction Time: mods/memenu-savestate (native); what it shares, and the
    // commands the menu hands it (mirage/save_states.*)
    if (name == "Save State")
        return {[] {
                    static int n = 0;
                    mirage::mods::Share("save_states.command", std::to_string(++n) + " clear");
                    if (auto* p = game::Pawn()) g_savedAt = p->Location;
                },
                [] { return Expect(SharedInt("memenu-savestate", "has_save") == 1, "no state was saved"); }, {}};
    if (name == "Load State")
        return {[] { if (auto* p = game::Pawn()) { p->Location.X += 2000.f; p->Velocity = Classes::FVector{}; } },   // Move away
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController* c) {
                    const float d = Distance(p->Location, g_savedAt);
                    if (d >= 150.f) return "the player is " + std::to_string(static_cast<int>(d)) +
                                           " units from the saved position after loading it";
                    // The controller left in its default state ignores movement and jump input after a load. (The SDK's
                    // GetStateName() always answers None; game::IsInState works.)
                    if (game::IsInState(c, "Dead")) return std::string("the controller reads as Dead: the state check is broken");
                    return game::IsInState(c, "PlayerWalking")
                               ? std::string()
                               : std::string("the controller is not in PlayerWalking after loading (no move or jump input)"); }); },
                {}};
    if (name == "Refill Reaction Time")
        return {[] { if (auto* c = game::Controller()) c->ReactionTimeEnergy = 0.f; },
                [] { return WithPlayer([](ATdPlayerPawn*, ATdPlayerController* c) {
                    return Expect(c->ReactionTimeEnergy >= 99.f, "the reaction time meter was not refilled"); }); },
                {}};
    if (name == "Save States") {
        // In a folder of its own (in the mod's folder), never the player's states. The check reads the file back from
        // disk and puts its state back after moving away: the stored format must bring the player back, walking.
        static std::filesystem::path folder;
        return {[] {
                    folder = paths::Dir() / "mods" / "memenu-savestate" / "selftest-savestates";
                    std::error_code ec;
                    std::filesystem::remove_all(folder, ec);
                    mirage::mods::Share("save_states.folder", "selftest-savestates");
                    if (auto* p = game::Pawn()) g_savedAt = p->Location;
                },
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController* c) -> std::string {
                    if (SharedInt("memenu-savestate", "states") != 1) return "running it did not add one state";
                    char map[64]{};
                    mirage::mods::game_api::ShareGet("memenu-savestate", "map", map, sizeof map);
                    const auto file = folder / (std::string(map) + ".json");
                    std::ifstream in(file);
                    if (!in) return "no file was written";
                    const auto j = nlohmann::json::parse(in, nullptr, false);
                    if (j.is_discarded() || !j.contains("states") || j["states"].empty()) return "the file has no states";
                    p->Location.X += 2000.f;
                    p->Velocity = Classes::FVector{};
                    if (!player_state::Apply(j["states"][0]["state"])) return std::string("putting the stored state back failed");
                    const float d = Distance(p->Location, g_savedAt);
                    if (d >= 150.f) return "the player is " + std::to_string(static_cast<int>(d)) +
                                           " units from the stored position after loading it";
                    return Expect(game::IsInState(c, "PlayerWalking"),
                                  "the controller is not in PlayerWalking after loading");
                }); },
                [] {
                    mirage::mods::Share("save_states.folder", "");
                    std::error_code ec;
                    std::filesystem::remove_all(folder, ec);
                    return std::string();
                }};
    }
    if (name == "Clear Frame") {
        // The live engine (not its class default): one walk of the object list, it lives as long as the game
        static Classes::UEngine* engine = nullptr;
        static bool was = false;
        return {[] {
                    engine = nullptr;
                    const auto& objects = Classes::UObject::GetGlobalObjects();
                    for (std::size_t i = 0; i < objects.Num() && !engine; ++i) {
                        auto* o = objects.GetByIndex(i);
                        if (o && game::IsA(o, Classes::UGameEngine::StaticClass()) && o->GetName().rfind("Default__", 0) != 0)
                            engine = static_cast<Classes::UEngine*>(o);
                    }
                    if (engine) was = engine->bEnableColorClear != 0;
                },
                [] { return Expect(engine && engine->bEnableColorClear, "the engine's bEnableColorClear is not set"); },
                [] { return Expect(engine && (engine->bEnableColorClear != 0) == was,
                                   "bEnableColorClear not put back after switching off"); }};
    }
    if (name == "FOV") {
        auto* slider = dynamic_cast<ctl::Slider*>(m.FindSetting("Degrees"));
        return {[slider] { if (slider) slider->Set(110.f); },
                [] { return WithPlayer([](ATdPlayerPawn*, ATdPlayerController* c) {
                    if (!c->PlayerCamera) return std::string("no player camera");
                    const float fov = c->PlayerCamera->GetFOVAngle();   // What the view is rendered with
                    return std::fabs(fov - 110.f) < 0.5f ? std::string()
                                                        : "the camera renders at " + std::to_string(fov) + " degrees, not 110"; }); },
                [] { return WithPlayer([](ATdPlayerPawn*, ATdPlayerController* c) {
                    return Expect(c->PlayerCamera && !c->PlayerCamera->bLockedFOV, "the camera's FOV is still locked"); }); }};
    }
    if (name == "Teleport to Spawn") {
        // Moved away and turned a quarter: the teleport must bring both back (the turn is SetRotation, a native-index
        // function the SDK's call never ran)
        static int turnedYaw = 0;
        return {[] {
                    if (auto* p = game::Pawn()) { p->Location.Z += 600.f; p->Location.X += 300.f; g_savedAt = p->Location; }
                    if (auto* c = game::Controller()) { c->Rotation.Yaw += 16384; turnedYaw = static_cast<int>(c->Rotation.Yaw); }
                },
                [] { return WithPlayer([](ATdPlayerPawn* p, ATdPlayerController* c) {
                    char where[160];
                    std::snprintf(where, sizeof where, "the player was not moved: at (%.0f, %.0f, %.0f), moved away to (%.0f, %.0f, %.0f)",
                                  p->Location.X, p->Location.Y, p->Location.Z, g_savedAt.X, g_savedAt.Y, g_savedAt.Z);
                    if (Distance(p->Location, g_savedAt) <= 200.f) return std::string(where);
                    return Expect(static_cast<int>(c->Rotation.Yaw) != turnedYaw, "the view was not turned to the spawn point (SetRotation)"); }); },
                {}};
    }
    // Levels and Sublevels are mods/mirage-levels: what they read, from what it shares (refreshed every 60 ticks)
    if (name == "Levels")
        return {{}, [] {
                    WaitFor(3.0, [] { return SharedInt("mirage-levels", "game_maps") > 0; });
                    return Expect(SharedInt("mirage-levels", "game_maps") > 0 && SharedInt("mirage-levels", "maps") > 0,
                                  "the level list is not the game's (or is empty)"); }, {}};
    if (name == "Sublevels")
        return {{}, [] {
                    WaitFor(3.0, [] { return SharedInt("mirage-levels", "sublevels") > 0; });
                    return Expect(SharedInt("mirage-levels", "sublevels") > 0, "no streaming sublevels listed for this level"); },
                {}};
    return {};
}
#endif

// ---- Checks around a step -------------------------------------------------------------------------------------------
// Records the frame/tick counters and the log position when a step starts, and checks them when it ends.
class Probe {
public:
    explicit Probe(Step& step) : step_(step)
    {
        frames_ = hooks::EndSceneCalls();
#if MIRAGE_GAME
        ticks_ = Ticks();
#endif
        std::vector<logging::Entry> ignored;
        logId_ = logging::CopySince(0, ignored);        // Newest id
        start_ = GetTickCount64();
    }

    // `minFrames` frames must have been drawn, the game must have ticked (unless a level load may have stopped it),
    // and nothing may have logged an error. Warnings become notes.
    void Check(unsigned minFrames, bool expectTicks)
    {
        const unsigned frames = hooks::EndSceneCalls() - frames_;
        if (frames < minFrames)
            Problem("only " + std::to_string(frames) + " frames drawn (expected at least " + std::to_string(minFrames) +
                    "): rendering stalled");
#if MIRAGE_GAME
        const auto ticks = Ticks() - ticks_;
        if (expectTicks && game::Ready() && !game::IsLoading() && ticks == 0)
            Problem("the game did not tick: game thread stalled");
#else
        (void)expectTicks;
#endif
        std::vector<logging::Entry> entries;
        logging::CopySince(logId_, entries);
        for (const auto& e : entries) {
            if (e.text.rfind("Self-test", 0) == 0) continue;
            const bool expected = std::any_of(expected_.begin(), expected_.end(),
                                              [&](const std::string& part) { return e.text.find(part) != std::string::npos; });
            if (e.level >= logging::Level::Error && !expected) Problem("log: " + e.text);
            else if (e.level >= logging::Level::Warn) step_.notes.push_back("log " + std::string(expected ? "(expected)" : "warning") + ": " + e.text);
        }
        step_.seconds = static_cast<double>(GetTickCount64() - start_) / 1000.0;
    }

    // An error the step causes on purpose (a crash it provokes): log lines containing `part` aren't problems
    void Expect(std::string part) { expected_.push_back(std::move(part)); }

    void Problem(std::string text)
    {
        spdlog::error("Self-test: {}: {}", step_.name, text);
        step_.ok = false;
        step_.problems.push_back(std::move(text));
        Publish();
    }

private:
    Step& step_;
    std::vector<std::string> expected_;
    unsigned frames_ = 0;
    unsigned long long ticks_ = 0;
    std::uint64_t logId_ = 0;
    ULONGLONG start_ = 0;
};

Step& Begin(std::string name)
{
    spdlog::info("Self-test: {}...", name);
    g_current = name;
    g_steps.push_back({std::move(name)});
    WriteResult(false);
    return g_steps.back();
}

void End(Step& s)
{
    spdlog::info("Self-test: {} {}{}", s.name, s.ok ? "ok" : "FAILED", s.problems.empty() ? "" : ": " + s.problems.front());
    g_current.clear();
    WriteResult(false);
}

// ---- Steps ----------------------------------------------------------------------------------------------------------
bool TestFrames()
{
    Step& s = Begin("frames");
    const bool drew = WaitFor(30.0, [] { return hooks::EndSceneCalls() > 0; });
    if (!drew) {
        Probe(s).Problem("EndScene never ran in 30 s: the menu can't draw");
        End(s);
        return false;
    }
    Probe p(s);
    OnRenderThread([] { gui::visible = true; });
    WaitTicks(10);
    OnRenderThread([] { gui::visible = false; });
    WaitTicks(5);
    OnRenderThread([] { gui::visible = true; });
    WaitTicks(5);
    p.Check(20, false);
    End(s);
    return s.ok;
}

#if MIRAGE_GAME
bool TestGame()
{
    Step& s = Begin("game");
    Probe p(s);
    if (!game::Ready()) p.Problem("game integration not ready (patterns or hooks failed, see the log)");
    else if (!WaitFor(10.0, [t = Ticks()] { return Ticks() > t + 10; })) p.Problem("the engine tick hook does not run");
    p.Check(1, false);
    End(s);
    return s.ok;
}

// Where the level step left the player (a step that needs room around the player goes back there: earlier steps can
// reach later checkpoints, so a checkpoint restart may be somewhere else)
Classes::FVector g_levelStart{};
bool g_haveLevelStart = false;

bool TestLevel()
{
    if (g_options.keepLevel) {
        Step& s = Begin("level (current)");
        Probe p(s);
        if (!game::GetStatus().hasPawn || game::IsLoading())
            p.Problem("no player here: load a level first, or let the test load one");
        else if (const std::string why = WaitPlayerReady(10.0); !why.empty())
            p.Problem("the player can't be moved here: " + why);
        p.Check(1, true);
        End(s);
        return s.ok;
    }
    if (Lower(g_options.map) == "none") return true;
    Step& s = Begin("level " + g_options.map);
    Probe p(s);
    // The list fills from the game's own data a moment after startup
    std::vector<levels::MapView> maps;
    WaitFor(15.0, [&] { maps = levels::Maps(); return !maps.empty(); });
    const std::string want = Lower(g_options.map);
    int index = -1;
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (Lower(maps[i].label).find(want) != std::string::npos) { index = i; break; }
    if (index < 0) {
        p.Problem("no level matching '" + g_options.map + "' in the level list (" + std::to_string(maps.size()) + " levels)");
        End(s);
        return false;
    }
    const int checkpoint = std::min(g_options.checkpoint, static_cast<int>(maps[index].checkpoints.size()));
    s.notes.push_back("loading " + maps[index].label +
                      (checkpoint > 0 ? ", checkpoint " + maps[index].checkpoints[checkpoint - 1] : ", level start"));
    const int before = g_loadsFinished.load();
    OnRenderThread([index, checkpoint] { levels::Load(index, checkpoint, levels::Method::Game, false); });
    if (!WaitFor(120.0, [&] { return g_loadsFinished.load() > before; }))
        p.Problem("the level did not finish loading within 120 s");
    else if (!WaitFor(60.0, [] { return game::GetStatus().hasPawn && !game::IsLoading(); }))
        p.Problem("no player pawn 60 s after the level loaded");
    else if (const std::string why = WaitPlayerReady(30.0); !why.empty()) {
        // Nothing that moves the player could be checked
        p.Problem("30 s after loading, the player can't be moved: " + why + " (pick another checkpoint)");
    }
    s.notes.push_back(levels::LastMessage());
    OnGameThread([] {
        if (auto* pawn = game::Pawn()) {
            g_levelStart = pawn->Location;
            g_haveLevelStart = true;
        }
    });
    p.Check(1, true);
    End(s);
    return s.ok;
}

// While the menu is open the game must not turn the camera (bIgnoreLookInput), and must get it back when it closes.
void TestInputBlocking()
{
    Step& s = Begin("input blocking");
    Probe p(s);
    OnRenderThread([] { gui::visible = false; });
    WaitTicks(3);
    bool before = false;
    OnGameThread([&] { if (auto* c = game::Controller()) before = c->bIgnoreLookInput != 0; });
    OnRenderThread([] { gui::visible = true; });
    WaitTicks(3);
    if (const auto problem = Verify([] { return WithPlayer([](ATdPlayerPawn*, ATdPlayerController* c) {
            return Expect(c->bIgnoreLookInput, "look input not blocked while the menu is open"); }); });
        !problem.empty())
        p.Problem(problem);
    OnRenderThread([] { gui::visible = false; });
    WaitTicks(3);
    if (const auto problem = Verify([before] { return WithPlayer([before](ATdPlayerPawn*, ATdPlayerController* c) {
            return Expect((c->bIgnoreLookInput != 0) == before, "look input not given back when the menu closed"); }); });
        !problem.empty())
        p.Problem(problem);
    OnRenderThread([] { gui::visible = true; });
    p.Check(1, true);
    End(s);
}

// The player's own key for a binding command ("GBA_MoveForward"): keyboard keys only, no modifier combos. Game thread.
std::string KeyFor(ATdPlayerController* pc, const std::string& command, const char* fallback)
{
    if (pc && pc->PlayerInput) {
        auto& binds = pc->PlayerInput->Bindings;
        for (std::size_t i = 0; i < binds.Num(); ++i) {
            const auto& b = binds[i];
            if (b.Control || b.Shift || b.Alt || !b.Command.IsValid()) continue;
            const std::string cmd = b.Command.ToString(), key = b.Name.GetName();
            if (key.rfind("XboxTypeS_", 0) == 0 || key.rfind("GBA_", 0) == 0) continue;   // Gamepad, or an alias
            if (cmd.rfind(command, 0) == 0 && (cmd.size() == command.size() || cmd[command.size()] == ' ' ||
                                               cmd[command.size()] == '|'))
                return key;
        }
    }
    return fallback;
}

// The TAS module's "As fast as possible" for the length of a test (its playbacks then run unpaced), put back after.
class ScopedTasSpeed {
public:
    explicit ScopedTasSpeed(bool fast)
    {
        if (!fast) return;
        modules::ForEach([&](modules::Tab&, Module& m) {
            if (m.Name() == "TAS / Macro") box_ = dynamic_cast<ctl::Checkbox*>(m.FindSetting("As fast as possible"));
        });
        if (!box_) return;
        was_ = box_->Get();
        OnRenderThread([this] { box_->Set(true); });
        WaitTicks(2);
    }
    ~ScopedTasSpeed()
    {
        if (!box_) return;
        OnRenderThread([this] { box_->Set(was_); });
        WaitTicks(2);
    }
    ScopedTasSpeed(const ScopedTasSpeed&) = delete;
    ScopedTasSpeed& operator=(const ScopedTasSpeed&) = delete;

private:
    ctl::Checkbox* box_ = nullptr;
    bool was_ = false;
};



// The collision view's inspector: with the panel open it lists the volumes near the player; facing one of them (the
// Nearby list's click) must make the look-at report that same volume: as the one looked at, or on the line behind
// another volume that hides it (the "behind" list). Volumes the player is inside can't be checked that way: up to five
// are tried, and one has to match.
// Back on the ground and walking (or 10 s passed). The kick tests jump and fall: in fast mode a tick count is almost
// no game time, so the next test would start with the player still in the air.
void WaitGrounded()
{
    WaitFor(10.0, [] {
        bool ok = false;
        OnGameThread([&] {
            auto* pawn = game::Pawn();
            // Walking, not only on the ground: after a high fall the landing moves hold the player's velocity for a
            // while, and a step that launches the player (the bounce pads) would measure the landing instead
            ok = pawn && pawn->Physics == Classes::EPhysics::PHYS_Walking &&
                 pawn->MovementState == Classes::EMovement::MOVE_Walking;
        }, 1.0);
        return ok;
    });
    WaitTicks(5);
}

// What Auto Kickglitch's kick buffer did (mods/memenu-trainer shares it: kick_buffer.*)
struct KickBufferStats {
    int held = 0, onWall = 0, late = 0, dropped = 0;
    bool hooked = false;
};
KickBufferStats KickBuffer()
{
    const auto get = [](const char* key) {
        char v[32]{};
        mirage::mods::game_api::ShareGet("memenu-trainer", key, v, sizeof v);
        return std::atoi(v);
    };
    return {get("kick_buffer.held"), get("kick_buffer.on_wall"), get("kick_buffer.late"), get("kick_buffer.dropped"),
            get("kick_buffer.hooked") != 0};
}

// ---- Engine hooks ---------------------------------------------------------------------------------------------------
Step& BeginEngine(const char* name, Probe*& probe, std::unique_ptr<Probe>& holder)
{
    Step& s = Begin(name);
    holder = std::make_unique<Probe>(s);
    probe = holder.get();
    return s;
}

// Mirage step 9's UI layer, with tests/mods/ui-lua: its window (most widgets, every frame), its module panel, and a
// window and a color it leaves open on purpose, which the runtime closes for it: frames keep coming, and the menu's
// own windows still draw after it.
void TestMirageUi()
{
    Step& s = Begin("Mirage UI layer");
    Probe p(s);
    {
        bool docking = false;
        OnRenderThread([&] { docking = ImGui::GetCurrentContext() && (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable); });
        if (!docking) p.Problem("ImGui's docking isn't on (the docking branch, ImGuiConfigFlags_DockingEnable)");
    }
    Module* m = modules::Find("UI test (Lua)");
    if (!m) {
        p.Problem("the ui-lua test mod's module was not found (is tests/mods deployed?)");
        p.Check(1, false);
        End(s);
        return;
    }
    const auto shared = [](const char* key) {
        char v[32]{};
        mirage::mods::game_api::ShareGet("ui-lua", key, v, sizeof v);
        return std::atoi(v);
    };
    gui::OpenColumnForTest(m->Tab().c_str());
    const bool expandedWas = m->expanded, visibleWas = gui::visible;
    OnRenderThread([&] {
        gui::visible = true;        // The panel draws in the menu
        m->expanded = true;
        m->SetEnabled(true);
    });
    const int framesBefore = shared("frames"), panelBefore = shared("panel_frames");
    const bool drew = WaitFor(5.0, [&] { return shared("frames") >= framesBefore + 20 && shared("panel_frames") >= panelBefore + 20; });
    const int frames = shared("frames") - framesBefore, panel = shared("panel_frames") - panelBefore;
    OnRenderThread([&] {
        m->SetEnabled(false);
        m->expanded = expandedWas;
        gui::visible = visibleWas;
    });
    gui::OpenColumnForTest("");
    WaitTicks(3);
    s.notes.push_back("window drawn " + std::to_string(frames) + " frames, panel " + std::to_string(panel));
    if (!drew) p.Problem("the mod's window or panel did not keep drawing (an unclosed window broke the frame?)");
    p.Check(1, false);
    End(s);
}

std::atomic<int> g_tickBefore{0}, g_tickAfter{0};
std::atomic<bool> g_blockTick{false};

// Before, after and blocking watches on the controller's PlayerTick, an event the engine calls every tick.
void TestEngineScriptEvents()
{
    Probe* p;
    std::unique_ptr<Probe> holder;
    Step& s = BeginEngine("Engine script events", p, holder);
    static bool added = false;
    if (!added) {
        added = true;
        for (const char* fn : {"Function TdGame.TdPlayerController.PlayerTick", "Function Engine.PlayerController.PlayerTick"}) {
            game::OnScriptEvent(fn, [](Classes::UObject*, void*) { ++g_tickBefore; });
            game::OnScriptEvent(fn, [](Classes::UObject*, void*) { ++g_tickAfter; }, true);
            game::BlockScriptEvent(fn, [](Classes::UObject*, void*) { return g_blockTick.load(); });
        }
    }
    WaitTicks(10);      // Looked up on the next tick
    const int b0 = g_tickBefore, a0 = g_tickAfter;
    WaitTicks(10);
    const int b1 = g_tickBefore, a1 = g_tickAfter;
    if (b1 <= b0) p->Problem("the before-watch on PlayerTick never ran");
    else if (a1 <= a0) p->Problem("the after-watch on PlayerTick never ran");
    else {
        g_blockTick = true;
        WaitTicks(2);
        const int b2 = g_tickBefore, a2 = g_tickAfter;
        WaitTicks(5);
        const int b3 = g_tickBefore, a3 = g_tickAfter;
        g_blockTick = false;
        WaitTicks(5);
        const int a4 = g_tickAfter;
        if (b3 <= b2) p->Problem("while blocked, PlayerTick stopped being called at all");
        else if (a3 != a2) p->Problem("blocked, PlayerTick still ran (its after-watch did)");
        else if (a4 <= a3) p->Problem("PlayerTick did not run again after unblocking");
        else s.notes.push_back("before/after/block all work (" + std::to_string(b1 - b0) + " calls in 10 ticks)");
    }
    p->Check(1, false);
    End(s);
}

// "set" on the engine object, read back from the object itself.
// Mirage step 1: UnrealLib reads the game's script packages inside the game, and Lua runs, without stopping the game
// (the parse runs on this thread while the game keeps ticking and drawing), within a time and memory budget.
void TestMirageRuntime()
{
    constexpr double kMaxSeconds = 20.0;
    constexpr std::int64_t kMaxPeakBytes = 512ll << 20;
    // Still allocated after the release, on a second run (the first keeps one-time setup): counted from the menu's own
    // allocations (runtime/memory.h), so only the menu's other threads add a little noise. A leak repeats every run.
    constexpr std::int64_t kMaxRetainedBytes = 1ll << 20;

    Step& s = Begin("Mirage runtime");
    Probe p(s);
    const mirage::packages::Report pk = mirage::packages::Probe();
    if (!pk.ok) {
        p.Problem("UnrealLib: " + pk.error);
    } else {
        for (const auto& pkg : pk.packages)
            s.notes.push_back(pkg.name + ": " + std::to_string(pkg.exports) + " exports, " + std::to_string(pkg.classes) +
                              " classes, " + std::to_string(pkg.functions) + " functions");
        char line[160];
        std::snprintf(line, sizeof line, "UnrealLib: %zu classes, %zu properties in %.2f s, +%.0f MB loaded, +%.1f MB after",
                      pk.classesRead, pk.properties, pk.seconds, static_cast<double>(pk.peakBytes) / 1048576.0,
                      static_cast<double>(pk.retainedBytes) / 1048576.0);
        s.notes.push_back(line);
        if (pk.seconds > kMaxSeconds)
            p.Problem("reading the script packages took " + std::to_string(pk.seconds) + " s (budget " +
                      std::to_string(kMaxSeconds) + " s)");
        const mirage::packages::Report again = mirage::packages::Probe();
        std::snprintf(line, sizeof line, "second run: %.2f s, +%.0f MB loaded, +%.1f MB after", again.seconds,
                      static_cast<double>(again.peakBytes) / 1048576.0, static_cast<double>(again.retainedBytes) / 1048576.0);
        s.notes.push_back(line);
        const std::int64_t peak = std::max(pk.peakBytes, again.ok ? again.peakBytes : 0);
        if (peak > kMaxPeakBytes)
            p.Problem("reading the script packages used " + std::to_string(peak >> 20) + " MB (budget " +
                      std::to_string(kMaxPeakBytes >> 20) + " MB)");
        if (!again.ok) p.Problem("UnrealLib, second run: " + again.error);
        else if (again.retainedBytes > kMaxRetainedBytes)
            p.Problem("a second run left " + std::to_string(again.retainedBytes >> 10) +
                      " KB more allocated after UnrealLib released everything: a leak");
    }

    const mirage::lua::ProbeReport lua = mirage::lua::Probe();
    for (const std::string& problem : lua.problems) p.Problem("Lua: " + problem);
    if (lua.ok)
        s.notes.push_back(std::string(mirage::lua::Version()) + ": " + std::to_string(lua.notes.size()) + " checks passed");

    p.Check(1, true);       // The game kept drawing and ticking meanwhile
    End(s);
}

// Mirage step 2: the layouts UnrealLib computes from the script packages are the engine's own, for every class, struct,
// function and state of Core, Engine and TdGame. Every difference goes to mirage\mirage-layouts.txt.
// Mirage step 2: calls with the engine's own parameter layout, native functions with a native index included (the SDK's
// IsInState and SetLocation answered garbage: ProcessEvent refuses those, and its bool parameters were 1 byte).
void TestMirageCalls()
{
    Step& s = Begin("Mirage calls");
    Probe p(s);
    std::vector<std::string> problems;
    OnGameThread([&] {
        auto* pawn = game::Pawn();
        auto* pc = game::Controller();
        if (!pawn || !pc) { problems.push_back("no player"); return; }
        std::string why;
        if (!mirage::call::LayoutChecked(&why)) { problems.push_back("UFunction layout: " + why); return; }
        const auto call = [&](Classes::UObject* o, mirage::call::Params& params, const char* what) {
            std::string error;
            if (!params.Valid() || !mirage::call::Call(o, params, &error))
                problems.push_back(std::string(what) + ": " + (params.Valid() ? error : params.Error()));
        };

        // IsInState (native 281), against the state read from the state frame
        for (const char* state : {"PlayerWalking", "Dead"}) {
            mirage::call::Params is(pc, "IsInState");
            is.SetName("TestState", state);
            is.Set("bTestStateStack", false);
            call(pc, is, "IsInState");
            const bool want = game::IsInState(pc, state);
            if (is.GetBool("ReturnValue") != want)
                problems.push_back(std::string("IsInState(") + state + ") answered " + (want ? "false" : "true"));
        }

        // SetLocation (native 267): the player really moves
        const Classes::FVector before = pawn->Location;
        Classes::FVector target = before;
        target.Z += 64.f;
        mirage::call::Params at(pawn, "SetLocation");
        at.SetRaw("NewLocation", &target, sizeof target);
        call(pawn, at, "SetLocation");
        const float moved = pawn->Location.Z - before.Z;
        if (!at.GetBool("ReturnValue")) problems.push_back("SetLocation returned false");
        else if (std::fabs(moved - 64.f) > 1.f)
            problems.push_back("SetLocation returned true but the player moved " + std::to_string(moved) + " up, not 64");
        pawn->Location = before;

        // SetMove (script, two bools before a bool return value). It returns false for the move the player is already in
        // (`if (NewMove == MovementState) return false;`), true for a change: both answers must come back right.
        const auto setMove = [&](Classes::EMovement m) {
            mirage::call::Params move(pawn, "SetMove");
            move.Set("NewMove", std::uint8_t{static_cast<std::uint8_t>(m)});
            move.Set("bViaReplication", false);
            move.Set("bCheckCanDo", false);
            call(pawn, move, "SetMove");
            return move.GetBool("ReturnValue");
        };
        using Classes::EMovement;
        if (pawn->MovementState == EMovement::MOVE_Walking && setMove(EMovement::MOVE_Walking))
            problems.push_back("SetMove(Walking) while walking returned true (the script returns false)");
        if (!setMove(EMovement::MOVE_Crouch)) problems.push_back("SetMove(Crouch) returned false");
        else if (pawn->MovementState != EMovement::MOVE_Crouch) problems.push_back("SetMove(Crouch) did not crouch");
        if (!setMove(EMovement::MOVE_Walking)) problems.push_back("SetMove(Walking) after crouching returned false");
        else if (pawn->MovementState != EMovement::MOVE_Walking) problems.push_back("SetMove(Walking) did not walk");

        // Wrong types are refused, not written
        mirage::call::Params wrong(pc, "IsInState");
        if (wrong.Set("TestState", 1.f)) problems.push_back("a float was accepted for a name parameter");
        if (wrong.Set("NoSuchParameter", true)) problems.push_back("an unknown parameter was accepted");
        if (!problems.empty()) return;
        s.notes.push_back("IsInState, SetLocation (native) and SetMove (script) answered like the engine");
    }, 10.0);
    for (const auto& problem : problems) p.Problem(problem);
    WaitGrounded();
    p.Check(1, true);
    End(s);
}

void TestMirageLayouts()
{
    Step& s = Begin("Mirage layouts");
    Probe p(s);
    // Every package the SDK covers. The native-only ones have no .u file: Compute skips what it can't find
    const std::vector<std::string>& packages = mirage::layout::SdkPackages();
    std::vector<std::string> scripts;
    for (const auto& name : packages)
        if (std::filesystem::exists(mirage::packages::CookedDir() / (name + ".u"))) scripts.push_back(name);

    mirage::layout::Layouts live;
    const clk::Ticks t0 = clk::Now();
    if (!OnGameThread([&] { live = mirage::layout::Live(packages); }, 30.0)) p.Problem("reading the live layouts timed out");
    const double liveSeconds = clk::SecondsSince(t0);
    const clk::Ticks t1 = clk::Now();
    const mirage::layout::Computed computed = mirage::layout::Compute(mirage::packages::CookedDir(), scripts);
    const double computeSeconds = clk::SecondsSince(t1);
    if (!computed.error.empty()) p.Problem("UnrealLib: " + computed.error);

    const mirage::layout::Report r = mirage::layout::Compare(live, computed.layouts);
    char line[200];
    std::snprintf(line, sizeof line, "%zu live (%.2f s), %zu computed (%.2f s); %zu compared: %zu match; properties %zu of %zu",
                  live.size(), liveSeconds, computed.layouts.size(), computeSeconds, r.structs, r.structsMatching,
                  r.propertiesMatching, r.properties);
    s.notes.push_back(line);
    if (r.onlyLive || r.onlyComputed)
        s.notes.push_back(std::to_string(r.onlyLive) + " only in the engine, " + std::to_string(r.onlyComputed) +
                          " only computed");
    for (const auto& [cause, n] : r.byCause) s.notes.push_back("  " + std::to_string(n) + "x " + cause);

    // Everything into a file, to read the differences one by one
    const std::filesystem::path file = paths::Dir() / "mirage-layouts.txt";
    {
        std::ofstream out(file, std::ios::trunc);
        out << line << "\n\n";
        for (const auto& [cause, n] : r.byCause) out << n << "  " << cause << "\n";
        out << "\n";
        for (const auto& m : r.mismatches) out << m.path << ": " << m.what << "\n";
        out << "\nOnly in the engine:\n";
        for (const auto& path : r.onlyLivePaths) out << "  " << path << "\n";
        out << "Only computed:\n";
        for (const auto& path : r.onlyComputedPaths) out << "  " << path << "\n";
        // Both sides of the structs named in MIRAGE_LAYOUT_DETAIL (comma separated paths), property by property
        out << "\n";
        char* detail = nullptr;
        std::size_t len = 0;
        if (_dupenv_s(&detail, &len, "MIRAGE_LAYOUT_DETAIL") == 0 && detail) {
            std::string list = detail;
            std::free(detail);
            std::size_t start = 0;
            while (start <= list.size()) {
                const std::size_t comma = list.find(',', start);
                const std::string path = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                start = comma == std::string::npos ? list.size() + 1 : comma + 1;
                const mirage::layout::Layouts* const sides[] = {&live, &computed.layouts};
                for (const auto* side : sides) {
                    const auto it = side->find(path);
                    out << (side == &live ? "live     " : "computed ") << path;
                    if (it == side->end()) { out << ": none\n"; continue; }
                    char head[96];
                    std::snprintf(head, sizeof head, " (%s, size %#x, align %u, super %s)\n", it->second.kind.c_str(),
                                  it->second.size, it->second.alignment, it->second.super.c_str());
                    out << head;
                    for (const auto& prop : it->second.properties) {
                        char row[160];
                        std::snprintf(row, sizeof row, "    %#06x %-10s x%u size %-4u mask %#x  %s %s\n", prop.offset,
                                      mirage::layout::KindName(prop.kind), prop.arrayDim, prop.elementSize, prop.boolMask,
                                      prop.name.c_str(), prop.type.c_str());
                        out << row;
                    }
                }
            }
        }
    }
    // What only the engine knows (intrinsic classes, native-only packages): the SDK generator's second input
    {
        mirage::layout::Layouts onlyLive;
        for (const auto& [path, l] : live)
            if (!computed.layouts.count(path)) onlyLive.emplace(path, l);
        std::ofstream(paths::Dir() / "mirage-live-layouts.json", std::ios::trunc) << mirage::layout::ToJson(onlyLive);
        s.notes.push_back(std::to_string(onlyLive.size()) + " engine-only layouts saved to mirage-live-layouts.json");
    }
    // The generated SDK's checks the compiler can't make (each bool's bit in its dword; offsets and sizes are
    // static_asserts): its bitfields as MSVC packs them, against the masks the engine uses
    {
        std::string first;
        if (const int failures = Classes::detail::CheckLayouts(&first))
            p.Problem("the SDK has " + std::to_string(failures) + " bool bits where the engine doesn't (first: " +
                      first + ")");
        else
            s.notes.push_back("SDK bitfields match the engine");
    }
    if (!r.mismatches.empty())
        p.Problem(std::to_string(r.mismatches.size()) + " structs differ from the engine (first: " +
                  r.mismatches[0].path + ": " + r.mismatches[0].what + "); all in " + file.string());
    if (r.structs == 0) p.Problem("nothing was compared");

    p.Check(1, true);
    End(s);
}

void TestEngineConsole()
{
    Probe* p;
    std::unique_ptr<Probe> holder;
    Step& s = BeginEngine("Engine console", p, holder);
    Classes::UEngine* engine = nullptr;
    bool was = false, now = false, ran = false;
    OnGameThread([&] {
        const auto& objects = Classes::UObject::GetGlobalObjects();
        for (std::size_t i = 0; i < objects.Num() && !engine; ++i) {
            auto* o = objects.GetByIndex(i);
            if (o && game::IsA(o, Classes::UGameEngine::StaticClass()) && o->GetName().rfind("Default__", 0) != 0)
                engine = static_cast<Classes::UEngine*>(o);
        }
        if (!engine) return;
        was = engine->bEnableColorClear != 0;
        ran = game::ConsoleCommand(std::string("set Engine bEnableColorClear ") + (was ? "false" : "true"));
    });
    WaitTicks(2);
    if (engine) OnGameThread([&] { now = engine->bEnableColorClear != 0; });
    if (!engine) p->Problem("no engine object");
    else if (!ran) p->Problem("ConsoleCommand had no controller to run it");
    else if (now == was) p->Problem("\"set Engine bEnableColorClear\" did not change the engine");
    else s.notes.push_back("set changed bEnableColorClear");
    if (engine) OnGameThread([&] { game::ConsoleCommand(std::string("set Engine bEnableColorClear ") + (was ? "true" : "false")); });
    WaitTicks(2);
    p->Check(1, false);
    End(s);
}

std::atomic<int> g_deathBefore{0}, g_deathAfter{0};

// Kills the player: the death hooks must see it, and the game must bring the player back (checkpoint restart).
void TestEngineDeath()
{
    Probe* p;
    std::unique_ptr<Probe> holder;
    Step& s = BeginEngine("Engine death hook", p, holder);
    static bool added = false;
    if (!added) {
        added = true;
        game::OnPlayerDeath([](bool after) { ++(after ? g_deathAfter : g_deathBefore); });
    }
    if (!game::DeathHooked()) {
        p->Problem("the death functions are not hooked");
        p->Check(1, false);
        End(s);
        return;
    }
    if (Module* god = modules::Find("God Mode")) OnRenderThread([god] { god->SetEnabled(false); });
    WaitTicks(2);
    const int before = g_deathBefore, after = g_deathAfter;
    const void* pawnBefore = nullptr;
    std::string nameBefore;     // Objects are named uniquely (TdPlayerPawn_N): the new pawn may get the old one's memory
    OnGameThread([&] {
        auto* pawn = game::Pawn();
        auto* pc = game::Controller();
        if (!pawn || !pc) return;
        pawnBefore = pawn;
        nameBefore = pawn->GetName();
        Classes::FTraceHitInfo hit{};
        pawn->TakeDamage(pawn->Health + 10000, nullptr, pawn->Location, Classes::FVector{},
                         Classes::UTdDmgType_Fell::StaticClass(), hit, nullptr);
    });
    if (!WaitFor(10.0, [&] { return g_deathBefore > before; })) {
        std::string state = "no player";
        OnGameThread([&] {
            if (auto* pawn = game::Pawn())
                state = "health " + std::to_string(pawn->Health) + ", physics " + std::to_string(static_cast<int>(pawn->Physics.GetValue())) +
                        ", move " + std::to_string(static_cast<int>(pawn->MovementState.GetValue())) +
                        (pawn == pawnBefore ? ", same pawn" : ", another pawn");
        }, 2.0);
        p->Problem("no death seen by the hook within 10 s (" + state + ")");
    }
    else {
        if (!WaitFor(10.0, [&] { return g_deathAfter > after; })) p->Problem("the after-death call never came");
        // Back in the game: a new pawn, alive and movable
        const bool back = WaitFor(60.0, [&] {
            bool ok = false;
            OnGameThread([&] { auto* pw = game::Pawn(); ok = pw && pw->GetName() != nameBefore && pw->Health > 0; }, 1.0);
            return ok;
        });
        // Walking again, with no cutscene or input hold. Not PlayerNotReady(): the engine leaves the controller's
        // bFrozen (set while dead; it only gates "fire to respawn") after the game's own checkpoint restart.
        bool walking = false, frozen = false;
        if (back)
            walking = WaitFor(30.0, [&] {
                OnGameThread([&] {
                    auto* pc = game::Controller();
                    walking = pc && game::Pawn() && game::IsInState(pc, "PlayerWalking") &&
                              !pc->bCinematicMode && !pc->bIgnoreMoveInput;
                    frozen = pc && pc->bFrozen;
                }, 1.0);
                return walking;
            });
        if (!back) p->Problem("the player did not respawn within 60 s");
        else if (!walking) p->Problem("after respawning, the player is not back in PlayerWalking");
        else s.notes.push_back(std::string("death seen before and after, player respawned and walking") +
                               (frozen ? " (bFrozen left set by the engine)" : ""));
    }
    p->Check(1, false);
    End(s);
}

// The game's TdInput.ini (SetBind saves it): kept as it was before the test and written back after.
std::filesystem::path TdInputIni()
{
    wchar_t docs[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, docs))) return {};
    return std::filesystem::path(docs) / L"EA Games" / L"Mirror's Edge" / L"TdGame" / L"Config" / L"TdInput.ini";
}

// Collision View is a native first-party mod (mods/mirage-collision): what it sees comes from what it shares (JSON).
nlohmann::json CollisionShared(const char* key)
{
    std::string v(1 << 16, '\0');
    const uint32_t n = mirage::mods::game_api::ShareGet("mirage-collision", key, v.data(), static_cast<uint32_t>(v.size()));
    v.resize(std::min<std::size_t>(n, v.size() - 1));
    const auto j = nlohmann::json::parse(v, nullptr, false);
    return j.is_discarded() ? nlohmann::json::object() : j;
}

// Turns the player's view toward a point (what the panel's Nearby click does)
void FaceTowards(float x, float y, float z)
{
    OnGameThread([x, y, z] {
        auto* pc = game::Controller();
        const rmath::Camera cam = game::GetCamera();
        if (!pc || !cam.valid) return;
        const float dx = x - cam.pos.x, dy = y - cam.pos.y, dz = z - cam.pos.z;
        const float flat = std::sqrt(dx * dx + dy * dy);
        constexpr float kUnits = 65536.f / (2.f * 3.14159265f);
        pc->Rotation.Yaw = static_cast<int>(std::atan2(dy, dx) * kUnits) & 0xFFFF;
        pc->Rotation.Pitch = static_cast<int>(std::atan2(dz, flat) * kUnits) & 0xFFFF;
    });
}

// Whether a mod is loaded (false: switched off in the mod manager, left out, or not installed)
bool ModLoaded(const std::string& id)
{
    for (const auto& m : mirage::mods::List())
        if (m.id == id) return m.loaded;
    return false;
}

void TestCollisionInspect()
{
    Step& s = Begin("Collision view inspect");
    Probe p(s);
    Module* view = modules::Find("Collision View");
    auto* inspect = view ? dynamic_cast<ctl::Checkbox*>(view->FindSetting("Inspect")) : nullptr;
    if (!view || !inspect) {
        if (!ModLoaded("mirage-collision")) s.notes.push_back("left out: mirage-collision isn't loaded (switched off in the mod manager?)");
        else p.Problem("the Collision View module or its Inspect section was not found");
        End(s);
        return;
    }
    auto* labels = dynamic_cast<ctl::Checkbox*>(view->FindSetting("Labels"));     // Drawn meanwhile (on screen)
    const bool labelsWas = labels && labels->Get();
    const bool enabledWas = view->Enabled(), expandedWas = view->expanded, inspectWas = inspect->Get();
    unsigned yawWas = 0, pitchWas = 0;
    OnGameThread([&] { if (auto* pc = game::Controller()) { yawWas = pc->Rotation.Yaw; pitchWas = pc->Rotation.Pitch; } });
    game::SetCameraTracking(true);
    OnRenderThread([&] {
        view->expanded = true;
        inspect->Set(true);
        if (labels) labels->Set(true);
        if (!view->Enabled()) view->SetEnabled(true);
    });

    nlohmann::json nearby = nlohmann::json::array();
    WaitFor(10.0, [&] { nearby = CollisionShared("inspect").value("nearby", nlohmann::json::array()); return !nearby.empty(); });
    if (nearby.empty()) p.Problem("no volumes listed near the player");
    int tried = 0, matched = 0;
    for (const auto& n : nearby) {
        if (tried >= 5 || g_aborted) break;
        const std::string name = n.value("name", std::string());
        const float distance = n.value("distance", 0.f);
        if (distance <= 0.f || name.empty()) continue;      // Inside it: the view ray starts in it
        ++tried;
        FaceTowards(n.value("x", 0.f), n.value("y", 0.f), n.value("z", 0.f));
        WaitTicks(45);          // The mod shares twice a second
        const auto look = CollisionShared("inspect").value("look", nlohmann::json::object());
        const bool valid = look.value("valid", false);
        const std::string lookName = look.value("name", std::string());
        bool behindIt = false;
        std::string behind;
        for (const auto& b : look.value("behind", nlohmann::json::array())) {
            const std::string bn = b.value("name", std::string());
            behindIt |= valid && bn == name;
            behind += " " + bn + "@" + std::to_string(static_cast<int>(b.value("at", 0.f)));
        }
        const bool front = valid && lookName == name;
        const bool ok = front || behindIt;
        matched += ok ? 1 : 0;
        const std::string what = front ? "looked at it" : behindIt ? "on the line behind " + lookName
                                 : valid ? "looked at " + lookName : "looked at nothing";
        char note[200];
        std::snprintf(note, sizeof note, "faced %s (center %.0f away): %s, hit %.0f away", name.c_str(), distance,
                      what.c_str(), look.value("distance", 0.f));
        s.notes.push_back(note);
        if (!behind.empty()) s.notes.push_back("  also on the line:" + behind);
        if (ok) break;
    }
    s.notes.push_back(std::to_string(nearby.size()) + " volumes listed nearby");
    if (!nearby.empty() && tried == 0) p.Problem("the player is inside every volume listed nearby: nothing to face");
    if (tried > 0 && matched == 0) p.Problem("facing a nearby volume never made the look-at report it");

    OnGameThread([&] { if (auto* pc = game::Controller()) { pc->Rotation.Yaw = yawWas; pc->Rotation.Pitch = pitchWas; } });
    OnRenderThread([&] {
        if (labels) labels->Set(labelsWas);
        inspect->Set(inspectWas);
        view->expanded = expandedWas;
        view->SetEnabled(enabledWas);
    });
    WaitTicks(5);
    p.Check(1, true);
    End(s);
}

// Static geometry in the collision view: the level is read (a slice per tick), real collision comes out of it (not only
// bounding boxes), something is drawn near the player, and the game thread never pays for it in one go.
void TestCollisionStatic()
{
    Step& s = Begin("Collision view static geometry");
    Probe p(s);
    Module* view = modules::Find("Collision View");
    auto* on = view ? dynamic_cast<ctl::Checkbox*>(view->FindSetting("Show static geometry")) : nullptr;
    auto* other = view ? dynamic_cast<ctl::Checkbox*>(view->FindSetting("Static: other")) : nullptr;
    if (!view || !on || !other) {
        if (!ModLoaded("mirage-collision")) s.notes.push_back("left out: mirage-collision isn't loaded (switched off in the mod manager?)");
        else p.Problem("the Collision View module or its static geometry settings were not found");
        End(s);
        return;
    }
    const bool enabledWas = view->Enabled(), onWas = on->Get(), otherWas = other->Get();
    OnRenderThread([&] {
        on->Set(true);
        other->Set(true);
        if (!view->Enabled()) view->SetEnabled(true);
    });

    nlohmann::json info;
    const bool read = WaitFor(30.0, [&] {
        info = CollisionShared("static");
        return !info.value("map", std::string()).empty() && !info.value("scanning", true) && info.value("meshes", 0) > 0;
    });
    WaitFor(5.0, [&] { info = CollisionShared("static"); return info.value("vertices", 0) > 0; });
    WaitTicks(120);                 // The steady state: the level read, the drawing kept up to date
    info = CollisionShared("static");
    std::size_t found = 0, drawn = 0;
    std::string per;
    const auto foundList = info.value("found", nlohmann::json::array()), drawnList = info.value("drawn", nlohmann::json::array());
    for (std::size_t c = 0; c < foundList.size() && c < drawnList.size(); ++c) {
        found += foundList[c].get<std::size_t>();
        drawn += drawnList[c].get<std::size_t>();
        per += std::string(per.empty() ? "" : ", ") + std::to_string(drawnList[c].get<std::size_t>()) + "/" +
               std::to_string(foundList[c].get<std::size_t>());
    }
    const double tickMax = info.value("tick_max_ms", 0.0), tickAvg = info.value("tick_ms", 0.0);
    char note[240];
    std::snprintf(note, sizeof note, "%s: %d meshes with simple collision, %zu placed; drawn/found per kind: %s",
                  info.value("map", std::string()).c_str(), info.value("meshes", 0), found, per.c_str());
    s.notes.push_back(note);
    std::snprintf(note, sizeof note, "%d k vertices built in %.1f ms (worker); game thread %.3f ms a tick on average, "
                  "slowest %.1f ms%s", info.value("vertices", 0) / 1000, info.value("build_ms", 0.0), tickAvg, tickMax,
                  info.value("budget_hit", false) ? "; vertex limit hit" : "");
    s.notes.push_back(note);
    if (!read) p.Problem("the level was never read");
    else if (info.value("meshes", 0) == 0) p.Problem("no mesh had simple collision we could read (only bounding boxes)");
    if (read && drawn == 0) p.Problem("nothing drawn near the player");
    if (tickMax > 4.0) p.Problem("a game thread tick took " + std::to_string(tickMax) + " ms");
    if (tickAvg > 0.5) p.Problem("the game thread spends " + std::to_string(tickAvg) + " ms a tick on it");

    OnRenderThread([&] {
        on->Set(onWas);
        other->Set(otherWas);
        view->SetEnabled(enabledWas);
    });
    WaitTicks(5);
    p.Check(1, true);
    End(s);
}

// The speedrun timer (mods/memenu-speedrun, a native first-party mod) against the game, through what it shares
// (memenu-speedrun/run.*) and the commands the menu hands it (mirage/speedrun.command):
//   1. the game's level completed event (the auto split) is hooked
//   2. reloading the level starts an individual level run (auto start)
//   3. a second reload while it runs (auto reset off) is not counted: real time grows, load removed time doesn't
//   4. a manual split finishes it (one segment) and sets a personal best; undo takes it back
// The level's record file is put back the way it was.
namespace speedrun_test
{
    std::string Get(const char* key)
    {
        char v[256]{};
        mirage::mods::game_api::ShareGet("memenu-speedrun", key, v, sizeof v);
        return v;
    }
    double Num(const char* key) { return std::atof(Get(key).c_str()); }
    void Command(const char* what)
    {
        static int n = 0;
        mirage::mods::Share("speedrun.command", std::to_string(++n) + " " + what);
    }
}

#endif

// Setting values of every module, to put back after the test
struct Saved {
    bool enabled;
    int key;
    json settings;
    std::vector<std::pair<int, int>> keys;
};

std::map<Module*, Saved> SaveAll()
{
    std::map<Module*, Saved> out;
    modules::ForEach([&](modules::Tab&, Module& m) {
        Saved sv{m.Enabled(), m.key, json::object(), {}};
        for (ctl::Setting* st : m.Settings()) {
            sv.keys.emplace_back(st->key, st->keyAlt);
            if (!st->Persistent()) continue;
            json v;
            st->Save(v);
            sv.settings[st->Name()] = v;
        }
        out.emplace(&m, std::move(sv));
    });
    return out;
}

void RestoreAll(const std::map<Module*, Saved>& saved)
{
    OnRenderThread([&] {
        for (const auto& [m, sv] : saved) {
            std::size_t i = 0;
            for (ctl::Setting* st : m->Settings()) {
                if (i < sv.keys.size()) { st->key = sv.keys[i].first; st->keyAlt = sv.keys[i].second; }
                ++i;
                if (st->Persistent() && sv.settings.contains(st->Name())) st->Load(sv.settings[st->Name()]);
            }
            m->key = sv.key;
            if (m->Enabled() != sv.enabled) m->SetEnabled(sv.enabled);
            m->expanded = false;
        }
    }, 10.0);
}

bool Selected(const Module& m)
{
    // The developer tools themselves: switching them would close the Test window (or developer mode) under the run
    if (m.Name() == devmode::kModuleName || m.Name() == "Test") return false;
    if (!g_options.only.empty() && !g_options.only.count(m.Name())) return false;
    return !g_options.skip.count(m.Name());
}

const sched::EntryView* FindEntry(const std::vector<sched::EntryView>& entries, const std::string& name)
{
    for (const auto& e : entries) if (e.name == name) return &e;
    return nullptr;
}

// Modules that draw on the screen every frame while on (a HUD, a counter): with the menu closed, the frame must have more
// in it while they are on than before, and no more after they are switched off.
const std::set<std::string> kDrawsHud = {"Crosshair", "Show FPS", "Watermark", "Speedometer", "Input Overlay", "Kickglitch Helper",
                                         "Speedrun Timer"};

// What the menu draws with its window closed (the modules' HUDs), a few frames after a change.
unsigned HudVertices()
{
    hooks::DescribeDrawLists(true);
    OnRenderThread([] { gui::visible = false; });
    // Until the count settles: in fast mode a few ticks can pass before the next frame is drawn, and the frame drawn
    // just before a module switched off still holds what it drew (a HUD that changes every frame, like the speedometer's
    // digits, never settles: then the last count)
    WaitTicks(3);
    unsigned last = hooks::MenuVertices();
    for (int i = 0; i < 20; ++i) {
        WaitTicks(3);
        const unsigned now = hooks::MenuVertices();
        if (now == last) break;
        last = now;
    }
    return last;
}

#if MIRAGE_GAME
// Mirage step 4: the example mods (examples/mods: hello built by its own CMake project with clang-cl, hello-lua), put in
// selftest-mods by `mirage game-test`, are loaded; their modules are in the menu, run, draw, save and load their
// settings like built-in ones, and keep their own config.
void TestMirageMods()
{
    Step& s = Begin("Mirage mods");
    Probe p(s);
    const auto mods = mirage::mods::List();
    for (const char* id : {"hello", "hello-lua"}) {
        const auto it = std::find_if(mods.begin(), mods.end(), [&](const auto& m) { return m.id == id; });
        if (it == mods.end()) {
            p.Problem(std::string(id) + " was not found (is selftest-mods there? mirage game-test puts it)");
            continue;
        }
        if (!it->loaded) p.Problem(std::string(id) + " was not loaded: " + it->problem);
        else s.notes.push_back(it->id + " " + it->version + " (" + it->kind + "): " + std::to_string(it->modules.size()) + " module(s)");
        for (const auto& w : it->warnings) p.Problem(std::string(id) + ": " + w);
    }

    for (const char* name : {"Hello (native)", "Hello (Lua)"}) {
        Module* m = modules::Find(name);
        if (!m) {
            p.Problem(std::string(name) + " is not in the menu");
            continue;
        }
        if (m->Tab() != "Mods") p.Problem(std::string(name) + " is in the " + m->Tab() + " column, not Mods");
        auto* greeting = dynamic_cast<ctl::Value<std::string>*>(m->FindSetting("Greeting"));
        auto* height = dynamic_cast<ctl::Value<float>*>(m->FindSetting("Height"));
        auto* ticks = dynamic_cast<ctl::Value<int>*>(m->FindSetting("Ticks"));
        auto* status = dynamic_cast<ctl::Value<std::string>*>(m->FindSetting("Status"));
        if (!greeting || !height || !ticks || !status) {
            p.Problem(std::string(name) + ": its settings are missing or of the wrong type");
            continue;
        }
        // Greeting, Height and Color come from mod.toml ([settings.*]): first in the panel, in the file's order
        auto* color = dynamic_cast<ctl::Combo*>(m->FindSetting("Color"));
        if (!color) p.Problem(std::string(name) + ": the choice setting Color from mod.toml is missing");
        else if (m->Settings().size() < 3 || m->Settings()[0]->Name() != "Greeting" || m->Settings()[2]->Name() != "Color")
            p.Problem(std::string(name) + ": the mod.toml settings aren't first, in the file's order");

        // Settings save and load like the menu's own (the config writes each with Save, reads it with Load)
        {
            nlohmann::ordered_json saved;
            const std::string before = greeting->Get();
            greeting->Set("saved greeting");
            greeting->Save(saved["Greeting"]);
            height->Set(0.5f);
            height->Save(saved["Height"]);
            greeting->Set("changed");
            height->Set(0.1f);
            greeting->Load(saved["Greeting"]);
            height->Load(saved["Height"]);
            if (greeting->Get() != "saved greeting" || std::fabs(height->Get() - 0.5f) > 1e-6f)
                p.Problem(std::string(name) + ": settings didn't load back what was saved");
            height->Set(2.0f);
            if (height->Get() > 0.95f) p.Problem(std::string(name) + ": Height isn't clamped to its range");
            greeting->Set(before);
            height->Set(0.3f);
        }

        // On: on_enable, on_tick (Ticks counts) and on_render (it draws); off: on_disable
        const int ticksBefore = ticks->Get();
        OnRenderThread([] { gui::visible = false; });
        WaitTicks(3);
        const unsigned drawnBefore = hooks::MenuVertices();
        OnRenderThread([m] { m->SetEnabled(true); });
        WaitTicks(10);
        const unsigned drawnOn = hooks::MenuVertices();
        if (!m->Enabled()) p.Problem(std::string(name) + " didn't switch on");
        if (status->Get().rfind("on (", 0) != 0) p.Problem(std::string(name) + ": on_enable didn't run (Status '" + status->Get() + "')");
        if (ticks->Get() <= ticksBefore) p.Problem(std::string(name) + ": on_tick didn't run");
        if (drawnOn <= drawnBefore) p.Problem(std::string(name) + ": on_render drew nothing");
        OnRenderThread([m] { m->SetEnabled(false); });
        OnRenderThread([] { gui::visible = true; });
        WaitTicks(3);
        if (status->Get() != "off") p.Problem(std::string(name) + ": on_disable didn't run");
        s.notes.push_back(std::string(name) + ": " + std::to_string(ticks->Get() - ticksBefore) + " ticks, drew " +
                          std::to_string(drawnOn - drawnBefore) + " vertices, " + status->Get());
    }

    // Each mod's own config (config.toml in its folder, written by mirage.config.set / mirage::SetConfig)
    for (const char* id : {"hello", "hello-lua"}) {
        const auto file = paths::Dir() / "selftest-mods" / id / "config.toml";
        std::ifstream in(file);
        std::stringstream text;
        text << in.rdbuf();
        if (text.str().find("times_enabled") == std::string::npos)
            p.Problem(std::string(id) + ": config.toml has no times_enabled");
    }
    p.Check(1, true);
    End(s);
}

// Mirage step 6: UnrealScript function hooks (runtime/fhooks.h). The offsets they need are found (UFunction::Func,
// the frame's Locals); a script function called through ProcessEvent (CheckMaxEffectDistance, whose answer depends on
// its parameters) runs hooks in priority order, sees and changes its parameters, can be replaced; a script function
// called from script every tick (UpdateMomentumStats) is caught too; removing the hooks puts the function back.
void TestMirageFunctionHooks()
{
    Step& s = Begin("Mirage function hooks");
    Probe p(s);
    namespace mh = mirage::hooks;
    struct Seen
    {
        std::vector<std::string> order;
        float cull = -1.f, z = 0.f;
        bool after = false, afterResult = false;
        int momentumCalls = 0;
        float momentumDelta = 0.f;
        Classes::UObject* momentumObject = nullptr;
    } seen;
    const char* kCheck = "Engine.Actor.CheckMaxEffectDistance";
    const auto checkOnce = [&](bool& answer, std::string& error) {
        auto* pc = game::Controller();
        auto* pawn = game::Pawn();
        if (!pc || !pawn) { error = "no player"; return false; }
        mirage::call::Params params(pawn, "CheckMaxEffectDistance");
        params.Set("P", static_cast<Classes::UObject*>(pc));
        Classes::FVector at = pc->ViewTarget ? pc->ViewTarget->Location : pawn->Location;
        params.SetRaw("SpawnLocation", &at, sizeof at);
        params.Set("CullDistance", 0.f);
        if (!mirage::call::Call(pawn, params, &error)) return false;
        answer = params.GetBool("ReturnValue");
        return true;
    };

    std::string why;
    // First-party mods keep hooks of their own (Speedrun Timer, Keep Key Bindings, the kick tools): only ours count
    const auto before = mh::GetStats();
    std::vector<int> ids;
    OnGameThread([&] {
        if (!mh::Ready(&why)) return;
        std::string error;
        // Two before hooks: priority 20 first. The second moves the spawn point far away and sets a cull distance, so
        // the function answers false where it would answer true
        ids.push_back(mh::Add(kCheck, mh::When::Before, 10, [](void* u, mh::Call& c) {
            auto& sn = *static_cast<Seen*>(u);
            sn.order.push_back("low");
            sn.cull = mirage::reflect::GetFloat(c.Param("CullDistance"));
            const auto z = c.Param("SpawnLocation.Z");
            mirage::reflect::SetFloat(z, mirage::reflect::GetFloat(z) + 100000.f);
            sn.z = mirage::reflect::GetFloat(z);
            mirage::reflect::SetFloat(c.Param("CullDistance"), 1.f);
        }, &seen, &error));
        ids.push_back(mh::Add(kCheck, mh::When::Before, 20, [](void* u, mh::Call&) { static_cast<Seen*>(u)->order.push_back("high"); },
                              &seen, &error));
        ids.push_back(mh::Add(kCheck, mh::When::After, 0, [](void* u, mh::Call& c) {
            auto& sn = *static_cast<Seen*>(u);
            sn.after = true;
            sn.afterResult = mirage::reflect::GetInt(c.Param("ReturnValue")) != 0;
        }, &seen, &error));
        ids.push_back(mh::Add("TdGame.TdPlayerController.UpdateMomentumStats", mh::When::Before, 0, [](void* u, mh::Call& c) {
            auto& sn = *static_cast<Seen*>(u);
            ++sn.momentumCalls;
            sn.momentumDelta = mirage::reflect::GetFloat(c.Param("DeltaTime"));
            sn.momentumObject = c.object;
        }, &seen, &error));
        if (!error.empty()) why = error;
    });
    if (!why.empty()) {
        p.Problem("hooks: " + why);
    } else {
        bool plain = false, hooked = true;
        std::string error;
        // Hooked: the before hooks change its parameters, so it answers false
        OnGameThread([&] { checkOnce(hooked, error); });
        if (!error.empty()) p.Problem("CheckMaxEffectDistance: " + error);
        if (seen.order != std::vector<std::string>{"high", "low"}) p.Problem("before hooks didn't run high priority first");
        if (seen.cull != 0.f) p.Problem("the hook read CullDistance " + std::to_string(seen.cull) + ", not 0");
        if (!seen.after) p.Problem("the after hook didn't run");
        if (hooked) p.Problem("changing its parameters didn't change its answer (true with the point 100000 away)");
        if (seen.afterResult != hooked) p.Problem("the after hook saw another return value than the caller");

        // Replace: it answers what the hook says, its own code doesn't run
        int replaceId = 0;
        OnGameThread([&] {
            replaceId = mh::Add(kCheck, mh::When::Replace, 30, [](void*, mh::Call& c) {
                mirage::reflect::SetInt(c.Param("ReturnValue"), 1);
            }, nullptr, &error);
        });
        bool replaced = false;
        OnGameThread([&] { checkOnce(replaced, error); });
        if (!replaced) p.Problem("the replace hook's return value didn't reach the caller");

        // Called from script every tick
        WaitTicks(5);
        if (seen.momentumCalls == 0) p.Problem("UpdateMomentumStats (called from script) never reached its hook");
        else if (seen.momentumDelta <= 0.f || seen.momentumObject != game::Controller())
            p.Problem("UpdateMomentumStats's hook saw DeltaTime " + std::to_string(seen.momentumDelta) + " on the wrong object");

        // Removed: the function as it was
        OnGameThread([&] {
            mh::Remove(replaceId);
            for (int id : ids) mh::Remove(id);
        });
        seen.order.clear();
        OnGameThread([&] { checkOnce(plain, error); });
        if (!seen.order.empty()) p.Problem("hooks still ran after they were removed");
        if (!plain) p.Problem("unhooked, CheckMaxEffectDistance at the view target answered false");
        const auto st = mh::GetStats();
        if (st.functions != before.functions || st.hooks != before.hooks)
            p.Problem(std::to_string(st.hooks - before.hooks) + " hooks left after removing them");
        s.notes.push_back(std::to_string(st.calls) + " hooked calls, " +
                          std::to_string(st.calls ? st.microseconds / static_cast<double>(st.calls) : 0.0) +
                          " us each around the original (native hooks); UpdateMomentumStats seen " +
                          std::to_string(seen.momentumCalls) + " times");
    }
    p.Check(1, true);
    End(s);
}

// Mirage step 6's milestone, with the self-test's own mods: hook-native (C++, priority 20) and hook-lua (priority 10)
// hook the same function while their modules are on, and run in priority order (each turns CullDistance into
// x * 10 + its number: 1 then 12); they read game objects by name and get events; reloading them keeps that up and
// switching them off leaves no hook behind. Measured: what an unhooked script call pays for the detour (its lookup),
// and what a native and a Lua hook add to a call.
// The mods' own tests (test_register; mirage.test in Lua, mirage::Test in C++, Mod.Test in C#): each runs on the game
// thread with the level loaded and its mod's modules on. `mirage mod test <folder>` runs only that mod's (option
// modtests=<id>); a full run runs every installed mod's.
void TestModTests()
{
    Step& s = Begin("Mod tests");
    Probe p(s);
    namespace mm = mirage::mods;
    // The mods under test (`mirage mod test`) have their modules on: a test checks what the mod does. A full run
    // switches nothing (the self-test's own mods include ones that crash or change the game on purpose): their tests
    // run with the modules as the other steps left them.
    std::vector<Module*> switched;
    for (const auto& info : mm::List()) {
        if (!info.loaded || g_options.modTests.empty()) continue;
        if (std::find(g_options.modTests.begin(), g_options.modTests.end(), info.id) == g_options.modTests.end())
            continue;
        for (const std::string& name : info.modules)
            if (Module* m = modules::Find(name); m && !m->Enabled()) {
                OnRenderThread([m] { m->SetEnabled(true); });
                switched.push_back(m);
            }
    }
    WaitTicks(5);
    std::vector<mm::TestResult> results;
    OnGameThread([&] { results = mm::RunTests(g_options.modTests); }, 120.0);
    for (const auto& r : results) {
        s.notes.push_back(fmt::format("{} {}: {} ({:.1f} ms){}", r.ok ? "PASS" : "FAIL", r.mod, r.name, r.ms,
                                      r.ok ? std::string() : " - " + r.message));
        if (!r.ok) p.Problem(fmt::format("{}: {}: {}", r.mod, r.name, r.message));
    }
    if (results.empty()) {
        std::string names;
        for (const auto& id : g_options.modTests) names += (names.empty() ? "" : ", ") + id;
        s.notes.push_back(names.empty() ? std::string("no mod has tests")
                                        : "no tests from " + names + " (does it call test_register / mirage.test / Mod.Test?)");
    }
    if (!g_options.modTests.empty() && results.empty()) p.Problem("the mod under test registered no tests");
    for (Module* m : switched) OnRenderThread([m] { m->SetEnabled(false); });
    p.Check(1, true);
    End(s);
}

// A checkpoint restart, waited out: the restart runs on a later tick, so the pawn there now is the old one until a new
// one (another ObjectId: a new pawn can take the old one's memory) is in and ready. False if none came.
bool RestartAndWait(double timeout = 30.0)
{
    game::ObjectId before{};
    OnGameThread([&] {
        before = game::IdOf(game::Pawn());
        levels::RestartFromLastCheckpoint();
    });
    const bool came = WaitFor(timeout, [&] {
        bool fresh = false;
        OnGameThread([&] {
            const auto* now = game::Pawn();
            fresh = now && !game::IsLoading() && !(game::IdOf(now) == before);
        }, 1.0);
        return fresh;
    });
    WaitPlayerReady(5.0);		// Frozen a while after a Prologue restart: what's checked next doesn't need it moving
    return came;
}

// Step 5 of the next ten steps (the plan's step 18), the read side and its investigation: the loaded level's Kismet as
// the engine holds it (runtime/kismet.h). Every link leads to an op the read found; the read is timed (the target is
// under 20 ms for a level's whole graph); what cooking kept (the editor's titles) and whether a checkpoint restart keeps
// the same sequence objects (it decides whether a runtime edit survives one) are noted.
void TestKismetRead()
{
    Step& s = Begin("Mirage Kismet (read)");
    Probe p(s);
    namespace ki = mirage::kismet;
    std::vector<ki::Sequence> seqs;
    double ms = 0.0;
    OnGameThread([&] {
        const auto start = clk::Now();
        seqs = ki::Read();
        ms = clk::Seconds(clk::Now() - start) * 1000.0;
    }, 10.0);
    if (seqs.empty()) {
        p.Problem("no sequence was read in the loaded level");
        End(s);
        return;
    }
    std::set<std::string> ops;
    int kinds[6] = {}, links = 0, titled = 0, touch = 0, roots = 0;
    for (const auto& q : seqs) {
        roots += q.parent.empty();
        for (const auto& op : q.ops) {
            ops.insert(op.path);
            ++kinds[static_cast<int>(op.kind)];
            titled += !op.title.empty();
            touch += op.cls == "SeqEvent_Touch";
            for (const auto& o : op.outputs) links += static_cast<int>(o.links.size());
        }
    }
    int dangling = 0;
    std::string firstDangling;
    for (const auto& q : seqs)
        for (const auto& op : q.ops)
            for (const auto& o : op.outputs)
                for (const auto& l : o.links)
                    if (!ops.count(l.op)) {
                        if (!dangling++) firstDangling = op.path + " -> " + l.op;
                    }
    if (dangling) p.Problem(fmt::format("{} link(s) lead to ops the read didn't find (the first: {})", dangling, firstDangling));
    s.notes.push_back(fmt::format("{} sequence(s) ({} root) in {:.2f} ms: {} events, {} actions, {} conditions, {} variables, "
                                  "{} nested sequences; {} output links; {} of {} objects keep the editor's title; {} touch events",
                                  seqs.size(), roots, ms, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4], links, titled,
                                  ops.size(), touch));
#ifdef NDEBUG
    constexpr double kReadBudgetMs = 20.0;      // The plan's target, for a release build
#else
    constexpr double kReadBudgetMs = 200.0;     // Debug runs everything several times slower
#endif
    if (ms > kReadBudgetMs) p.Problem(fmt::format("reading the graph took {:.1f} ms (budget {:.0f})", ms, kReadBudgetMs));
    for (const auto& q : seqs)
        for (const auto& op : q.ops)
            if (op.cls == "SeqEvent_Touch" && !op.outputs.empty() && !op.outputs[0].links.empty()) {
                s.notes.push_back("e.g. " + op.name + " (originator " + op.originator + ") -> " + op.outputs[0].links[0].op);
                goto sampled;
            }
sampled:
    // The package file read offline (UnrealLib, on this thread): the same ops and the same links as the engine holds
    {
        const auto start = clk::Now();
        std::string why;
        const std::vector<ki::Op> file = ki::ReadPackage("Edge_p", &why);
        const double fileSeconds = clk::SecondsSince(start);
        const auto lower = [](std::string x) {
            std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return x;
        };
        const auto linksOf = [&](const ki::Op& op) {
            std::vector<std::string> v;
            for (std::size_t o = 0; o < op.outputs.size(); ++o)
                for (const auto& l : op.outputs[o].links) v.push_back(fmt::format("{}>{}[{}]", o, lower(l.op), l.input));
            std::sort(v.begin(), v.end());
            return v;
        };
        // Edge_p's own objects in memory: its sequences (the root one is no other's op) and their ops, without the
        // streamed sublevels' root sequences, which the engine nests into the level's root at load
        std::map<std::string, const ki::Op*> inMemory;
        std::vector<ki::Op> rootOps;
        rootOps.reserve(seqs.size());
        int nestedAtLoad = 0;
        for (const auto& q : seqs)
            if (q.level == "Edge_p") {
                if (q.parent.empty()) {
                    ki::Op root;
                    root.path = q.path;
                    root.cls = "Sequence";
                    rootOps.push_back(std::move(root));
                    inMemory[lower(q.path)] = &rootOps.back();
                }
                for (const auto& op : q.ops) {
                    if (!lower(op.path).starts_with("edge_p.")) {
                        ++nestedAtLoad;
                        continue;
                    }
                    inMemory[lower(op.path)] = &op;
                }
            }
        int same = 0, onlyFile = 0, linksDiffer = 0, fileLinks = 0;
        std::string firstMissing, firstDiff;
        std::set<std::string> seen;
        for (const auto& op : file) {
            const std::string key = lower(op.path);
            seen.insert(key);
            const auto l = linksOf(op);
            fileLinks += static_cast<int>(l.size());
            const auto it = inMemory.find(key);
            if (it == inMemory.end()) {
                if (!onlyFile++) firstMissing = op.path;
                spdlog::info("Self-test: Kismet only in the package file: {} ({})", op.path, op.cls);
                continue;
            }
            ++same;
            if (linksOf(*it->second) != l && !linksDiffer++) firstDiff = op.path;
        }
        int onlyMemory = 0;
        for (const auto& [key, op] : inMemory)
            if (!seen.count(key)) {
                ++onlyMemory;
                spdlog::info("Self-test: Kismet only in memory: {} ({})", op->path, op->cls);
            }
        s.notes.push_back(fmt::format("the package file (UnrealLib, {:.1f} s): {} objects, {} links; {} the same in memory, "
                                      "{} only in the file{}, {} only in memory, {} with other links{}", fileSeconds,
                                      file.size(), fileLinks, same, onlyFile, onlyFile ? " (" + firstMissing + ")" : "",
                                      onlyMemory, linksDiffer, linksDiffer ? " (" + firstDiff + ")" : ""));
        s.notes.push_back(fmt::format("{} streamed sublevels' root sequences are nested in the level's root at load", nestedAtLoad));
        if (file.empty()) p.Problem("UnrealLib read nothing from Edge_p: " + why);
        else if (onlyFile || onlyMemory || linksDiffer) p.Problem("the package file and the engine's graph differ");
    }
    // A checkpoint restart: the same sequence objects (an edit survives it) or new ones (an edit is applied again)?
    const auto rootOf = [](const std::vector<ki::Sequence>& v) {
        for (const auto& q : v)
            if (q.parent.empty()) return q.path + " " + std::to_string(q.ops.size());
        return std::string();
    };
    const std::string before = rootOf(seqs);
    void* rootBefore = nullptr;
    OnGameThread([&] { rootBefore = game::FindObject<Classes::UObject>(("Sequence " + seqs.front().path).c_str()); });
    if (!RestartAndWait()) p.Problem("no new player came after the checkpoint restart");
    std::vector<ki::Sequence> again;
    void* rootAfter = nullptr;
    OnGameThread([&] {
        again = ki::Read();
        rootAfter = game::FindObject<Classes::UObject>(("Sequence " + seqs.front().path).c_str());
    });
    if (!rootBefore) p.Problem("the root sequence wasn't found by its name (object_find) before the restart");
    s.notes.push_back(fmt::format("after a checkpoint restart: {} sequence(s), root \"{}\" -> \"{}\", the same object: {}",
                                  again.size(), before, rootOf(again), rootBefore && rootBefore == rootAfter ? "yes" : "no"));
    p.Check(1, true);
    End(s);
}

// Step 5 of the next ten steps, the plan's Scenario C: tests/mods/kismet-edit takes over a Prologue touch event (its
// links out removed, one added to a new op of the mod's own SequenceAction class, its Activated in Lua). The player
// walks into the event's trigger and the Lua runs; a checkpoint restart keeps the edit (the same sequence objects); a
// hot reload puts the graph back and the reloaded mod edits it again (a new op, the old one gone); the module off
// (mirage.revert) leaves the graph exactly as it was read before.
std::string KismetEditShared(const char* key)
{
    char v[512]{};
    mirage::mods::game_api::ShareGet("kismet-edit", key, v, sizeof v);
    return v;
}

// The graph's structure as text (each sequence's ops and every link, sorted): what an edit changes and a revert puts back
std::string KismetGraph(const std::vector<mirage::kismet::Sequence>& seqs)
{
    std::vector<std::string> lines;
    for (const auto& q : seqs)
        for (const auto& op : q.ops) {
            lines.push_back(q.path + " has " + op.path + " (" + op.cls + ")");
            for (std::size_t o = 0; o < op.outputs.size(); ++o)
                for (const auto& l : op.outputs[o].links)
                    lines.push_back(fmt::format("{}[{}] -> {}[{}]", op.path, o, l.op, l.input));
        }
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (const auto& l : lines) out += l + "\n";
    return out;
}

// Where `event`'s Touched output leads now ("" if nowhere; "+" between several)
std::string KismetTouchedTargets(const std::vector<mirage::kismet::Sequence>& seqs, const std::string& event, std::string* cls)
{
    std::map<std::string, std::string> classes;
    for (const auto& q : seqs)
        for (const auto& op : q.ops) classes[op.path] = op.cls;
    for (const auto& q : seqs)
        for (const auto& op : q.ops)
            if (op.path == event && !op.outputs.empty()) {
                std::string out;
                for (const auto& l : op.outputs[0].links) {
                    out += (out.empty() ? "" : "+") + l.op;
                    if (cls) *cls = classes[l.op];
                }
                return out;
            }
    return {};
}

int KismetCount(const std::vector<mirage::kismet::Sequence>& seqs, const std::string& cls)
{
    int n = 0;
    for (const auto& q : seqs)
        for (const auto& op : q.ops) n += op.cls == cls;
    return n;
}

void TestKismetEdit()
{
    Step& s = Begin("Mirage Kismet (edit)");
    Probe p(s);
    namespace ki = mirage::kismet;
    Module* m = modules::Find("Kismet edit (Lua)");
    if (!m) {
        p.Problem("the Kismet edit (Lua) module is missing (tests/mods/kismet-edit)");
        End(s);
        return;
    }
    const auto read = [] {
        std::vector<ki::Sequence> v;
        OnGameThread([&] { v = ki::Read(); }, 10.0);
        return v;
    };
    // A player to walk in (frozen is fine: after the read step's checkpoint restart the Prologue holds it a while)
    WaitFor(15.0, [] {
        bool there = false;
        OnGameThread([&] { there = game::Pawn() && !game::IsLoading(); }, 1.0);
        return there;
    });
    const std::string original = KismetGraph(read());
    const auto firstDifference = [&](const std::string& now) {
        std::istringstream a(original), b(now);
        std::string x, y;
        while (true) {
            const bool ha = static_cast<bool>(std::getline(a, x)), hb = static_cast<bool>(std::getline(b, y));
            if (!ha && !hb) return std::string();
            if (x != y || ha != hb) return "was \"" + (ha ? x : "") + "\", now \"" + (hb ? y : "") + "\"";
        }
    };
    const auto finish = [&] {
        OnRenderThread([m] { m->SetEnabled(false); });
        WaitTicks(5);
        End(s);
    };

    OnRenderThread([m] { m->SetEnabled(true); });
    WaitFor(20.0, [] { return !KismetEditShared("applied").empty() || !KismetEditShared("problem").empty(); });
    if (const std::string why = KismetEditShared("problem"); !why.empty() || KismetEditShared("applied").empty()) {
        p.Problem("the mod didn't edit the graph: " + (why.empty() ? std::string("it never said so") : why));
        finish();
        return;
    }
    const std::string event = KismetEditShared("event");
    std::string cls;
    const std::string ping = KismetTouchedTargets(read(), event, &cls);
    s.notes.push_back(fmt::format("{}: {} link(s) out removed, Touched -> {} ({})", event, KismetEditShared("unlinked"), ping, cls));
    if (ping.empty() || ping.find('+') != std::string::npos || cls != "MirageSeqAct_Ping") {
        p.Problem("the event's Touched doesn't lead to one MirageSeqAct_Ping op");
        finish();
        return;
    }

    // Walking in: the player put at the event's trigger, nudged until it touches
    // (Out of it first: a player already inside the trigger, at a checkpoint in it, gets no new touch moving into it)
    int triggerCount = 0;
    Classes::FVector home{}, target{};
    std::string state = "no event, trigger or player";
    OnGameThread([&] {
        auto* ev = game::FindObject<Classes::USequenceEvent>(("SeqEvent_Touch " + event).c_str());
        auto* pawn = game::Pawn();
        if (!ev || !ev->Originator || !pawn) {
            state = fmt::format("event {}, its trigger {}, the player {}", ev ? "found" : "not found",
                                ev && ev->Originator ? "found" : "not found", pawn ? "found" : "not found");
            return;
        }
        triggerCount = ev->TriggerCount;
        home = pawn->Location;
        target = ev->Originator->Location;
        Classes::FVector above = target;
        above.Z += 600.f;
        const bool out = pawn->SetLocation(above);
        state = fmt::format("{} ({}), enabled {}, fired {}/{}, retrigger delay {:.1f}, lifted out: {}", ev->Originator->GetName(),
                            ev->Originator->Class->GetName(), ev->bEnabled ? 1 : 0, ev->TriggerCount, ev->MaxTriggerCount,
                            ev->ReTriggerDelay, out ? "yes" : "no");
    });
    WaitTicks(2);
    OnGameThread([&] {
        auto* pawn = game::Pawn();
        if (!pawn) return;
        const bool in = pawn->SetLocation(target);
        const float dx = pawn->Location.X - target.X, dy = pawn->Location.Y - target.Y, dz = pawn->Location.Z - target.Z;
        state += fmt::format(", put in: {} ({:.0f} units from its center)", in ? "yes" : "no", std::sqrt(dx * dx + dy * dy + dz * dz));
    });
    s.notes.push_back("the trigger: " + state);
    int pings = 0;
    const clk::Ticks walkStart = clk::Now();
    for (int i = 0; pings == 0 && clk::SecondsSince(walkStart) < 6.0; ++i) {
        WaitTicks(1);
        pings = static_cast<int>(util::ParseFloatOr(KismetEditShared("pings").c_str(), 0.f));
        if (pings == 0 && i % 4 == 3)
            OnGameThread([&] {
                if (auto* pawn = game::Pawn()) {
                    Classes::FVector at = pawn->Location;
                    at.Z += (i % 8 == 3) ? -1.f : 1.f;
                    pawn->SetLocation(at);
                }
            });
    }
    int opPings = -1, activations = -1, fired = -1;
    OnGameThread([&] {
        if (auto* op = game::FindObject<Classes::UObject>(("MirageSeqAct_Ping " + ping).c_str())) {
            opPings = mirage::reflect::GetInt(mirage::reflect::Resolve(op->Class, op, "Pings"));
            activations = static_cast<Classes::USequenceOp*>(op)->ActivateCount;
        }
        if (auto* ev = game::FindObject<Classes::USequenceEvent>(("SeqEvent_Touch " + event).c_str())) fired = ev->TriggerCount - triggerCount;
        // The event as it was: walking in again must fire it (the reloaded mod picks the same one)
        if (auto* ev = game::FindObject<Classes::USequenceEvent>(("SeqEvent_Touch " + event).c_str())) ev->TriggerCount = triggerCount;
        if (auto* pawn = game::Pawn()) pawn->SetLocation(home);
    });
    s.notes.push_back(fmt::format("walking in: the event fired {} time(s), the op activated {} time(s), the Lua Activated ran {} "
                                  "time(s), the op's Pings is {}", fired, activations, pings, opPings));
    if (pings == 0) p.Problem("walking into the trigger didn't run the mod's Activated");
    else if (opPings != pings) p.Problem(fmt::format("the op's Pings is {}, the mod counted {}", opPings, pings));

    // A checkpoint restart: the same sequence objects, so the edit stays
    if (!RestartAndWait()) p.Problem("no new player came after the checkpoint restart");
    const std::string afterRestart = KismetTouchedTargets(read(), event, nullptr);
    s.notes.push_back("after a checkpoint restart, Touched -> " + afterRestart);
    if (afterRestart != ping) p.Problem("a checkpoint restart lost the edit (Touched -> " + afterRestart + ")");

    // A hot reload: the unload puts the graph back, the reloaded mod edits it again with a new op
    std::string reload = "not run";
    OnGameThread([&] { reload = mirage::mods::Reload("kismet-edit"); }, 10.0);
    if (!reload.empty()) p.Problem("the hot reload failed: " + reload);
    std::string again, againCls;
    int pingOps = 0;
    WaitFor(20.0, [&] {
        const auto now = read();
        again = KismetTouchedTargets(now, event, &againCls);
        pingOps = KismetCount(now, "MirageSeqAct_Ping");
        return !again.empty() && again != ping && againCls == "MirageSeqAct_Ping";
    });
    s.notes.push_back(fmt::format("after a hot reload, Touched -> {} ({} Ping op(s) in the level)", again, pingOps));
    if (again == ping || againCls != "MirageSeqAct_Ping") p.Problem("the reloaded mod's edit isn't there (Touched -> " + again + ")");
    if (pingOps != 1) p.Problem(fmt::format("{} Ping ops in the level after the reload: the unload didn't take the old one out", pingOps));

    // Off: mirage.revert() puts everything back
    OnRenderThread([m] { m->SetEnabled(false); });
    std::string diff;
    WaitFor(10.0, [&] {
        diff = firstDifference(KismetGraph(read()));
        return diff.empty();
    });
    s.notes.push_back("module off: reverted " + KismetEditShared("reverted") + (diff.empty() ? ", the graph is as it was" : ""));
    if (!diff.empty()) p.Problem("the graph isn't as it was after the revert: " + diff);
    p.Check(1, true);
    End(s);
}

// Step 5's data side, firing and the trace: tests/mods/kismet-layer (a content mod) gets a Kismet layer written while
// the game runs, from names read in the Prologue: a new SeqAct_Log op, a touch event's link moved to it, the event's
// properties set, and one entry naming nothing (skipped and listed). Applied within a couple of seconds (the file
// changed); kismet_fire fires the event twice (the op activates twice) and the trace sees both; the file removed, the
// layer is undone and the graph is as it was.
void TestKismetLayer()
{
    Step& s = Begin("Mirage Kismet (layer)");
    Probe p(s);
    namespace ki = mirage::kismet;
    namespace kl = mirage::kismet_layers;
    std::filesystem::path dir;
    for (const auto& m : mirage::mods::List())
        if (m.id == "kismet-layer" && m.loaded) dir = m.dir;
    if (dir.empty()) {
        p.Problem("the kismet-layer mod isn't loaded (tests/mods/kismet-layer)");
        End(s);
        return;
    }
    const std::filesystem::path file = dir / "kismet" / "edge_p.toml";
    std::error_code ec;
    std::filesystem::remove(file, ec);      // One left by a run that stopped halfway
    const auto entries = [] {
        std::vector<kl::Entry> v;
        OnGameThread([&] {
            for (auto& e : kl::List())
                if (e.mod == "kismet-layer") v.push_back(e);
        });
        return v;
    };
    WaitFor(5.0, [&] { return entries().empty(); });
    std::vector<ki::Sequence> seqs;
    OnGameThread([&] { seqs = ki::Read(); }, 10.0);
    const std::string original = KismetGraph(seqs);

    // A touch event with a link out and a name only it has, and where that link goes
    std::map<std::string, int> names;       // In Edge_p itself: a layer's name means the map's own level's op first
    for (const auto& q : seqs)
        if (q.level == "Edge_p")
            for (const auto& op : q.ops) ++names[op.name];
    const ki::Op* event = nullptr;
    const ki::Sequence* home = nullptr;
    for (const auto& q : seqs)
        for (const auto& op : q.ops)
            if (!event && q.level == "Edge_p" && op.cls == "SeqEvent_Touch" && names[op.name] == 1 && !op.outputs.empty() && !op.outputs[0].links.empty()) {
                event = &op;
                home = &q;
            }
    if (!event) {
        p.Problem("no touch event with a link and a unique name in the Prologue");
        End(s);
        return;
    }
    const ki::Link moved = event->outputs[0].links[0];
    const std::string target = moved.op.substr(moved.op.rfind('.') + 1);
    // The target by the end of its path when its name repeats (the layer's way to say which)
    std::string targetRef = target;
    if (names[target] > 1) {
        const std::string parent = moved.op.substr(0, moved.op.rfind('.'));
        targetRef = parent.substr(parent.rfind('.') + 1) + "." + target;
    }
    {
        std::filesystem::create_directories(file.parent_path(), ec);
        std::ofstream out(file, std::ios::binary);
        out << "# Written by the self-test (Mirage Kismet (layer)); removed when it ends\n"
            << "[[op]]\nname = \"Log\"\nclass = \"Engine.SeqAct_Log\"\nsequence = \"" << home->name << "\"\n"
            << "[op.properties]\nbOutputToScreen = false\n\n"
            << "[[unlink]]\nfrom = \"" << event->name << "\"\noutput = 0\nto = \"" << targetRef << "\"\ninput = " << moved.input << "\n\n"
            << "[[link]]\nfrom = \"" << event->name << "\"\noutput = \"" << event->outputs[0].desc << "\"\nto = \"Log\"\n\n"
            << "[[link]]\nfrom = \"NoSuchOpAnywhere\"\nto = \"Log\"\n\n"
            << "[[set]]\nop = \"" << event->name << "\"\n[set.properties]\nMaxTriggerCount = 0\nReTriggerDelay = 0.0\n";
    }
    std::vector<kl::Entry> applied;
    WaitFor(10.0, [&] { applied = entries(); return !applied.empty(); });
    int ok = 0, skipped = 0;
    std::string skip;
    for (const auto& e : applied) {
        if (e.problem.empty()) ++ok;
        else {
            ++skipped;
            skip = e.what + ": " + e.problem;
        }
    }
    s.notes.push_back(fmt::format("{} (in {}): {} entries applied, {} skipped ({})", event->name, home->name, ok, skipped, skip));
    if (ok != 4 || skipped != 1 || skip.find("NoSuchOpAnywhere") == std::string::npos)
        p.Problem(fmt::format("{} entries applied and {} skipped, not 4 and 1 (the missing op)", ok, skipped));
    std::string cls;
    std::vector<ki::Sequence> now;
    OnGameThread([&] { now = ki::Read(); }, 10.0);
    const std::string to = KismetTouchedTargets(now, event->path, &cls);
    s.notes.push_back("Touched -> " + to + " (" + cls + ")");
    if (cls != "SeqAct_Log" || to.find('+') != std::string::npos) p.Problem("the event's first output doesn't lead to the layer's op alone");

    // Fired twice from code; the trace sees the event and the op each time
    int fired = 0, activations = -1, triggerCount = 0;
    double traceUs = 0.0;
    int traced = 0, tracedLog = 0;
    std::string diag;
    OnGameThread([&] { mirage::kismet_trace::Enable("selftest", true); });
    WaitTicks(2);
    for (int i = 0; i < 2; ++i) {
        OnGameThread([&] {
            auto* ev = game::FindObject<Classes::UObject>(("SeqEvent_Touch " + event->path).c_str());
            if (!i && ev) triggerCount = static_cast<Classes::USequenceEvent*>(ev)->TriggerCount;
            const int before = ev ? static_cast<Classes::USequenceEvent*>(ev)->TriggerCount : 0;
            fired += mirage::mods::game_api::KismetFire(nullptr, reinterpret_cast<MirageObject*>(ev), nullptr, nullptr);
            if (auto* e = static_cast<Classes::USequenceEvent*>(ev)) {
                int queued = 0;
                if (auto* seq = e->ParentSequence)
                    for (std::size_t k = 0; k < seq->ActiveSequenceOps.Num(); ++k) queued += seq->ActiveSequenceOps[k] == e;
                auto* log = game::FindObject<Classes::USequenceOp>(("SeqAct_Log " + to).c_str());
                diag += fmt::format("(fire {}: TriggerCount +{}, queued {}, output impulse {}, event active {}, Log impulse {}, "
                                    "sequence enabled {}, its active ops {}) ", i, e->TriggerCount - before, queued,
                                    e->OutputLinks.Num() ? e->OutputLinks[0].bHasImpulse : 9, e->bActive ? 1 : 0,
                                    log && log->InputLinks.Num() ? log->InputLinks[0].bHasImpulse : 9,
                                    e->ParentSequence ? e->ParentSequence->bEnabled : 9,
                                    e->ParentSequence ? e->ParentSequence->ActiveSequenceOps.Num() : 0);
            }
        });
        WaitTicks(3);
    }
    OnGameThread([&] {
        if (auto* op = game::FindObject<Classes::USequenceOp>(("SeqAct_Log " + to).c_str())) activations = op->ActivateCount;
        if (auto* ev = game::FindObject<Classes::USequenceEvent>(("SeqEvent_Touch " + event->path).c_str())) ev->TriggerCount = triggerCount;
        traceUs = mirage::kismet_trace::Get().lastTickUs;
        std::string text(mirage::kismet_trace::Read("selftest", nullptr, 0) + 1, '\0');
        text.resize(mirage::kismet_trace::Read("selftest", text.data(), static_cast<std::uint32_t>(text.size())));
        for (const auto& h : nlohmann::json::parse(text, nullptr, false)) {
            if (!h.is_object()) continue;
            if (h.value("op", "") == event->path) traced += h.value("times", 0);
            if (h.value("op", "") == to) tracedLog += h.value("times", 0);
        }
        mirage::kismet_trace::Enable("selftest", false);
    });
    const auto stats = mirage::kismet_trace::Get();
    s.notes.push_back(fmt::format("fired {} of 2; the op activated {} time(s); the trace saw the event {} and the op {} time(s) "
                                  "({} ops watched, {:.1f} us a tick)", fired, activations, traced, tracedLog, stats.ops, traceUs));
    if (activations != 2) s.notes.push_back(diag);
    if (fired != 2) p.Problem(fmt::format("kismet_fire activated the event {} of 2 times", fired));
    if (activations != 2) p.Problem(fmt::format("the layer's op activated {} times, not 2", activations));
    if (traced != 2 || tracedLog != 2) p.Problem(fmt::format("the trace saw the event {} and the op {} times, not 2 each", traced, tracedLog));
#ifdef NDEBUG
    if (traceUs > 200.0) p.Problem(fmt::format("the trace took {:.0f} us of a tick", traceUs));
#endif

    // The file gone: the layer undone
    std::filesystem::remove(file, ec);
    std::string diff = "not compared";
    WaitFor(10.0, [&] {
        std::vector<ki::Sequence> v;
        OnGameThread([&] { v = ki::Read(); }, 10.0);
        const std::string g = KismetGraph(v);
        diff = g == original ? "" : "differs";
        return diff.empty() && entries().empty();
    });
    if (!diff.empty()) p.Problem("the graph isn't as it was after the layer's file was removed");
    else s.notes.push_back("the file removed: undone, the graph is as it was");
    p.Check(1, true);
    End(s);
}

// Step 6 of the next ten steps, the plan's Scenario E: tests/mods/physics-lua spawns a prop with a mesh this step picks
// (a KActor's, else a small one with collision from the level), lets it fall, pushes it at 1x and 4x mass, turns its
// gravity off, changes its collision, and finds it with an overlap and a trace. This step checks what the mod shares,
// times 500 traces in one tick (the plan's target: under 1 ms), and checks the revert takes the prop away.
// A static mesh for a physics prop, shared as "selftest" "mesh" for the test mods (physics-lua, triggers-lua): a
// KActor's (made to be a prop), else the smallest-but-not-tiny one with collision in the level. "" if none.
std::string PropMesh()
{
    std::string mesh;
    OnGameThread([&] {
        float best = 1e9f;
        auto& objects = Classes::UObject::GetGlobalObjects();
        for (std::size_t i = 0; i < objects.Num(); ++i) {
            auto* o = objects.GetByIndex(i);
            if (!o || !o->IsA(Classes::UStaticMeshComponent::StaticClass())) continue;
            auto* c = static_cast<Classes::UStaticMeshComponent*>(o);
            if (!c->StaticMesh || !c->StaticMesh->BodySetup || !c->Owner || !game::IsA(c->Owner, Classes::AActor::StaticClass())) continue;
            const bool kactor = c->Owner->IsA(Classes::AKActor::StaticClass());
            const float r = c->Bounds.SphereRadius;
            const float score = kactor ? 0.f : (r >= 20.f && r <= 80.f ? r : 1e8f);
            if (score < best) {
                best = score;
                mesh = c->StaticMesh->GetPathName();
            }
        }
        if (best >= 1e8f) mesh.clear();
    });
    static MirageMod self;      // The self-test's shares
    self.id = "selftest";
    mirage::mods::game_api::ShareSet(&self, "mesh", mesh.c_str());
    return mesh;
}

std::string PhysicsShared(const char* key)
{
    char v[512]{};
    mirage::mods::game_api::ShareGet("physics-lua", key, v, sizeof v);
    return v;
}

void TestPhysics()
{
    Step& s = Begin("Mirage physics");
    Probe p(s);
    Module* m = modules::Find("Physics (Lua)");
    if (!m) {
        p.Problem("the Physics (Lua) module is missing (tests/mods/physics-lua)");
        End(s);
        return;
    }
    if (!mirage::physics::TraceAvailable()) p.Problem("UWorld::SingleLineCheck wasn't found (runtime/symbols.toml)");
    // From where the level started (a checkpoint restart, then back to that spot: earlier steps may have reached a
    // later checkpoint): the mod finds room for its props around the player there
    if (!RestartAndWait()) p.Problem("no new player came after the checkpoint restart");
    if (g_haveLevelStart) {
        OnGameThread([] {
            if (auto* pawn = game::Pawn()) {
                pawn->SetLocation(g_levelStart);
                pawn->Velocity = Classes::FVector{};
            }
        });
        WaitGrounded();
    }

    const std::string mesh = PropMesh();
    if (mesh.empty()) {
        p.Problem("no static mesh with collision for a prop in the level");
        End(s);
        return;
    }
    p.Expect("a live body's mass can't be changed");     // The mod checks that the refusal is clean
    p.Expect("over the engine's cap");                   // ... and asks for more than the cap

    int before = -1;
    OnGameThread([&] { before = static_cast<int>(game::ActorsOf(Classes::AKActorSpawnable::StaticClass()).size()); });
    OnRenderThread([m] { m->SetEnabled(true); });
    WaitFor(25.0, [] { return !PhysicsShared("done").empty() || !PhysicsShared("problem").empty(); });
    const auto num = [](const char* key) { return util::ParseFloatOr(PhysicsShared(key).c_str(), -1.f); };
    const auto yes = [](const char* key) { return PhysicsShared(key) == "true"; };
    if (const std::string why = PhysicsShared("problem"); !why.empty())
        p.Problem("the mod: " + why + (PhysicsShared("spot_why").empty() ? "" : " (" + PhysicsShared("spot_why") + ")"));
    else if (PhysicsShared("done").empty()) p.Problem("the mod didn't finish (it shared: spawned '" + PhysicsShared("spawned") + "')");
    s.notes.push_back(fmt::format("prop of {}: simulated {}, dropped {:.0f} in {:.2f} s", mesh, PhysicsShared("simulated"),
                                  num("dropped"), num("fall_time")));
    s.notes.push_back("where: " + PhysicsShared("where"));
    if (!yes("simulated")) p.Problem("the prop isn't simulated (PHYS_RigidBody with a body)");
    if (num("dropped") < 50.f) p.Problem(fmt::format("the prop dropped {:.0f} units: it didn't fall", num("dropped")));

    // Mass: the same impulse in the air, props spawned at other masses at the same spot: speed goes as 1 / mass
    const float v1 = num("speed_1x"), vLight = num("speed_0.25"), v2 = num("speed_2"), v4 = num("speed_4");
    s.notes.push_back(fmt::format("on the floor an impulse gives {:.0f} u/s; in the air with gravity off (drift {:.1f} in 0.5 s) the "
                                  "same impulse: {:.0f} u/s at 1x mass, {:.0f} at 0.25x, {:.0f} at 2x, {:.0f} at 4x (effective mass "
                                  "x{:.2f}, x{:.2f}, x{:.2f}; 4x reads back {}); a live body's mass change refused, the body kept: {}",
                                  num("speed_floor"), num("drift"), v1, vLight, v2, v4, vLight > 0 ? v1 / vLight : 0.f,
                                  v2 > 0 ? v1 / v2 : 0.f, v4 > 0 ? v1 / v4 : 0.f, PhysicsShared("mass_4"), PhysicsShared("live_mass_refused")));
    if (std::fabs(num("drift")) > 30.f) p.Problem(fmt::format("with gravity_scale 0 the prop drifted {:.0f} units in 0.5 s", num("drift")));
    if (PhysicsShared("live_mass_refused") != "true") p.Problem("set_body with a mass scale wasn't refused cleanly");
    if (v1 <= 10.f) p.Problem("the impulse didn't move the prop");
    else {
        if (vLight / v1 < 3.f || vLight / v1 > 5.f) p.Problem(fmt::format("at 0.25x the mass the speed is {:.0f}, not about 4x {:.0f}", vLight, v1));
        if (v2 <= 0.f || v1 / v2 < 1.6f || v1 / v2 > 2.5f) p.Problem(fmt::format("at 2x the mass the speed is {:.0f}, not about half of {:.0f}", v2, v1));
        // 4x: capped by the engine (MassScale 100); it reads back as the capped scale and moves as that mass
        const float capped = num("mass_4");
        if (capped <= 0.f || capped > 4.f) p.Problem("the 4x prop's mass_scale reads " + PhysicsShared("mass_4"));
        else if (v4 <= 0.f || v1 / v4 < capped * 0.8f || v1 / v4 > capped * 1.25f)
            p.Problem(fmt::format("at 4x asked ({:.2f}x given) the speed is {:.0f}, not {:.0f}", capped, v4, v1 / capped));
    }

    // Collision, overlap, trace
    s.notes.push_back(fmt::format("block_actors {} -> {}; overlap finds it: {}; a trace with actors hits it: {} ({}); without: hits the world: {}",
                                  PhysicsShared("block_before"), PhysicsShared("block_after"), PhysicsShared("overlap"),
                                  PhysicsShared("trace_hit_prop"), PhysicsShared("trace_hit_what"), PhysicsShared("trace_world")));
    if (PhysicsShared("block_after") != "false") p.Problem("set_collision didn't take block_actors away");
    if (!yes("overlap")) p.Problem("the overlap around the prop didn't find it");
    if (!yes("trace_hit_prop")) p.Problem("a downward trace with actors didn't hit the prop");

    // Joints: each prop hangs about 100 below its anchor (without the joint it drops ~250 to the floor)
    s.notes.push_back(fmt::format("joints: ball {} holds its prop {:.0f} from the anchor (dropped {:.0f}), hinge {} {:.0f} (dropped {:.0f}); "
                                  "the floor under the player: material {}, physical material {}", PhysicsShared("joint_ball"),
                                  num("hang_ball"), num("drop_ball"), PhysicsShared("joint_hinge"), num("hang_hinge"), num("drop_hinge"),
                                  PhysicsShared("floor_material"), PhysicsShared("floor_physmat")));
    for (const char* kind : {"ball", "hinge"}) {
        const float hang = num((std::string("hang_") + kind).c_str());
        if (hang < 50.f || hang > 150.f) p.Problem(fmt::format("the {} joint's prop is {:.0f} from its anchor, not about 100", kind, hang));
    }
    if (PhysicsShared("floor_physmat") == "none") p.Problem("a material trace onto the floor reported no physical material");

    // 500 traces in one tick: lines down from a ring around the player
    double ms = -1.0;
    int hits = 0;
    OnGameThread([&] {
        auto* pawn = game::Pawn();
        if (!pawn) return;
        const Classes::FVector at = pawn->Location;
        const auto start = clk::Now();
        for (int i = 0; i < 500; ++i) {
            const float a = static_cast<float>(i) * 0.0125664f, r = 100.f + static_cast<float>(i % 50) * 10.f;
            MirageHit h{};
            h.size = sizeof h;
            const float from[3] = {at.X + std::cos(a) * r, at.Y + std::sin(a) * r, at.Z + 200.f};
            const float to[3] = {from[0], from[1], at.Z - 1000.f};
            hits += mirage::mods::game_api::WorldTrace(nullptr, from, to, nullptr, MIRAGE_TRACE_ACTORS, nullptr, &h) == 1;
        }
        ms = clk::Seconds(clk::Now() - start) * 1000.0;
    }, 10.0);
    s.notes.push_back(fmt::format("500 line traces in {:.2f} ms ({} hit)", ms, hits));
    if (ms < 0) p.Problem("the traces didn't run");
    else if (hits == 0) p.Problem("none of 500 traces down from around the player hit anything");
#ifdef NDEBUG
    if (ms > 1.0) p.Problem(fmt::format("500 traces took {:.2f} ms (target under 1 ms)", ms));
#endif

    // Off: the revert takes the prop away
    OnRenderThread([m] { m->SetEnabled(false); });
    WaitFor(10.0, [] { return !PhysicsShared("reverted").empty(); });
    WaitTicks(5);
    int left = -1;
    OnGameThread([&] { left = static_cast<int>(game::ActorsOf(Classes::AKActorSpawnable::StaticClass()).size()); });
    s.notes.push_back(fmt::format("module off: reverted {}, KActorSpawnables in play {} (before the mod: {})", PhysicsShared("reverted"), left, before));
    if (left != before) p.Problem("the prop is still in play after the revert");
    p.Check(1, true);
    End(s);
}

// Step 7 of the next ten steps, the plan's Scenario F (its trigger half): tests/mods/triggers-lua puts a pawn trigger (with
// stay) and a KActor-only trigger at one spot in front of the player. The player is put inside: enter (the player), stay
// every tick, the mod's logic pushing its prop; the KActor trigger stays quiet; out again: leave. A box shape is refused.
// The module off: the revert takes the triggers away.
std::string TriggersShared(const char* key)
{
    char v[256]{};
    mirage::mods::game_api::ShareGet("triggers-lua", key, v, sizeof v);
    return v;
}

void TestTriggers()
{
    Step& s = Begin("Mirage triggers");
    Probe p(s);
    p.Expect("only cylinders");     // The mod asks for a box on purpose
    Module* m = modules::Find("Triggers (Lua)");
    if (!m) {
        p.Problem("the Triggers (Lua) module is missing (tests/mods/triggers-lua)");
        End(s);
        return;
    }
    if (g_haveLevelStart) {         // Room in front of the player: where the level started
        OnGameThread([] {
            if (auto* pawn = game::Pawn()) {
                pawn->SetLocation(g_levelStart);
                pawn->Velocity = Classes::FVector{};
            }
        });
        WaitGrounded();
    }
    const auto num = [](const char* key) { return static_cast<int>(util::ParseFloatOr(TriggersShared(key).c_str(), 0.f)); };
    PropMesh();                     // The prop the mod pushes on entering
    int before = -1;
    OnGameThread([&] { before = static_cast<int>(game::ActorsOf(Classes::ATrigger::StaticClass()).size()); });
    OnRenderThread([m] { m->SetEnabled(true); });
    WaitFor(10.0, [] { return !TriggersShared("made").empty(); });
    if (TriggersShared("made") != "2") {
        p.Problem("the mod made " + TriggersShared("made") + " of its 2 triggers (the log says why)");
        OnRenderThread([m] { m->SetEnabled(false); });
        End(s);
        return;
    }
    if (TriggersShared("box_refused") != "true") p.Problem("a box trigger wasn't refused");
    float sx = 0, sy = 0, sz = 0;
    sscanf_s(TriggersShared("spot").c_str(), "%f %f %f", &sx, &sy, &sz);

    // In: the player stands in the trigger
    Classes::FVector home{};
    OnGameThread([&] {
        if (auto* pawn = game::Pawn()) {
            home = pawn->Location;
            pawn->SetLocation(Classes::FVector{sx, sy, sz});
        }
    });
    // A teleport isn't always a touch: small moves until the engine sees it (as the Kismet edit step does)
    for (int i = 0; i < 120 && num("enter") < 1; ++i) {
        WaitTicks(1);
        if (i % 4 == 3)
            OnGameThread([&] {
                if (auto* pawn = game::Pawn()) {
                    Classes::FVector at = pawn->Location;
                    at.Z += (i % 8 == 3) ? -1.f : 1.f;
                    pawn->SetLocation(at);
                }
            });
    }
    const int stayStart = num("stay");
    WaitTicks(20);
    const int enter = num("enter"), stayIn = num("stay") - stayStart, inside = num("inside"), launched = num("launched");
    // Out again
    OnGameThread([&] {
        if (auto* pawn = game::Pawn()) pawn->SetLocation(home);
    });
    WaitFor(5.0, [&] { return num("leave") >= 1; });
    WaitTicks(10);
    const int leave = num("leave"), stayAfter = num("stay") - stayStart, filtered = num("filtered");
    s.notes.push_back(fmt::format("trigger {} at {:.0f} {:.0f} {:.0f}: entered {} ({}), {} stay calls in 20 ticks, {} inside, left {}; "
                                  "the prop launched {} time(s); the KActor-only trigger fired {} time(s); stay after leaving: {} more",
                                  TriggersShared("trigger"), sx, sy, sz, enter, TriggersShared("who"), stayIn, inside, leave, launched,
                                  filtered, stayAfter - stayIn));
    if (enter != 1) p.Problem(fmt::format("entering the trigger was reported {} times, not once", enter));
    if (stayIn < 10) p.Problem(fmt::format("{} stay calls in 20 ticks inside", stayIn));
    if (inside != 1) p.Problem(fmt::format("trigger_count said {} inside, not 1", inside));
    if (leave != 1) p.Problem(fmt::format("leaving was reported {} times, not once", leave));
    if (stayAfter - stayIn > 2) p.Problem(fmt::format("{} stay calls after leaving", stayAfter - stayIn));
    if (filtered != 0) p.Problem(fmt::format("the KActor-only trigger fired {} times for the player", filtered));
    if (!TriggersShared("who").starts_with("TdPlayerPawn")) p.Problem("the trigger's other actor wasn't the player: " + TriggersShared("who"));
    if (launched != 1) p.Problem(fmt::format("the mod's logic ran {} times on entering, not once", launched));

    // Off: taken away
    OnRenderThread([m] { m->SetEnabled(false); });
    WaitFor(10.0, [] { return !TriggersShared("reverted").empty(); });
    WaitTicks(3);
    int left = -1;
    OnGameThread([&] { left = static_cast<int>(game::ActorsOf(Classes::ATrigger::StaticClass()).size()); });
    s.notes.push_back(fmt::format("module off: reverted {}, Trigger actors in play {} (before the mod: {})", TriggersShared("reverted"), left, before));
    if (left != before) p.Problem("the mod's triggers are still in play after the revert");
    p.Check(1, true);
    End(s);
}

// Step 4 of the next ten steps, the plan's Scenario A: movement tuned from data (tests/mods/movement-tuning: five
// constants in content/defaults.toml, on the classes' defaults and the live objects). On: the player's move objects and
// pawn hold the mod's values and a standing jump goes as high as the faster jump makes it; off: every value is the
// game's again, exactly, and so is the jump. movement_get reports what the player is doing.
float StandingJump();   // Below

void TestMovementTuning()
{
    Step& s = Begin("Mirage movement tuning");
    Probe p(s);
    Module* m = modules::Find("Movement Tuning (test)");
    if (!m) {
        p.Problem("the Movement Tuning (test) module is missing (tests/mods/movement-tuning)");
        End(s);
        return;
    }
    struct Value
    {
        const char* move;       // A TdMove class in the pawn's Moves, or nullptr: the pawn itself
        const char* property;
        float tuned;
    };
    const Value values[] = {{"TdMove_Jump", "BaseJumpZ", 945.f}, {"TdMove_Jump", "JumpAddXY", 300.f},
                            {nullptr, "SprintVelocity", 900.f}, {nullptr, "RunVelocity", 700.f},
                            {"TdMove_WallRun", "WallRunningMinSpeed", 150.f}};
    // The live values: the player's move objects (TdPawn.Moves) and pawn
    const auto read = [&] {
        std::vector<float> out(std::size(values), -1.f);
        OnGameThread([&] {
            auto* pawn = static_cast<Classes::ATdPawn*>(game::Pawn());
            if (!pawn) return;
            for (std::size_t i = 0; i < std::size(values); ++i) {
                Classes::UObject* o = pawn;
                if (values[i].move) {
                    o = nullptr;
                    for (std::size_t k = 0; k < pawn->Moves.Num(); ++k)
                        if (auto* mv = pawn->Moves[k]; mv && mv->Class && mv->Class->GetName() == values[i].move) o = mv;
                }
                if (o) out[i] = mirage::reflect::GetFloat(mirage::reflect::Resolve(o->Class, o, values[i].property));
            }
        });
        return out;
    };
    const auto set = [&](bool on) {
        OnRenderThread([m, on] { m->SetEnabled(on); });
        return WaitFor(5.0, [&] {
            const auto now = read();
            return on ? now[0] == values[0].tuned : now[0] != values[0].tuned;
        });
    };
    // Every jump from the same place: only the player's location put back (a save state would put the move objects'
    // properties back too, the tuning with them)
    Classes::FVector start{};
    const auto jump = [&] {
        OnGameThread([&] {
            if (auto* pawn = game::Pawn()) {
                pawn->Location = start;
                pawn->Velocity = Classes::FVector{};
            }
        });
        WaitGrounded();
        return StandingJump();
    };
    set(false);
    WaitGrounded();
    OnGameThread([&] { if (auto* pawn = game::Pawn()) start = pawn->Location; });
    const std::vector<float> game = read();
    const float normal = jump();

    if (!set(true)) p.Problem("the mod's values didn't reach the player's move objects");
    const std::vector<float> tuned = read();
    for (std::size_t i = 0; i < std::size(values); ++i)
        if (tuned[i] != values[i].tuned)
            p.Problem(fmt::format("on: {}.{} is {}, not the mod's {}", values[i].move ? values[i].move : "TdPlayerPawn",
                                  values[i].property, tuned[i], values[i].tuned));
    MirageMovement mv{};
    mv.size = sizeof mv;
    int got = 0;
    OnGameThread([&] { got = mirage::mods::game_api::MovementGet(&mv); });
    if (!got || !mv.move_object) p.Problem("movement_get didn't report the player's move");
    const float high = jump();

    set(false);
    const std::vector<float> back = read();
    for (std::size_t i = 0; i < std::size(values); ++i)
        if (back[i] != game[i])
            p.Problem(fmt::format("off: {}.{} is {}, not the game's {}", values[i].move ? values[i].move : "TdPlayerPawn",
                                  values[i].property, back[i], game[i]));
    const float after = jump();

    // A jump's height goes with its upward speed squared: the mod's 945 against the game's BaseJumpZ
    const float expected = game[0] > 0.f ? normal * (values[0].tuned / game[0]) * (values[0].tuned / game[0]) : 0.f;
    s.notes.push_back(fmt::format("standing jump: {:.0f} (BaseJumpZ {:.0f}), tuned {:.0f} (expected about {:.0f}), off again "
                                  "{:.0f}; movement_get: move {}, previous {}, physics {}", normal, game[0], high, expected,
                                  after, mv.move, mv.previous_move, mv.physics));
    if (normal <= 10.f) p.Problem("the player didn't jump");
    else {
        if (high < expected * 0.75f || high > expected * 1.25f)
            p.Problem(fmt::format("tuned, the jump reached {:.0f}, not about {:.0f}", high, expected));
        if (std::fabs(after - normal) > normal * 0.1f) p.Problem(fmt::format("off again, the jump is {:.0f}, not {:.0f}", after, normal));
    }
    WaitGrounded();
    // The transition record (movement_transitions): the jumps above are in it, named from the engine's EMovement
    {
        std::string text(mirage::mods::game_api::MovementTransitions(nullptr, 0) + 1, '\0');
        text.resize(std::min<std::size_t>(mirage::mods::game_api::MovementTransitions(text.data(), static_cast<std::uint32_t>(text.size())),
                                          text.size() - 1));
        const auto list = nlohmann::json::parse(text, nullptr, false);
        int jumps = 0, named = 0;
        std::string sample;
        if (list.is_array())
            for (const auto& t : list) {
                const std::string to = t.value("to_name", "");
                named += !to.empty();
                if (to == "MOVE_Jump") {
                    ++jumps;
                    if (sample.empty()) sample = t.value("from_name", "") + " -> " + to + fmt::format(" (tick {})", t.value("tick", 0ull));
                }
            }
        const std::size_t total = list.is_array() ? list.size() : 0;
        s.notes.push_back(fmt::format("transition record: {} entries, {} named, {} into MOVE_Jump (e.g. {})", total, named, jumps, sample));
        if (jumps < 3) p.Problem(fmt::format("the transition record has {} jumps, not the 3 jumped here", jumps));
        if (named != static_cast<int>(total)) p.Problem("some transitions have no move name");
    }
    p.Check(1, true);
    End(s);
}

// The engine's symbols and hooks by symbol (step 2 of the next ten steps): every pattern of runtime/symbols.toml is
// found, exactly once; the names the runtime's code works out are there too. tests/mods/hook-symbol detours
// StaticLoadObject by name and counts the calls; with its Crash setting on, the next call faults in its detour: the load
// still works (the guard goes on in the real function), the mod is stopped and named, and the game goes on.
void TestEngineSymbols()
{
    Step& s = Begin("Mirage engine symbols");
    Probe p(s);
    namespace mm = mirage::mods;
    int fromToml = 0, provided = 0;
    for (const auto& sym : mirage::symbols::List()) {
        if (sym.provided) {
            ++provided;
            continue;
        }
        ++fromToml;
        if (!sym.address) p.Problem(sym.name + " wasn't found: " + sym.problem);
        else if (sym.matches != sym.expected)
            p.Problem(fmt::format("{} matches in {} places, not the {} symbols.toml says", sym.name, sym.matches, sym.expected));
    }
    for (const char* name : {"FrameTiming", "PreDeath", "PostDeath", "GConfig", "GMalloc"})
        if (!mirage::symbols::Find(name)) p.Problem(std::string("the runtime's code didn't provide ") + name);
    s.notes.push_back(fmt::format("{} symbols from symbols.toml, {} provided by the runtime's code", fromToml, provided));
    if (const std::uintptr_t n = mirage::symbols::Find("UObject::execNew"))
        s.notes.push_back(fmt::format("UObject::execNew at MirrorsEdge.exe+{:#x}", n - reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr))));

    Module* m = modules::Find("Hook (symbol)");
    if (!m) {
        p.Problem("the Hook (symbol) module is missing (tests/mods/hook-symbol)");
        End(s);
        return;
    }
    const auto calls = [m] {
        auto* v = dynamic_cast<ctl::Value<int>*>(m->FindSetting("Calls"));
        return v ? v->Get() : -1;
    };
    const auto load = [] {
        Classes::UObject* o = nullptr;
        OnGameThread([&] { o = mirage::classes::LoadByName("Engine.Actor", Classes::UClass::StaticClass()); });
        return o;
    };
    OnRenderThread([m] { m->SetEnabled(true); });
    WaitTicks(2);
    void* target = mirage::symbols::Find<void>("StaticLoadObject");
    if (mirage::detours::Chain(target).empty()) p.Problem("hook-symbol's detour of StaticLoadObject isn't on it");
    const int before = calls();
    if (!load()) p.Problem("StaticLoadObject didn't load Engine.Actor through the detour");
    if (calls() <= before) p.Problem("hook-symbol's detour didn't see the call");
    // Its mid-function hook inside StaticLoadObject, where the function reads its Name: it saw "Engine.Actor"
    if (auto* mid = dynamic_cast<ctl::Value<int>*>(m->FindSetting("Mid calls")); !mid || mid->Get() < 1)
        p.Problem("hook-symbol's mid-function hook didn't see the load of Engine.Actor (its registers)");

    // The fault: the load goes on in the real function, the mod is named, its detour is gone
    p.Expect("[mod hook-symbol] CRASHED");
    if (auto* crash = dynamic_cast<ctl::Value<bool>*>(m->FindSetting("Crash"))) OnRenderThread([crash] { crash->Set(true); });
    if (!load()) p.Problem("with the detour faulting, StaticLoadObject didn't load Engine.Actor (the guard didn't go on)");
    const auto named = [] {
        for (const auto& i : mm::List())
            if (i.id == "hook-symbol") return i.problem;
        return std::string("hook-symbol not found");
    };
    if (!WaitFor(5.0, [&] { return named().find("its detour of StaticLoadObject") != std::string::npos; }))
        p.Problem("the mod wasn't stopped and named for its detour: " + named());
    else s.notes.push_back("hook-symbol: " + named());
    // Named first, then its hooks removed (both on the game thread): the chain empties right after
    if (!WaitFor(5.0, [&] { return mirage::detours::Chain(target).empty(); }))
        p.Problem("the faulted detour is still on StaticLoadObject");
    if (!load()) p.Problem("after the fault, StaticLoadObject doesn't load any more");
    const auto ticks = Ticks();
    WaitTicks(3);
    if (Ticks() == ticks) p.Problem("the game stopped after the detour's fault");
    p.Check(1, true);
    End(s);
}

void TestMirageModHooks()
{
    Step& s = Begin("Mirage hooks from mods");
    Probe p(s);
    namespace mm = mirage::mods;
    Module* native = modules::Find("Hook (native)");
    Module* lua = modules::Find("Hook (Lua)");
    if (!native || !lua) {
        p.Problem("the Hook (native) or Hook (Lua) module is missing (tests/mods)");
        p.Check(1, true);
        End(s);
        return;
    }
    const auto value = [](Module* m, const char* name) {
        auto* v = dynamic_cast<ctl::Value<float>*>(m->FindSetting(name));
        return v ? v->Get() : -999.f;
    };
    // One call of CheckMaxEffectDistance from 0, `count` times: the time per call (us)
    const auto calls = [&](int count) {
        double us = -1.0;
        OnGameThread([&] {
            auto* pc = game::Controller();
            auto* pawn = game::Pawn();
            if (!pc || !pawn) return;
            mirage::call::Params params(pawn, "CheckMaxEffectDistance");
            const Classes::FVector at = pawn->Location;
            const clk::Ticks start = clk::Now();
            for (int i = 0; i < count; ++i) {
                params.Set("P", static_cast<Classes::UObject*>(pc));
                params.SetRaw("SpawnLocation", &at, sizeof at);
                params.Set("CullDistance", 0.f);
                mirage::call::Call(pawn, params);
            }
            us = clk::Micros(clk::Now() - start) / count;
        }, 30.0);
        return us;
    };
    const auto set = [](Module* m, bool on) { OnRenderThread([m, on] { m->SetEnabled(on); }); };
    constexpr int kCalls = 2000;

    set(native, false);
    set(lua, false);
    const auto baseline = mirage::hooks::GetStats();   // Other mods' own hooks (first-party mods keep some)
    const double bare = calls(kCalls);
    set(native, true);
    const double withNative = calls(kCalls);
    if (value(native, "Saw") != 1.f) p.Problem("hook-native alone saw " + std::to_string(value(native, "Saw")) + ", not 1");
    set(native, false);
    set(lua, true);
    const double withLua = calls(kCalls);
    if (value(lua, "Saw") != 2.f) p.Problem("hook-lua alone saw " + std::to_string(value(lua, "Saw")) + ", not 2");

    // Both: priority 20 (native) first
    set(native, true);
    calls(1);
    if (value(native, "Saw") != 1.f || value(lua, "Saw") != 12.f)
        p.Problem("both on: native saw " + std::to_string(value(native, "Saw")) + ", Lua " + std::to_string(value(lua, "Saw")) +
                  " (expected 1 then 12: priority order)");

    // Reloaded with their hooks on: the same order, no hook doubled or lost
    for (int i = 1; i <= 3; ++i) {
        for (const char* id : {"hook-native", "hook-lua"}) {
            std::string result = "timed out";
            OnGameThread([&] { result = mm::Reload(id); }, 10.0);
            if (!result.empty()) p.Problem(std::string("reload of ") + id + ": " + result);
        }
        calls(1);
        const auto st = mirage::hooks::GetStats();
        if (st.hooks - baseline.hooks != 2)
            p.Problem("reload " + std::to_string(i) + ": " + std::to_string(st.hooks - baseline.hooks) + " hooks, not 2");
        if (value(native, "Saw") != 1.f || value(lua, "Saw") != 12.f)
            p.Problem("reload " + std::to_string(i) + ": the order changed (native " + std::to_string(value(native, "Saw")) +
                      ", Lua " + std::to_string(value(lua, "Saw")) + ")");
    }

    // Objects by name, events
    WaitTicks(5);
    float pawnZ = 0.f;
    OnGameThread([&] { if (auto* pawn = game::Pawn()) pawnZ = pawn->Location.Z; });
    if (std::fabs(value(native, "Pawn Z") - pawnZ) > 50.f || std::fabs(value(lua, "Pawn Z") - pawnZ) > 50.f)
        p.Problem("the pawn's Location.Z read by name: native " + std::to_string(value(native, "Pawn Z")) + ", Lua " +
                  std::to_string(value(lua, "Pawn Z")) + ", the pawn " + std::to_string(pawnZ));
    auto* ticks = dynamic_cast<ctl::Value<int>*>(lua->FindSetting("Ticks"));
    auto* lastMap = dynamic_cast<ctl::Value<std::string>*>(lua->FindSetting("Last map"));
    if (!ticks || ticks->Get() == 0) p.Problem("hook-lua got no tick events");
    if (!lastMap || lastMap->Get().empty()) p.Problem("hook-lua got no level load event (with the map)");

    // Off: nothing left hooked
    set(native, false);
    set(lua, false);
    if (const auto left = mirage::hooks::GetStats().hooks - baseline.hooks; left != 0)
        p.Problem(std::to_string(left) + " hooks left with both modules off");

    // Costs. The calls go through ProcessEvent and run the function, so the hooks' share is the difference.
    const double lookupNs = mirage::hooks::UnhookedLookupNanoseconds();
    const double nativeUs = withNative - bare, luaUs = withLua - bare;
    char line[256];
    std::snprintf(line, sizeof line, "a call: %.2f us; a native hook adds %.2f us, a Lua hook %.2f us; an unhooked script "
                  "call pays %.1f ns for the detour's lookup", bare, nativeUs, luaUs, lookupNs);
    s.notes.push_back(line);
    // The budgets are release numbers (measured there: 0.44 us native, 1.25 us Lua). A debug build runs everything
    // 7-20 times slower (the bare call too), so it gets ten times the budget: still far below what a real regression costs.
#ifdef NDEBUG
    constexpr double kScale = 1.0;
#else
    constexpr double kScale = 10.0;
#endif
    const auto budget = [&](double limit) { return std::to_string(static_cast<int>(limit * kScale)); };
    if (lookupNs > 100.0 * kScale) p.Problem("the detour's lookup costs " + std::to_string(lookupNs) + " ns (budget " + budget(100) + ")");
    if (nativeUs > 5.0 * kScale) p.Problem("a native hook adds " + std::to_string(nativeUs) + " us a call (budget " + budget(5) + ")");
    if (luaUs > 50.0 * kScale) p.Problem("a Lua hook adds " + std::to_string(luaUs) + " us a call (budget " + budget(50) + ")");
    s.notes.push_back("map " + (lastMap ? lastMap->Get() : std::string()) + ", " + std::to_string(ticks ? ticks->Get() : 0) + " tick events");
    p.Check(1, true);
    End(s);
}

// The cache system (core/cache_evict.h, core/cache_sched.h, game/object_query.h): the object index is built in slices by
// the cache scheduler; its answers are the same as a scan of the whole object list, and faster (timed here against the
// scans it replaced); the actor cache reads the levels' own actor lists; everything is under the eviction manager.
// The memory manager (runtime/mem): it measures the process and its address space, finds a leak made on purpose, answers
// simulated pressure (caches to their floor, handlers called, recovery after) and costs little per step.
void TestMemoryManager()
{
    Step& s = Begin("Memory manager");
    Probe p(s);
    namespace mm = mirage::mem;
    char line[240];

    // 1. Measurements: an address space walk finished, the process read
    if (!WaitFor(15.0, [] { return mm::GetStatus().address.complete; })) p.Problem("no address space walk finished in 15 s");
    mm::Status st = mm::GetStatus();
    if (st.process.privateBytes == 0) p.Problem("the process's memory was not read");
    std::snprintf(line, sizeof line, "address space: %.0f MB free, largest block %.0f MB, fragmentation %.2f, %zu regions walked in %.2f ms",
                  st.address.free.totalFree / 1048576.0, st.address.free.largestFree / 1048576.0, st.address.free.Index(), st.address.regions,
                  st.address.walkMs);
    s.notes.push_back(line);
    std::snprintf(line, sizeof line, "pressure %s; menu heap %.1f MB; %zu Lua mods measured", mm::LevelName(st.level),
                  st.heap.live / 1048576.0, st.modLua.size());
    s.notes.push_back(line);

    // 2. A leak made on purpose, found by its stack; what was freed is not reported
    std::vector<char*> leaked;
    mm::sampler::Report leak;
    {
        mm::LeakCheck check;
        for (int i = 0; i < 3; ++i) leaked.push_back(new char[1000]);
        for (int i = 0; i < 50; ++i) delete[] new char[500];
        leak = check.Finish();
    }
    if (leak.liveBytes < 3000) p.Problem("the leak check found " + std::to_string(leak.liveBytes) + " bytes, not the 3,000 left on purpose");
    if (leak.liveBytes > 3000 + 2048) p.Problem("the leak check found " + std::to_string(leak.liveBytes) + " bytes: freed blocks counted too");
    if (leak.sites.empty() || leak.sites[0].frames.empty()) p.Problem("the leak has no call stack");
    else s.notes.push_back("leak found at " + mm::sampler::Describe(leak.sites[0].frames[0]));
    for (char* c : leaked) delete[] c;
    if (mm::sampler::CurrentMode() != mm::Configured().tracking) p.Problem("the leak check didn't put the tracking mode back");

    // 3. Simulated pressure: handlers run, caches go to their floor, then come back
    // ("Ended": any level under High after it; this machine may sit at Elevated on its own, e.g. little commit left)
    struct Seen { std::atomic<int> high{0}, normal{0}; } seen;
    const int handler = mm::AddPressureHandler("self-test", 0, [&seen](mm::Level level, bool) {
        if (level >= mm::Level::High) ++seen.high;
        else if (seen.high.load() > 0) ++seen.normal;
        return std::size_t{0};
    });
    const mm::Config cfg = mm::Configured();
    const std::size_t budgetBefore = cache::Evictions().Budget();
    if (!cfg.act) s.notes.push_back("the Memory module isn't acting (switched off): pressure checks skipped");
    else {
        mm::SimulatePressure(mm::Level::High, 3.0);
        if (!WaitFor(5.0, [&] { return seen.high.load() > 0; })) p.Problem("simulated High pressure never reached the handlers");
        // High halves the budget each second (gradual: no single long frame); 3 s of it takes 16 MB to 2 MB or the floor
        if (!WaitFor(3.5, [&] { return cache::Evictions().Budget() <= std::max(cfg.cacheMin, budgetBefore / 4); }))
            p.Problem("under High pressure the cache budget is still " + std::to_string(cache::Evictions().Budget() >> 20) + " MB");
        if (!WaitFor(10.0, [&] { return seen.normal.load() > 0; }))
            p.Problem(std::string("the handlers were not told the pressure ended (level now ") + mm::LevelName(mm::GetStatus().level) + ": " +
                      mm::GetStatus().reason + ")");
        if (!WaitFor(30.0, [&] { return cache::Evictions().Budget() >= std::min(budgetBefore, cfg.cacheMax); }))
            p.Problem("the cache budget didn't come back after the pressure (" + std::to_string(cache::Evictions().Budget() >> 20) + " MB)");
        st = mm::GetStatus();
        if (st.actions == 0) p.Problem("no pressure action was counted");
    }
    mm::RemovePressureHandler(handler);

    // 4. A trim on request, pools and arenas in the report, the mods' API
    {
        mm::SyncObjectPool<std::array<int, 16>> pool("self-test pool");
        std::vector<std::array<int, 16>*> made;
        for (int i = 0; i < 500; ++i) made.push_back(pool.Create());
        for (auto* m : made) pool.Destroy(m);
        mm::Scratch scratch;
        (void)scratch.Array<int>(4096);
        const std::string report = mm::Report(false);
        for (const char* part : {"Pressure:", "Address space:", "Menu heap:", "Profiler:", "self-test pool", "Caches:"})
            if (report.find(part) == std::string::npos) p.Problem(std::string("the report has no \"") + part + "\"");
        mm::RequestTrim();
        if (!WaitFor(5.0, [&] { return pool.Stats().slabs <= 1; })) p.Problem("a requested trim didn't free the pool's empty slabs");
    }
    MirageMemoryStats ms{};
    ms.size = sizeof ms;
    if (mirage::mods::Api()->memory_stats(nullptr, &ms) != 1 || ms.process_private == 0) p.Problem("memory_stats gave nothing");

    // 5. Its own cost
    st = mm::GetStatus();
    // Steady state first: a few steps with nothing to do after the trim above
    WaitFor(4.0, [] { return false; });
    st = mm::GetStatus();
    std::snprintf(line, sizeof line, "game thread step %.3f ms now, %.3f average, %.3f max (an action: caches enforced, mods told); monitor pass %.2f ms (max %.2f, own thread); malloc %.0f ns average (timed 1 in %u)",
                  st.stepMs, st.avgStepMs, st.maxStepMs, st.monitorMs, st.maxMonitorMs, mm::ReadTiming(true).averageNs, mm::kTimingEvery);
    s.notes.push_back(line);
    s.notes.push_back(std::string("pressure ") + mm::LevelName(st.level) + (st.reason.empty() ? "" : ": " + st.reason));
    if (st.stepMs > 1.0) p.Problem("a manager step with nothing to do took " + std::to_string(st.stepMs) + " ms on the game thread (budget 1 in debug)");
    if (st.maxStepMs > 10.0) p.Problem("a manager action took " + std::to_string(st.maxStepMs) + " ms on the game thread (budget 10 in debug)");
    End(s);
}

// The crash reporter, the event bus and background tasks as wired in the game, and the reflection
// path cache measured against parsing every path.
namespace crash_test {
// A real access violation, caught: its exception pointers are copied for the report (the copies outlive the handler)
EXCEPTION_RECORD g_record;
CONTEXT g_context;
int Filter(EXCEPTION_POINTERS* ep)
{
    g_record = *ep->ExceptionRecord;
    g_context = *ep->ContextRecord;
    return EXCEPTION_EXECUTE_HANDLER;
}
// A thread that crashes for real (nothing catches it): the crash window's end-to-end check
DWORD WINAPI CrashingThread(LPVOID)
{
    SetThreadDescription(GetCurrentThread(), L"mirage self-test crash");
    volatile int* p = reinterpret_cast<volatile int*>(0x24);
    *p = 1;
    return 0;
}

bool Fault()
{
    __try {
        volatile int* p = reinterpret_cast<volatile int*>(0x10);
        *p = 1;
    } __except (Filter(GetExceptionInformation())) {
        return true;
    }
    return false;
}
}

void TestCrashReporter()
{
    Step& s = Begin("Crash reporter");
    Probe p(s);
    namespace fs = std::filesystem;

    // 1. A report from a real (caught) exception: written, with what and where, without ending the game
    {
        diag::ExpectedFault expected;    // Not a crash for the crash logger (it would flag the run)
        if (!crash_test::Fault()) p.Problem("the test fault didn't fault");
    }
    EXCEPTION_POINTERS ep{&crash_test::g_record, &crash_test::g_context};
    // With a minidump: written by the helper process (loader/crashdump/), from outside the running game. From inside,
    // dbghelp deadlocked here (the report written, the .dmp left at 0 bytes, the game frozen).
    const std::wstring report = mirage::crash::WriteReport(&ep, "self-test (not a real crash)");
    if (report.empty()) p.Problem("no report was written");
    else {
        const std::string text = mirage::crash::ReadReport(fs::path(report));
        for (const char* part : {"What: access violation", "Where: Mirage.dll+", "Access: writing 0x00000010", "Call stack", "Modules ("})
            if (text.find(part) == std::string::npos) p.Problem(std::string("the report has no \"") + part + "\"");
        std::error_code ec;
        const auto dumpSize = fs::file_size(fs::path(report).replace_extension(".dmp"), ec);
        if (ec || dumpSize < 64 * 1024) p.Problem("no minidump next to the report (the helper mirage_crashdump.exe)");
        s.notes.push_back("report: " + fs::path(report).filename().string() + ", " + std::to_string(text.size()) +
                          " bytes; minidump " + std::to_string(ec ? 0 : dumpSize / 1024) + " KB");
    }

    // 1b. The crash handler's analysis in the report: named frames (Mirage.dll has its PDB next to it), the
    // UnrealScript stack's section, the code at the crash; and the same analysis in the crash window's preview
    if (!report.empty()) {
        const std::string text = mirage::crash::ReadReport(fs::path(report), 256 * 1024);
        for (const char* part : {"Thread: ", "UnrealScript call stack:", "Code at the crash:", "=> ", "Registers:"})
            if (text.find(part) == std::string::npos) p.Problem(std::string("the report has no \"") + part + "\" (the analysis)");
        crashhandler::Analysis a;
        if (!mirage::crash::LastAnalysis(a)) p.Problem("the reporter kept no analysis");
        else {
            const bool pdb = fs::exists(paths::Dir() / "Mirage.pdb");
            // Named from the PDB: crash_test::Fault in a debug build. An optimized one folds identical functions together
            // (/OPT:ICF), so the address may carry another function's name there: a name is what is checked.
#ifdef NDEBUG
            const bool named = !a.frames.empty() && !a.frames.front().symbol.empty();
#else
            const bool named = !a.frames.empty() && a.frames.front().symbol.find("Fault") != std::string::npos;
#endif
            if (pdb && !named)
                p.Problem("the faulting frame isn't named from the PDB: " + (a.frames.empty() ? std::string("no frames") : a.frames.front().Full()));
            if (a.disasm.empty() || std::none_of(a.disasm.begin(), a.disasm.end(), [](const crashhandler::DisasmLine& l) { return l.current; }))
                p.Problem("no disassembly at the fault");
            if (a.findings.empty()) p.Problem("no possible cause for a write to 0x10 (the null pointer analyzer)");
            s.notes.push_back("analysis: " + a.where.Full() + ", " + std::to_string(a.frames.size()) + " frames, " +
                              std::to_string(a.findings.size()) + " finding(s)");
            crashhandler::window::Preview(a, report);
            if (!WaitFor(5.0, [] { return crashhandler::window::PreviewFrames() >= 3; }))
                p.Problem("the crash window's preview didn't draw");
            crashhandler::window::ClosePreview();
        }
        std::error_code ec;
        fs::remove(fs::path(report), ec);
        fs::remove(fs::path(report).replace_extension(".dmp"), ec);
    }

    // 1b'. Names for the game's code: an address a little into a native function is named after it (the table the
    // crash handler builds from the native UFunctions on the game thread)
    {
        std::string expected, got;
        OnGameThread([&] {
            crashhandler::symbols::Refresh();
            const int funcOffset = mirage::hooks::FuncOffset();
            auto* fn = game::FindObject<Classes::UFunction>("Function Engine.Actor.Trace");
            if (!fn || funcOffset <= 0) return;
            const std::uint32_t func = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const char*>(fn) + funcOffset);
            expected = "Actor.execTrace";
            got = crashhandler::symbols::Describe(func + 0x10).Full();
        });
        if (expected.empty()) p.Problem("Engine.Actor.Trace or UFunction::Func wasn't found (the engine names can't be checked)");
        else if (got.find(expected) == std::string::npos) p.Problem("the game's code isn't named after its native function: " + got);
        else s.notes.push_back("engine names: " + got);
    }

    // 1c. A real crash on a thread of its own: the crash window comes up over the running game, "End this thread" ends
    // that thread, and the game goes on. No minidump for it (it would only slow the run).
    {
        p.Expect("Crash in the menu");             // The crash logger sees the fault first (it is in this DLL)
        p.Expect("the crashed thread");            // The reporter's warning that the thread ended
        mirage::crash::SetFatalDump(false);
        mirage::crash::SetUnattended(false);       // This check answers the window itself
        HANDLE t = CreateThread(nullptr, 0, &crash_test::CrashingThread, nullptr, 0, nullptr);
        if (!t) p.Problem("the crashing thread didn't start");
        else {
            if (!WaitFor(20.0, [] { return crashhandler::window::Showing() && crashhandler::window::FramesDrawn() >= 3; }))
                p.Problem("the crash window didn't come up for a crashed thread");
            crashhandler::window::Choose(crashhandler::window::Action::EndThread);
            if (WaitForSingleObject(t, 10000) != WAIT_OBJECT_0) p.Problem("the crashed thread didn't end");
            else {
                DWORD code = 0;
                GetExitCodeThread(t, &code);
                if (code != EXCEPTION_ACCESS_VIOLATION) p.Problem(fmt::format("the crashed thread ended with 0x{:X}", code));
            }
            CloseHandle(t);
            if (crashhandler::window::Showing()) p.Problem("the crash window is still up");
            crashhandler::Analysis a;
            if (mirage::crash::LastAnalysis(a)) {
                if (a.threadName.find("mirage self-test crash") == std::string::npos)
                    p.Problem("the crashed thread isn't named: " + a.threadName);
                s.notes.push_back("crash window: " + std::to_string(crashhandler::window::FramesDrawn()) + " frames, " + a.threadName);
            }
            const auto before = Ticks();
            WaitTicks(5);
            if (Ticks() == before) p.Problem("the game stopped after the crashed thread ended");
        }
        mirage::crash::SetFatalDump(true);
        mirage::crash::SetUnattended(true);
    }

    // 2. The event bus and the background tasks, through the game's dispatchers
    std::atomic<bool> onGame{false};
    const DWORD gameThread = [] {
        DWORD id = 0;
        OnGameThread([&] { id = GetCurrentThreadId(); });
        return id;
    }();
    struct SelfTestPing { int n; };
    auto sub = events::Bus().Subscribe<SelfTestPing>([&](const SelfTestPing&) { onGame = GetCurrentThreadId() == gameThread; },
                                                     {.where = events::Where::GameThread, .owner = "self-test"});
    events::Bus().Publish(SelfTestPing{1});
    if (!WaitFor(3.0, [&] { return onGame.load(); })) p.Problem("a game-thread event handler didn't run on the game thread");
    auto task = tasks::Run([] { return GetCurrentThreadId(); }, {.name = "self-test"});
    std::atomic<bool> continued{false};
    task.Then([&](const DWORD&) { continued = GetCurrentThreadId() == gameThread; }, tasks::On::GameThread);
    if (!task.Wait(5.0) || !task.Ok()) p.Problem("a background task didn't finish: " + task.Error());
    else if (*task.Result() == gameThread) p.Problem("the background task ran on the game thread");
    if (!WaitFor(3.0, [&] { return continued.load(); })) p.Problem("the task's continuation didn't run on the game thread");
    const auto ts = tasks::Snapshot();
    s.notes.push_back(std::to_string(ts.workers) + " task workers, " + std::to_string(ts.completed) + " tasks done");

    // 3. The reflection path cache: the same reads, parsed every time against cached (measured on the game thread)
    double uncachedUs = 0.0, cachedUs = 0.0;
    bool same = true;
    OnGameThread([&] {
        auto* pawn = game::Pawn();
        if (!pawn) return;
        const char* paths[] = {"Location.Z", "Velocity.X", "Rotation.Yaw", "Health", "Controller.Location.X"};
        constexpr int kRounds = 2000;
        for (int pass = 0; pass < 2; ++pass) {
            mirage::reflect::SetPathCache(pass == 1);
            const auto t0 = clk::Now();
            for (int r = 0; r < kRounds; ++r)
                for (const char* path : paths) {
                    const auto v = mirage::reflect::Resolve(pawn->Class, pawn, path);
                    if (!v && r == 0) same = false;
                }
            (pass ? cachedUs : uncachedUs) = clk::Micros(clk::Now() - t0) / (kRounds * 5.0);
        }
        // The same answers both ways
        for (const char* path : paths) {
            mirage::reflect::SetPathCache(false);
            const auto a = mirage::reflect::Resolve(pawn->Class, pawn, path);
            mirage::reflect::SetPathCache(true);
            const auto b = mirage::reflect::Resolve(pawn->Class, pawn, path);
            if (a.address != b.address || a.member != b.member) same = false;
        }
        mirage::reflect::SetPathCache(true);
    });
    if (!same) p.Problem("the path cache resolved a path differently from parsing it");
    char line[160];
    std::snprintf(line, sizeof line, "path resolve: %.3f us parsed, %.3f us cached (%.1fx)", uncachedUs, cachedUs,
                  cachedUs > 0 ? uncachedUs / cachedUs : 0.0);
    s.notes.push_back(line);
    if (cachedUs > uncachedUs) p.Problem("the path cache is slower than parsing");
    End(s);
}

// C# mods (runtime/mod_dotnet.h): the .NET runtime starts, examples/mods/hello-cs (built by game-test with the .NET
// SDK) ticks and reads the player, an exception in a module switches it off, hot reload unloads the old assembly, and
// C# scripts (examples/mods/hello-script, tests/mods/script-error) compile in the game or say why not.
void TestCSharpMods()
{
    Step& s = Begin("Mirage C# mods");
    Probe p(s);
    namespace mm = mirage::mods;
    bool found = false;
    for (const mm::Info& i : mm::List())
        if (i.id == "hello-cs") {
            found = true;
            if (!i.loaded) p.Problem("hello-cs didn't load: " + i.problem);
            if (i.kind.find("C#") == std::string::npos) p.Problem("hello-cs is listed as " + i.kind);
        }
    if (!found) {
        p.Problem("hello-cs isn't installed (game-test builds it with the .NET SDK)");
        End(s);
        return;
    }
    const auto st = mirage::dotnet::GetStatus();
    if (!st.started) {
        p.Problem("the C# runtime didn't start: " + st.problem);
        End(s);
        return;
    }
    char line[200];
    std::snprintf(line, sizeof line, ".NET %s started in %.0f ms, %.1f MB of address space", st.version.c_str(), st.startMs,
                  (static_cast<double>(st.addressBefore) - static_cast<double>(st.addressAfter)) / 1048576.0);
    s.notes.push_back(line);

    Module* hello = nullptr;
    Module* thrower = nullptr;
    modules::ForEach([&](modules::Tab&, Module& m) {
        if (m.Name() == "Hello (C#)") hello = &m;
        if (m.Name() == "Throw (C#)") thrower = &m;
    });
    if (!hello || !thrower) {
        p.Problem("the C# modules aren't in the menu");
        End(s);
        return;
    }
    const auto intOf = [](Module* m, const char* name) {
        auto* v = dynamic_cast<ctl::SliderInt*>(m->FindSetting(name));
        return v ? v->Get() : -1;
    };
    const auto floatOf = [](Module* m, const char* name) {
        auto* v = dynamic_cast<ctl::Slider*>(m->FindSetting(name));
        return v ? v->Get() : -1e9f;
    };

    // 1. It ticks, gets events, reads the player
    if (!hello->Enabled()) OnRenderThread([&] { hello->SetEnabled(true); });
    const int ticksBefore = intOf(hello, "Ticks");
    WaitTicks(30);
    if (intOf(hello, "Ticks") <= ticksBefore) p.Problem("Hello (C#) doesn't tick");
    if (intOf(hello, "Tick events") <= 0) p.Problem("the C# tick event never arrived");
    float pawnZ = 0.f;
    OnGameThread([&] { if (auto* pawn = game::Pawn()) pawnZ = pawn->Location.Z; });
    if (std::fabs(floatOf(hello, "Pawn Z") - pawnZ) > 50.f)
        p.Problem("C# read Location.Z " + std::to_string(floatOf(hello, "Pawn Z")) + ", the pawn is at " + std::to_string(pawnZ));
    for (const auto& e : sched::Snapshot())
        if (e.name == "Hello (C#)") {
            std::snprintf(line, sizeof line, "C# module tick %.1f us average (reads the pawn by path)", e.avgUs);
            s.notes.push_back(line);
        }

    // 2. An exception in a module: logged, the module off, the game untouched
    p.Expect("thrown on purpose by the self-test");
    const auto logContains = [](const std::string& part) {
        std::vector<logging::Entry> entries;
        logging::CopySince(0, entries);
        return std::any_of(entries.begin(), entries.end(), [&](const logging::Entry& e) { return e.text.find(part) != std::string::npos; });
    };
    OnRenderThread([&] { thrower->SetEnabled(true); });
    if (!WaitFor(5.0, [&] { return !thrower->Enabled(); })) p.Problem("a C# module that throws stayed on");
    if (!WaitFor(2.0, [&] { return logContains("Throw (C#): OnTick threw InvalidOperationException"); }))
        p.Problem("the C# exception wasn't logged with its module");

    // 3. Hot reload: the old assembly context unloads (nothing kept it alive), the module comes back and ticks
    std::string reload = "timed out";
    OnGameThread([&] { reload = mm::Reload("hello-cs"); }, 15.0);
    if (!reload.empty()) p.Problem("hot reload of hello-cs: " + reload);
    Module* again = nullptr;
    modules::ForEach([&](modules::Tab&, Module& m) { if (m.Name() == "Hello (C#)") again = &m; });
    if (!again) p.Problem("Hello (C#) is gone after the reload");
    else {
        if (!again->Enabled()) OnRenderThread([&] { again->SetEnabled(true); });
        const int t0 = intOf(again, "Ticks");
        WaitTicks(20);
        if (intOf(again, "Ticks") <= t0) p.Problem("Hello (C#) doesn't tick after the reload");
    }
    // 4. Scripts: examples/mods/hello-script is compiled in the game and ticks; tests/mods/script-error doesn't compile,
    //    and why names the file and line; the editor's check sees unsaved text
    const mm::Info* script = nullptr;
    const mm::Info* broken = nullptr;
    const auto mods = mm::List();
    for (const mm::Info& i : mods) {
        if (i.id == "hello-script") script = &i;
        if (i.id == "script-error") broken = &i;
    }
    if (!script || !script->loaded) p.Problem("the C# script mod hello-script didn't load: " + (script ? script->problem : "not installed"));
    else {
        Module* sm = nullptr;
        modules::ForEach([&](modules::Tab&, Module& m) { if (m.Name() == "Script (C#)") sm = &m; });
        if (!sm) p.Problem("the script's module isn't in the menu");
        else {
            if (!sm->Enabled()) OnRenderThread([&] { sm->SetEnabled(true); });
            const int t0 = intOf(sm, "Ticks");
            WaitTicks(20);
            if (intOf(sm, "Ticks") <= t0) p.Problem("the C# script's module doesn't tick");
        }
        const auto clean = mirage::dotnet::CheckScripts(script->dir, {}, {});
        if (clean.errors != 0) p.Problem("checking hello-script found " + std::to_string(clean.errors) + " error(s): " + clean.output);
        const auto dirty = mirage::dotnet::CheckScripts(script->dir, script->dir / "main.cs", "public sealed class X : Mod { int y = ; }");
        if (dirty.errors <= 0 || dirty.output.find("main.cs(1,") == std::string::npos)
            p.Problem("checking unsaved text with an error: " + std::to_string(dirty.errors) + " error(s), " + dirty.output);
        if (const auto ms = dirty.output.rfind('('); ms != std::string::npos) s.notes.push_back("script check " + dirty.output.substr(ms));
    }
    if (!broken) p.Problem("tests/mods/script-error isn't installed");
    else if (broken->loaded) p.Problem("script-error loaded although it doesn't compile");
    else if (broken->problem.find("main.cs(5,") == std::string::npos || broken->problem.find("CS0029") == std::string::npos)
        p.Problem("script-error's problem doesn't name the file, line and error: " + broken->problem);
    for (const auto& w : script ? script->warnings : std::vector<std::string>{}) s.notes.push_back("hello-script: " + w);

    const auto after = mirage::dotnet::GetStatus(true);
    if (after.stillLoaded != 0) p.Problem(std::to_string(after.stillLoaded) + " unloaded C# mod context(s) still alive (a reference kept it)");
    std::snprintf(line, sizeof line, "managed heap %.2f MB after a reload, %d old contexts alive", after.managedBytes / 1048576.0,
                  after.stillLoaded);
    s.notes.push_back(line);
    End(s);
}

#if MIRAGE_FEATURE_SCRIPT_EDITOR
#endif

void TestCacheSystem()
{
    Step& s = Begin("Cache system");
    Probe p(s);
    // A pass made in slices by the cache scheduler (not one a query made at once)
    if (!WaitFor(20.0, [] { const auto st = game::GetQueryStats(); return st.passes > st.exactPasses; }))
        p.Problem("the cache scheduler didn't complete an object index pass in 20 s");
    const auto ms = [](clk::Ticks t) { return clk::Seconds(t) * 1000.0; };
    char line[240];
    OnGameThread([&] {
        const auto& objects = Classes::UObject::GetGlobalObjects();
        // ---- By class: every UFunction
        Classes::UClass* fnClass = Classes::UFunction::StaticClass();
        clk::Ticks t0 = clk::Now();
        std::size_t scanned = 0;
        for (std::size_t i = 0; i < objects.Num(); ++i)
            if (auto* o = objects.GetByIndex(i); o && o->Class == fnClass) ++scanned;
        const double scanMs = ms(clk::Now() - t0);
        t0 = clk::Now();
        std::size_t indexed = 0;
        for (auto* o : game::ObjectsOf(fnClass)) indexed += o->Class == fnClass;
        const double indexMs = ms(clk::Now() - t0);
        if (indexed != scanned) p.Problem("ObjectsOf(Function) found " + std::to_string(indexed) + ", a scan " + std::to_string(scanned));
        std::snprintf(line, sizeof line, "by class: %zu functions in %.3f ms (a scan: %.3f ms)", indexed, indexMs, scanMs);
        s.notes.push_back(line);

        // ---- By path: the old fallback built every object's name
        const std::string path = "TdGame.Default__TdPawn";
        t0 = clk::Now();
        Classes::UObject* slow = nullptr;
        for (std::size_t i = 0; i < objects.Num() && !slow; ++i)
            if (auto* o = objects.GetByIndex(i); o && o->GetName() == "Default__TdPawn" && o->GetPathName() == path) slow = o;
        const double slowMs = ms(clk::Now() - t0);
        t0 = clk::Now();
        Classes::UObject* fast = game::FindByPath(path);
        const double fastMs = ms(clk::Now() - t0);
        if (!slow || fast != slow) p.Problem("FindByPath(" + path + ") isn't what a scan finds");
        std::snprintf(line, sizeof line, "by path: %s in %.3f ms (a name scan: %.3f ms, x%.0f)", path.c_str(), fastMs, slowMs,
                      fastMs > 0 ? slowMs / fastMs : 0.0);
        s.notes.push_back(line);

        // ---- Actors: the pawns in play
        Classes::UClass* pawnClass = Classes::APawn::StaticClass();
        t0 = clk::Now();
        std::vector<Classes::UObject*> all;
        for (std::size_t i = 0; i < objects.Num(); ++i)
            if (auto* o = objects.GetByIndex(i); o && game::IsA(o, pawnClass) && o->GetName().rfind("Default__", 0) != 0) all.push_back(o);
        const double pawnScanMs = ms(clk::Now() - t0);
        t0 = clk::Now();
        const auto pawns = game::ActorsOf(pawnClass);
        const double actorsMs = ms(clk::Now() - t0);
        t0 = clk::Now();
        game::ActorsOf(pawnClass);
        const double againMs = ms(clk::Now() - t0);
        const auto st = game::GetQueryStats();
        if (!st.levelListsOk) p.Problem("the levels' actor lists weren't usable (ULevel::Actors at 0x3C, WorldInfo first)");
        if (std::find(pawns.begin(), pawns.end(), static_cast<Classes::AActor*>(game::Pawn())) == pawns.end())
            p.Problem("ActorsOf(Pawn) hasn't the player's pawn");
        for (auto* a : pawns) {
            if (!game::IsA(a, pawnClass)) { p.Problem("ActorsOf(Pawn) returned a " + a->Class->GetName()); break; }
            if (std::find(all.begin(), all.end(), a) == all.end()) { p.Problem("ActorsOf(Pawn) returned an object a scan doesn't find"); break; }
        }
        std::snprintf(line, sizeof line, "actors: %zu pawns of %zu actors in %zu levels in %.3f ms, %.4f ms again this tick (a scan: "
                      "%.3f ms, %zu pawn objects)", pawns.size(), st.actors, st.levels, actorsMs, againMs, pawnScanMs, all.size());
        s.notes.push_back(line);
        std::snprintf(line, sizeof line, "index: %zu slots, %zu classes, %zu names; %llu passes (%llu at once for a query, "
                      "%llu sliced ones restarted), the last %.2f ms of scanning", st.slots, st.classes, st.names,
                      static_cast<unsigned long long>(st.passes), static_cast<unsigned long long>(st.exactPasses),
                      static_cast<unsigned long long>(st.restarts), st.lastPassMs);
        s.notes.push_back(line);

        // ---- The managers
        const auto pass = cache::Evictions().Enforce();
        const auto caches = cache::Evictions().Snapshot();
        if (std::none_of(caches.begin(), caches.end(), [](const auto& v) { return v.name == "Object index"; }))
            p.Problem("the object index isn't under the eviction manager");
        if (pass.after > cache::Evictions().Budget()) p.Problem("over the cache budget after an eviction pass");
        std::snprintf(line, sizeof line, "eviction: %zu caches, %.1f KB of %.0f KB budget", caches.size(), pass.after / 1024.0,
                      cache::Evictions().Budget() / 1024.0);
        s.notes.push_back(line);
    });
    const auto jobs = cache::Maintenance().Snapshot(clk::Seconds(clk::Now()));
    for (const char* name : {"Object index", "Eviction"}) {
        const auto it = std::find_if(jobs.begin(), jobs.end(), [&](const auto& j) { return j.name == name; });
        if (it == jobs.end() || it->runs == 0) p.Problem(std::string("the cache scheduler never ran ") + name);
        else {
            std::snprintf(line, sizeof line, "job %s: %llu runs, %llu done, %.2f ms in total", name,
                          static_cast<unsigned long long>(it->runs), static_cast<unsigned long long>(it->completions), it->totalMs);
            s.notes.push_back(line);
        }
    }
    WaitTicks(5);       // The measurements ran in one game-thread call: frames go on meanwhile
    p.Check(1, true);
    End(s);
}

// Mirage step 7: a content mod (examples/mods/high-steps, data only) overrides a default property. Its module turns it
// on and off: on, the class's default object and the player's pawn have the mod's value, and a respawned pawn gets it;
// off, the exact original is back, on the pawn now and on the next one. No game file is written.
void TestMirageContent()
{
    Step& s = Begin("Mirage content");
    Probe p(s);
    Module* m = modules::Find("High Steps");
    if (!m) {
        p.Problem("the High Steps module (examples/mods/high-steps) is missing");
        p.Check(1, true);
        End(s);
        return;
    }
    // MaxStepHeight of the default object and of the pawn the player has now
    const auto readSteps = [&](float& cdo, float& pawn, void** which) {
        OnGameThread([&] {
            auto* d = game::FindObject<Classes::UObject>("TdPlayerPawn TdGame.Default__TdPlayerPawn");
            auto* pw = game::Pawn();
            cdo = d ? mirage::reflect::GetFloat(mirage::reflect::Resolve(d->Class, d, "MaxStepHeight")) : -1.f;
            pawn = pw ? pw->MaxStepHeight : -1.f;
            if (which) *which = pw;
        });
    };
    const auto set = [&](bool on) {
        OnRenderThread([m, on] { m->SetEnabled(on); });
        WaitTicks(3);       // Applied / put back on the game thread
    };
    // A new pawn by its ObjectId, not its address: the restarted pawn can get the old one's memory
    const auto respawn = [&] {
        const bool came = RestartAndWait();
        WaitTicks(10);      // RestartAndWait doesn't need the player unfrozen (the controller's bFrozen stays set)
        return came;
    };

    set(false);
    float cdo = 0, pawn = 0;
    readSteps(cdo, pawn, nullptr);
    const float original = cdo;
    if (pawn != original) p.Problem("off: the pawn's MaxStepHeight " + std::to_string(pawn) + " isn't the default " + std::to_string(original));

    set(true);
    readSteps(cdo, pawn, nullptr);
    if (cdo != 105.f || pawn != 105.f)
        p.Problem("on: MaxStepHeight is " + std::to_string(cdo) + " (default object) and " + std::to_string(pawn) + " (pawn), not 105");
    if (!respawn()) p.Problem("the player wasn't respawned");
    readSteps(cdo, pawn, nullptr);
    if (pawn != 105.f) p.Problem("on: a respawned pawn has MaxStepHeight " + std::to_string(pawn) + ", not 105");

    set(false);
    readSteps(cdo, pawn, nullptr);
    if (cdo != original || pawn != original)
        p.Problem("off: MaxStepHeight is " + std::to_string(cdo) + " / " + std::to_string(pawn) + ", not the original " + std::to_string(original));
    respawn();
    readSteps(cdo, pawn, nullptr);
    if (pawn != original) p.Problem("off: a respawned pawn has " + std::to_string(pawn) + ", not the original " + std::to_string(original));
    s.notes.push_back("MaxStepHeight " + std::to_string(original) + " -> 105 with the mod on, back to " + std::to_string(pawn) + " off");

    set(true);      // As the player would have it
    p.Check(1, true);
    End(s);
}

// Mirage step 7: a texture replaced by a content mod (examples/mods/retexture: one PNG, on from the start). The level
// loaded before this step opened the packages holding the texture as rewritten copies, made on demand: the texture
// object has the image's size. Turned off, a package the game already opened keeps its copy for the session (the
// engine keeps its files open: switching one under it hung the loader), so reloading the level works and still shows
// the mod; a package not opened yet is the original again, and turned back on, it would be a copy.
void TestMirageTextures()
{
    Step& s = Begin("Mirage textures");
    Probe p(s);
    Module* m = modules::Find("Retexture");
    if (!m) {
        p.Problem("the Retexture module (examples/mods/retexture) is missing");
        p.Check(1, true);
        End(s);
        return;
    }
    constexpr const char* kTexture = "Texture2D M_Rooftops.Textures.T_Roof_02_D";
    const auto size = [&] {
        int w = -1, h = -1;
        OnGameThread([&] {
            auto* t = game::FindObject<Classes::UObject>(kTexture);
            if (!t) return;
            w = mirage::reflect::GetInt(mirage::reflect::Resolve(t->Class, t, "SizeX"));
            h = mirage::reflect::GetInt(mirage::reflect::Resolve(t->Class, t, "SizeY"));
        });
        return std::pair{w, h};
    };
    const auto describe = [](std::pair<int, int> wh) { return std::to_string(wh.first) + "x" + std::to_string(wh.second); };
    const auto set = [&](bool on) {
        OnRenderThread([m, on] { m->SetEnabled(on); });
        WaitTicks(3);
        return mirage::textures::WaitIdle(120.0);
    };
    const auto reload = [&] {
        const int loads = g_loadsFinished.load();
        const auto maps = levels::Maps();
        for (int i = 0; i < static_cast<int>(maps.size()); ++i)
            if (Lower(maps[i].label).find(Lower(g_options.map)) != std::string::npos) {
                OnGameThread([i] { levels::Load(i, g_options.checkpoint, levels::Method::Game, false); });
                break;
            }
        if (!WaitFor(120.0, [&] { return g_loadsFinished.load() > loads && !game::IsLoading(); })) return false;
        WaitPlayerReady(30.0);
        return true;
    };
    const std::filesystem::path unopened = mirage::packages::CookedDir() / "Maps" / "SP09" / "Scraper_Roof.me1";

    if (!set(true)) p.Problem("the texture index took more than 2 minutes");
    const auto st = mirage::textures::GetStatus("retexture");
    for (const auto& problem : st.problems) p.Problem(problem);
    char line[200];
    std::snprintf(line, sizeof line, "index: %zu packages (%.1f s); the texture is in %d of them; %llu copies made "
                  "(%.1f s), %llu package opens from copies", st.indexedPackages, st.indexSeconds, st.packages,
                  static_cast<unsigned long long>(mirage::files::Built()), st.buildSeconds,
                  static_cast<unsigned long long>(mirage::files::Redirected()));
    s.notes.push_back(line);
    if (st.textures != 1 || st.packages < 1) p.Problem("the texture should be found in at least one package");

    const auto on = size();
    if (on != std::pair{256, 256}) p.Problem("on: the texture is " + describe(on) + ", not the mod's 256x256");
    if (mirage::files::Redirected() == 0) p.Problem("the game opened no package from a mod copy");

    set(false);
    if (mirage::files::Redirects(unopened)) p.Problem("off: a package not opened yet (Scraper_Roof) is still redirected");
    if (mirage::files::Held() == 0) p.Problem("off: no opened package is held (the level's packages should keep their copies)");
    if (!reload()) p.Problem("off: the level didn't reload (the loader must not see a package change files)");
    const auto held = size();
    if (held != std::pair{256, 256}) p.Problem("off, reloaded: the texture is " + describe(held) + ", not the held copy's 256x256");
    s.notes.push_back("off: " + std::to_string(mirage::files::Held()) + " opened packages keep their copies until a restart; "
                      "reloaded fine, texture " + describe(held));

    set(true);      // As the player would have it
    if (!mirage::files::Redirects(unopened)) p.Problem("on again: Scraper_Roof (not opened yet) would not be redirected");
    p.Check(1, true);
    End(s);
}

// Mirage step 8: a script patch from a Lua mod (examples/mods/higher-jumps). On, TdMove_Jump.StartMove's bytecode
// multiplies the jump's upward speed by 1.5: a standing jump goes about 2.25 times as high, with no native detour.
// Off, the function's own bytecode is back and the jump is as before.
// Classes from a package file anywhere (step 12): a copy of a script package the game ships, renamed and put outside
// the game's folders, loads through StaticLoadObject with its path, and its class is the class the engine made from it
void TestClassesProbe()
{
    Step& s = Begin("Mirage classes (probe)");
    Probe p(s);
    namespace fs = std::filesystem;
    const fs::path source = paths::Dir().parent_path().parent_path() / "TdGame" / "CookedPC" / "MirrorsEdgeConsole.u";
    std::error_code ec;
    if (!fs::exists(source, ec))
    {
        s.notes.push_back("skipped: no MirrorsEdgeConsole.u to copy");
        End(s);
        return;
    }
    const fs::path copy = paths::Dir() / "selftest-mods" / "MirageProbe.u";
    fs::create_directories(copy.parent_path(), ec);
    fs::copy_file(source, copy, fs::copy_options::overwrite_existing, ec);
    std::string why;
    Classes::UClass* cls = nullptr;
    OnGameThread([&] {
        if (!mirage::classes::Ready(&why)) return;
        cls = mirage::classes::LoadClassFrom(copy, "MirrorsEdgeConsole", &why);
    }, 20.0);
    if (!cls) p.Problem("the class didn't load from " + copy.string() + ": " + why);
    else
    {
        std::string path, super;
        OnGameThread([&] {
            path = cls->GetPathName();
            if (cls->SuperField) super = cls->SuperField->GetName();
        });
        s.notes.push_back("loaded " + path + " (extends " + super + ") from " + copy.filename().string());
        if (path != "MirageProbe.MirrorsEdgeConsole") p.Problem("the class's path is " + path);
        if (super != "TdConsole") p.Problem("its super is " + super + ", not TdConsole");
    }
    End(s);
}

// A class written in UnrealScript (step 12's milestone): MirageBouncePad.uc, a Trigger with a property and a Touch
// event, compiled by the runtime and loaded by the engine. Spawned on the player, its Touch counts and launches them.
// bNoDelete=False: a Trigger is a level's own actor (bNoDelete), and the engine spawns none of those.
const char* kBouncePadSource = R"(class MirageBouncePad extends Trigger
    placeable;

var() float Strength;
var int Bounces;

event Touch(Actor Other, PrimitiveComponent OtherComp, Vector HitLocation, Vector HitNormal)
{
    local Pawn P;

    P = Pawn(Other);
    if (P == None)
    {
        return;
    }
    Bounces++;
    P.Velocity.Z = Strength;
    P.SetPhysics(PHYS_Falling);
}

function int TimesBounced()
{
    return Bounces;
}

defaultproperties
{
    Strength=1500.0
    bNoDelete=False
}
)";

// A bounce pad spawned on the player: its Strength default, its Touch counted (Bounces) and the player launched
struct Bounce
{
    int bounces = 0;
    float velocityZ = 0.f;
};
Bounce BounceOn(Classes::UClass* cls, Probe& p, Step& s, bool destroy = true)
{
    Bounce out;
    WaitGrounded();
    Classes::AActor* pad = nullptr;
    float strength = 0.f;
    OnGameThread([&] {
        auto* pawn = game::Pawn();
        if (!pawn) return;
        pad = pawn->Spawn(cls, nullptr, Classes::FName{}, pawn->Location, pawn->Rotation, nullptr, true);
        if (!pad) return;
        strength = mirage::reflect::GetFloat(mirage::reflect::Resolve(cls, pad, "Strength"));
        // A nudge: a move finds what the pawn now overlaps
        Classes::FVector at = pawn->Location;
        at.Z += 1.f;
        pawn->SetLocation(at);
    });
    if (!pad)
    {
        p.Problem("it didn't spawn");
        return out;
    }
    if (strength != 1500.f) p.Problem(fmt::format("its Strength is {} (the default is 1500)", strength));
    for (int i = 0, after = 0; i < 60 && after < 10; ++i, after += out.bounces > 0 ? 1 : 0)   // Ten ticks past the touch
    {
        WaitTicks(1);
        OnGameThread([&] {
            out.bounces = mirage::reflect::GetInt(mirage::reflect::Resolve(cls, pad, "Bounces"));
            auto* pawn = game::Pawn();
            if (!pawn) return;
            out.velocityZ = (std::max)(out.velocityZ, pawn->Velocity.Z);
            // Not touched yet (right after another pad's bounce a single move may not register): another small move
            if (out.bounces == 0 && i % 4 == 3)
            {
                Classes::FVector at = pawn->Location;
                at.Z += (i % 8 == 3) ? -1.f : 1.f;
                pawn->SetLocation(at);
            }
        });
    }
    s.notes.push_back(fmt::format("touched {} time(s), the player's vertical speed reached {:.0f}", out.bounces, out.velocityZ));
    if (out.bounces == 0) p.Problem("its Touch event never ran");
    else if (out.velocityZ < 1000.f) p.Problem(fmt::format("the player wasn't launched (vertical speed {:.0f})", out.velocityZ));
    if (destroy) OnGameThread([&] { pad->Destroy(); });
    WaitGrounded();
    return out;
}

void TestClassesSource()
{
    Step& s = Begin("Mirage classes (.uc)");
    Probe p(s);
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = paths::Dir() / "selftest-classes";
    fs::create_directories(dir, ec);
    const fs::path uc = dir / "MirageBouncePad.uc";
    {
        std::ofstream f(uc, std::ios::binary | std::ios::trunc);
        f << kBouncePadSource;
    }
    // Built from scratch every run (a package built by an older builder is not reused)
    for (const auto& e : fs::directory_iterator(paths::Dir() / "classes", ec))
        if (e.path().filename().string().rfind("MirageBouncePad_", 0) == 0) fs::remove(e.path(), ec);

    mirage::classes::BuiltClass built;
    std::string why;
    const clk::Ticks start = clk::Now();
    if (!mirage::classes::BuildSource(uc, built, &why))
    {
        p.Problem("it didn't compile: " + why);
        End(s);
        return;
    }
    s.notes.push_back(fmt::format("compiled {} in {:.2f} s (the game's script packages read with it)", built.package.filename().string(),
                                  clk::SecondsSince(start)));
    // The same source again: the package is reused
    mirage::classes::BuiltClass again;
    if (!mirage::classes::BuildSource(uc, again, &why) || !again.cached || again.package != built.package)
        p.Problem("the same source was built again");

    Classes::UClass* cls = nullptr;
    OnGameThread([&] { cls = mirage::classes::LoadClassFrom(built.package, built.className, &why); }, 20.0);
    if (!cls)
    {
        p.Problem("the engine didn't load it: " + why);
        End(s);
        return;
    }
    std::string path, super;
    OnGameThread([&] {
        path = cls->GetPathName();
        if (cls->SuperField) super = cls->SuperField->GetName();
    });
    s.notes.push_back("loaded " + path + " (extends " + super + ")");
    if (super != "Trigger") p.Problem("its super is " + super + ", not Trigger");

    // Spawned on the player: the engine's Touch runs the compiled bytecode
    BounceOn(cls, p, s);
    // UClass::ClassFlags where runtime/classes reads it: Trigger is native and placeable; the built class placeable
    std::uint32_t trigger = 0, own = 0;
    OnGameThread([&] {
        trigger = mirage::classes::ClassFlags(Classes::UClass::FindClass("Class Engine.Trigger"));
        own = mirage::classes::ClassFlags(cls);
    });
    if ((trigger & 0x280u) != 0x280u || !(own & 0x200u))
        p.Problem(fmt::format("class flags read wrong (Trigger 0x{:08X}, MirageBouncePad 0x{:08X})", trigger, own));
    End(s);
}

// Step 12's milestone, the other half: tests/mods/class-lua and class-cs describe the bounce pad from Lua
// (mirage.define_class) and C# (Mod.DefineClass), its Touch and a new function written in the mod's language; it
// behaves as the .uc pad (the player is launched), TimesBounced returns the mod's value. A hot reload of the mod retires the class (its actors destroyed, abstract: no new spawns);
// the reloaded mod defines it again from the same source: the same class, spawnable again.
void TestDescribedClass(const char* step, const char* moduleName, const char* className, const char* modId)
{
    Step& s = Begin(step);
    Probe p(s);
    namespace mm = mirage::mods;
    Module* m = modules::Find(moduleName);
    if (!m)
    {
        p.Problem(std::string("the ") + moduleName + " module is missing (tests/mods/" + modId + ")");
        End(s);
        return;
    }
    const auto findClass = [className] {
        Classes::UClass* found = nullptr;
        OnGameThread([&] {
            auto& objects = Classes::UObject::GetGlobalObjects();
            for (int i = 0; i < static_cast<int>(objects.Num()); ++i)
            {
                Classes::UObject* o = objects.GetByIndex(i);
                if (o && o->Class == Classes::UClass::StaticClass() && o->GetName() == className) found = static_cast<Classes::UClass*>(o);
            }
        });
        return found;
    };
    OnRenderThread([m] { m->SetEnabled(true); });
    Classes::UClass* cls = nullptr;
    const clk::Ticks start = clk::Now();
    for (int i = 0; i < 300 && !cls; ++i)
    {
        WaitTicks(5);
        cls = findClass();
    }
    if (!cls)
    {
        p.Problem("the mod's class never appeared (the mod's log says why)");
        End(s);
        return;
    }
    std::string path;
    OnGameThread([&] { path = cls->GetPathName(); });
    s.notes.push_back(fmt::format("{} defined by {} in {:.2f} s", path, modId, clk::SecondsSince(start)));

    const Bounce b = BounceOn(cls, p, s, false);
    // TimesBounced: a function declared by the mod, its value returned by the mod's code
    int times = -1, bouncesNow = -2;
    OnGameThread([&] {
        Classes::UObject* pad = nullptr;
        auto& objects = Classes::UObject::GetGlobalObjects();
        for (int i = 0; i < static_cast<int>(objects.Num()); ++i)
            if (auto* o = objects.GetByIndex(i); o && o->Class == cls && !(*reinterpret_cast<std::uint64_t*>(reinterpret_cast<char*>(o) + 8) & 0x600) && !static_cast<Classes::AActor*>(o)->bDeleteMe) pad = o;   // A live pad, never the class's defaults (calling into them crashed the game)
        if (!pad) return;
        namespace ga = mirage::mods::game_api;
        if (MirageCall* call = ga::CallBegin(reinterpret_cast<MirageObject*>(pad), "TimesBounced"))
        {
            MirageValue v{};
            if (ga::CallInvoke(call) && ga::CallGet(call, "ReturnValue", &v)) times = v.i;
            ga::CallEnd(call);
        }
        bouncesNow = mirage::reflect::GetInt(mirage::reflect::Resolve(cls, pad, "Bounces"));
    });
    (void)b;
    if (times != bouncesNow) p.Problem(fmt::format("TimesBounced returned {}, Bounces is {}", times, bouncesNow));
    // class-lua's Twice: declared `native("double")` in UnrealScript, the mod's Lua native behind it
    if (std::string(modId) == "class-lua")
    {
        float twice = -1.f;
        OnGameThread([&] {
            Classes::UObject* pad = nullptr;
            auto& objects = Classes::UObject::GetGlobalObjects();
            for (int i = 0; i < static_cast<int>(objects.Num()); ++i)
                if (auto* o = objects.GetByIndex(i); o && o->Class == cls && !(*reinterpret_cast<std::uint64_t*>(reinterpret_cast<char*>(o) + 8) & 0x600) && !static_cast<Classes::AActor*>(o)->bDeleteMe) pad = o;   // A live pad, never the class's defaults (calling into them crashed the game)
            namespace ga = mirage::mods::game_api;
            if (MirageCall* call = pad ? ga::CallBegin(reinterpret_cast<MirageObject*>(pad), "Twice") : nullptr)
            {
                MirageValue v{};
                v.type = MIRAGE_VALUE_FLOAT;
                v.f = 21.f;
                ga::CallSet(call, "X", &v);
                if (ga::CallInvoke(call) && ga::CallGet(call, "ReturnValue", &v)) twice = v.f;
                ga::CallEnd(call);
            }
        });
        s.notes.push_back(fmt::format("Twice(21) = {} (a native(\"double\") function, the mod's Lua native)", twice));
        if (twice != 42.f) p.Problem(fmt::format("Twice(21) returned {}, not 42", twice));
    }

    // Its lifetime: a hot reload retires it (the pad still there is destroyed, the class can't spawn), the reloaded
    // mod's definition brings it back
    int alive = 0;
    const auto countPads = [&] {
        alive = 0;
        OnGameThread([&] {
            auto& objects = Classes::UObject::GetGlobalObjects();
            for (int i = 0; i < static_cast<int>(objects.Num()); ++i)
                if (auto* o = objects.GetByIndex(i); o && o->Class == cls && !(*reinterpret_cast<std::uint64_t*>(reinterpret_cast<char*>(o) + 8) & 0x200))
                    if (!static_cast<Classes::AActor*>(o)->bDeleteMe) ++alive;
        });
        return alive;
    };
    if (countPads() != 1) p.Problem(fmt::format("{} pads before the reload, not 1", alive));
    std::string reload;
    std::uint32_t retiredFlags = 0;
    OnGameThread([&] {
        reload = mm::Reload(modId);
        retiredFlags = mirage::classes::ClassFlags(cls);
    }, 20.0);
    if (!reload.empty()) p.Problem("hot reload: " + reload);
    if (!(retiredFlags & 1u)) p.Problem("after the unload, the class isn't abstract (it could still be spawned)");
    if (countPads() != 0) p.Problem(fmt::format("{} pads still there after the mod's unload", alive));
    std::uint32_t flags = 1;
    for (int i = 0; i < 100 && (flags & 1u); ++i)
    {
        WaitTicks(2);
        OnGameThread([&] { flags = mirage::classes::ClassFlags(cls); });
    }
    if (flags & 1u) p.Problem("the reloaded mod's definition didn't bring the class back");
    else if (findClass() != cls) p.Problem("the reloaded mod got another class for the same source");
    else BounceOn(cls, p, s);

    // mirage.keep's flag: RF_RootSet on and off
    OnGameThread([&] {
        Classes::UObject* o = game::Pawn();
        if (!o) return;
        auto& f = *reinterpret_cast<std::uint64_t*>(reinterpret_cast<char*>(o) + 8);
        const std::uint64_t before = f & 0x4000;
        mirage::classes::Keep(o, true);
        const bool kept = (f & 0x4000) != 0;
        mirage::classes::Keep(o, before != 0);
        if (!kept) p.Problem("Keep didn't set RF_RootSet");
    });
    OnRenderThread([m] { m->SetEnabled(false); });
    End(s);
}

// The example mod (examples/mods/bounce-pad): its UnrealScript class compiled from the mod's folder, a pad spawned from
// Lua (mirage.call(pawn, "Spawn", { SpawnClass = ... })) when its module is switched on, and the player launched by it
void TestClassesExample()
{
    Step& s = Begin("Mirage classes (example mod)");
    Probe p(s);
    Module* m = modules::Find("Bounce Pad");
    if (!m)
    {
        p.Problem("the Bounce Pad module is missing (examples/mods/bounce-pad)");
        End(s);
        return;
    }
    WaitGrounded();
    // The example's pads: the actors of a class named MirageBouncePad from a package the .uc test didn't build
    const auto pads = [] {
        std::vector<Classes::AActor*> out;
        auto& objects = Classes::UObject::GetGlobalObjects();
        for (int i = 0; i < static_cast<int>(objects.Num()); ++i)
        {
            Classes::UObject* o = objects.GetByIndex(i);
            if (!o || !o->Class || o->Class->GetName() != "MirageBouncePad") continue;
            if (*reinterpret_cast<std::uint64_t*>(reinterpret_cast<char*>(o) + 8) & 0x600) continue;   // Defaults, templates
            if (o->Class->GetPathName() == "MirageBouncePad_59abd376.MirageBouncePad") continue;          // The .uc test's
            if (!static_cast<Classes::AActor*>(o)->bDeleteMe) out.push_back(static_cast<Classes::AActor*>(o));
        }
        return out;
    };
    std::size_t before = 0;
    OnGameThread([&] { before = pads().size(); });
    OnRenderThread([m] { m->SetEnabled(true); });
    Classes::AActor* pad = nullptr;
    // The pad spawns where the player stands and may launch them in its first tick, before it is found here: the
    // player's vertical speed is followed from the switch-on, every tick, so the launch's peak can't be missed
    float velocityZ = 0.f;
    for (int i = 0; i < 600 && !pad; ++i)    // About 10 s: the first compile takes 3-4 s
    {
        WaitTicks(1);
        OnGameThread([&] {
            if (auto* pawn = game::Pawn()) velocityZ = (std::max)(velocityZ, pawn->Velocity.Z);
            const auto now = pads();
            if (now.size() > before) pad = now.back();
        });
    }
    if (!pad)
    {
        p.Problem("no pad was dropped (the mod's log says why)");
        End(s);
        return;
    }
    std::string cls;
    OnGameThread([&] { cls = pad->Class->GetPathName(); });
    s.notes.push_back("dropped a " + cls);
    int bounces = 0;
    for (int i = 0, after = 0; i < 80 && after < 10; ++i, after += bounces > 0 ? 1 : 0)   // Ten ticks past the touch
    {
        WaitTicks(1);
        OnGameThread([&] {
            bounces = mirage::reflect::GetInt(mirage::reflect::Resolve(pad->Class, pad, "Bounces"));
            auto* pawn = game::Pawn();
            if (!pawn) return;
            velocityZ = (std::max)(velocityZ, pawn->Velocity.Z);
            if (bounces == 0 && i % 4 == 3)
            {
                Classes::FVector at = pawn->Location;
                at.Z += (i % 8 == 3) ? -1.f : 1.f;
                pawn->SetLocation(at);
            }
        });
    }
    s.notes.push_back(fmt::format("touched {} time(s), the player's vertical speed reached {:.0f}", bounces, velocityZ));
    if (bounces == 0) p.Problem("the pad's Touch never ran");
    else if (velocityZ < 1000.f) p.Problem(fmt::format("the player wasn't launched (vertical speed {:.0f})", velocityZ));
    OnGameThread([&] { pad->Destroy(); });
    WaitGrounded();
    End(s);
}

void TestClassesLua() { TestDescribedClass("Mirage classes (Lua)", "Class (Lua)", "MirageBouncePadLua", "class-lua"); }
void TestClassesCSharp() { TestDescribedClass("Mirage classes (C#)", "Class (C#)", "MirageBouncePadCs", "class-cs"); }

// Level additions (step 13): tests/mods/layer-test, a content mod, adds two pads of its own UnrealScript class to
// Edge_p (levels/edge_p.toml), one with a property set, and one entry whose class doesn't exist (reported, not fatal).
// A checkpoint restart spawns them again, fresh (a pad's changed state is gone); a hot reload of the mod replaces them.
void TestLevelLayers()
{
    Step& s = Begin("Mirage level additions");
    Probe p(s);
    namespace mm = mirage::mods;
    struct Seen
    {
        int spawned = 0, problems = 0;
        Classes::AActor *a = nullptr, *b = nullptr;
        game::ObjectId aId;         // PadA by its identity: a new pad can get the old one's memory
        std::string missing;
    };
    const auto look = [] {
        Seen out;
        OnGameThread([&] {
            for (const auto& i : mirage::layers::List())
            {
                if (i.mod != "layer-test") continue;
                if (i.actor) ++out.spawned;
                if (!i.problem.empty()) ++out.problems;
                if (i.name == "PadA") {
                    out.a = i.actor;
                    if (i.actor) out.aId = game::IdOf(i.actor);
                }
                if (i.name == "PadB") out.b = i.actor;
                if (i.name == "Missing") out.missing = i.problem;
            }
        });
        return out;
    };
    Seen first;
    WaitFor(20.0, [&] { first = look(); return first.spawned >= 2; });
    s.notes.push_back(fmt::format("{} actor(s) spawned, {} with a problem ({})", first.spawned, first.problems, first.missing));
    if (first.spawned != 2 || !first.a || !first.b)
    {
        p.Problem(fmt::format("{} of the layer's 2 pads spawned (the log says why)", first.spawned));
        End(s);
        return;
    }
    if (first.problems != 1 || first.missing.find("no class") == std::string::npos)
        p.Problem("the entry with a missing class isn't reported as such: " + first.missing);
    float strengthA = 0.f, strengthB = 0.f, z = 0.f;
    OnGameThread([&] {
        strengthA = mirage::reflect::GetFloat(mirage::reflect::Resolve(first.a->Class, first.a, "Strength"));
        strengthB = mirage::reflect::GetFloat(mirage::reflect::Resolve(first.b->Class, first.b, "Strength"));
        z = first.a->Location.Z;
        // A pad's state changed: the restart must give a fresh one
        mirage::reflect::SetInt(mirage::reflect::Resolve(first.a->Class, first.a, "Bounces"), 7);
    });
    if (strengthA != 1500.f || strengthB != 1800.f)
        p.Problem(fmt::format("Strength {} and {}: not the default 1500 and the layer's 1800", strengthA, strengthB));
    if (z != 9017.f) p.Problem(fmt::format("PadA is at Z {}, not the layer's 9017", z));
    // As a mod finds it (API layer_actor)
    MirageObject* byName = nullptr;
    OnGameThread([&] { byName = mirage::mods::game_api::LayerActor(nullptr, "layer-test", "PadB"); });
    if (byName != reinterpret_cast<MirageObject*>(first.b)) p.Problem("layer_actor(\"layer-test\", \"PadB\") didn't find PadB");

    // The streamed sublevel (a copy of the game's Cranes_Bake as the package LayerTestArea): loaded and shown, with
    // its 16 static mesh actors (their outer chain: PersistentLevel, TheWorld, the package)
    struct Sub
    {
        Classes::ULevelStreaming* streaming = nullptr;
        bool loaded = false, visible = false;
        int meshes = 0;
        std::string problem;
    };
    const auto sublevel = [] {
        Sub out;
        OnGameThread([&] {
            for (const auto& i : mirage::layers::Sublevels())
                if (i.mod == "layer-test" && i.package == "LayerTestArea")
                {
                    out.streaming = i.streaming;
                    out.loaded = i.loaded;
                    out.visible = i.visible;
                    out.problem = i.problem;
                }
            auto& objects = Classes::UObject::GetGlobalObjects();
            for (int i = 0; i < static_cast<int>(objects.Num()); ++i)
            {
                Classes::UObject* o = objects.GetByIndex(i);
                if (!o || !o->Class || o->Class->GetName() != "StaticMeshActor" || static_cast<Classes::AActor*>(o)->bDeleteMe) continue;
                const Classes::UObject* package = o->Outer && o->Outer->Outer ? o->Outer->Outer->Outer : nullptr;
                if (package && !package->Outer && package->GetName() == "LayerTestArea") ++out.meshes;
            }
        });
        return out;
    };
    Sub sub;
    WaitFor(20.0, [&] { sub = sublevel(); return sub.loaded && sub.visible && sub.meshes > 0; });
    s.notes.push_back(fmt::format("sublevel LayerTestArea: loaded {}, visible {}, {} static mesh actor(s){}", sub.loaded, sub.visible,
                                  sub.meshes, sub.problem.empty() ? "" : " (" + sub.problem + ")"));
    if (!sub.streaming) p.Problem("the sublevel wasn't added: " + sub.problem);
    else if (!sub.loaded || !sub.visible) p.Problem("the sublevel didn't load and show");
    else if (sub.meshes != 16) p.Problem(fmt::format("the sublevel has {} static mesh actors, not the package's 16", sub.meshes));

    // Unloaded with the mod's layer (what a mod's unload does), then streamed in again: once, not twice
    if (sub.loaded)
    {
        OnGameThread([] { mirage::layers::Remove("layer-test"); });
        Sub gone;
        WaitFor(10.0, [&] { gone = sublevel(); return gone.meshes == 0; });
        if (gone.meshes != 0) p.Problem(fmt::format("{} of the sublevel's actors still there after its unload", gone.meshes));
        Sub back;
        WaitFor(20.0, [&] { back = sublevel(); return back.loaded && back.visible && back.meshes > 0 && back.streaming != sub.streaming; });
        s.notes.push_back(fmt::format("after an unload and the layer again: {} static mesh actor(s)", back.meshes));
        if (back.meshes != 16) p.Problem(fmt::format("the sublevel came back with {} static mesh actors, not 16", back.meshes));
        sub = back;
        // The pads were removed and spawned again with it; the new PadA gets the changed state the restart must undo
        WaitFor(10.0, [&] { first = look(); return first.spawned >= 2; });
        if (first.a)
            OnGameThread([&] { mirage::reflect::SetInt(mirage::reflect::Resolve(first.a->Class, first.a, "Bounces"), 7); });
    }

    // A checkpoint restart: the pads spawned again, fresh
    const int generation = mirage::layers::Generation();
    OnRenderThread([] { levels::RestartFromLastCheckpoint(); });
    WaitFor(30.0, [&] { return mirage::layers::Generation() > generation; });
    Seen again;
    WaitFor(10.0, [&] { again = look(); return again.spawned >= 2; });
    int bounces = -1;
    if (again.a) OnGameThread([&] { bounces = mirage::reflect::GetInt(mirage::reflect::Resolve(again.a->Class, again.a, "Bounces")); });
    s.notes.push_back(fmt::format("after a checkpoint restart: {} pad(s), PadA's Bounces {}", again.spawned, bounces));
    if (mirage::layers::Generation() <= generation) p.Problem("the restart wasn't seen");
    else if (again.spawned != 2) p.Problem(fmt::format("{} pad(s) after the restart", again.spawned));
    else if (bounces != 0) p.Problem(fmt::format("PadA kept its state through the restart (Bounces {})", bounces));
    if (sub.loaded)
    {
        const Sub kept = sublevel();
        if (kept.streaming != sub.streaming || !kept.loaded || kept.meshes != 16)
            p.Problem(fmt::format("the sublevel didn't stay through the restart (loaded {}, {} actors)", kept.loaded, kept.meshes));
    }
    // The old ones are gone
    int stale = 0;
    OnGameThread([&] {
        auto& objects = Classes::UObject::GetGlobalObjects();
        for (int i = 0; i < static_cast<int>(objects.Num()); ++i)
            if (auto* o = objects.GetByIndex(i); o && o->Class && o->Class->GetName() == "LayerPad" &&
                                                 !(*reinterpret_cast<std::uint64_t*>(reinterpret_cast<char*>(o) + 8) & 0x600) &&
                                                 !static_cast<Classes::AActor*>(o)->bDeleteMe)
                ++stale;
    });
    if (stale != 2) p.Problem(fmt::format("{} live pads of the layer's class after the restart, not 2", stale));

    // A hot reload of the mod: replaced
    WaitGrounded();
    std::string reload;
    OnGameThread([&] { reload = mm::Reload("layer-test"); }, 20.0);
    if (!reload.empty()) p.Problem("hot reload: " + reload);
    Seen reloaded;
    WaitFor(10.0, [&] { reloaded = look(); return reloaded.spawned >= 2 && !(reloaded.aId == again.aId); });
    if (reloaded.spawned != 2 || reloaded.aId == again.aId) p.Problem("the hot reload didn't replace the layer's actors");
    End(s);
}

// Maps from mods (step 13): tests/mods/map-test's maps/MirageTestMap.me1 (the game-test copies the Prologue's persistent
// level there) is in the game's level select data (the TdGameData store's TdMaps list, as the level select asks it) and
// in the menu's Levels list with its two checkpoints; it loads at one of them, and the game's Restart Checkpoint puts the
// player back at that checkpoint in the same map. Then the test level is loaded again.
void TestModMaps()
{
    Step& s = Begin("Mirage maps");
    Probe p(s);
    const auto mine = [] {
        for (const auto& m : mirage::maps::List())
            if (m.package == "MirageTestMap") return m;
        return mirage::maps::MapInfo{};
    };
    mirage::maps::MapInfo info;
    WaitFor(15.0, [&] { info = mine(); return info.listed; });
    if (info.package.empty()) {
        p.Problem("the map-test mod's map wasn't found (is the mod loaded? the log says)");
        End(s);
        return;
    }
    if (!info.listed) p.Problem("MirageTestMap isn't in the level select data: " + info.problem);

    // As the level select reads it: the store's list, by index
    std::vector<std::string> files;
    OnGameThread([&] {
        Classes::UObject* store = nullptr;
        auto& objects = Classes::UObject::GetGlobalObjects();
        for (int i = 0; i < static_cast<int>(objects.Num()) && !store; ++i)
            if (auto* o = objects.GetByIndex(i); o && game::IsA(o, Classes::UUIDataStore_TdGameData::StaticClass()) &&
                                                 o->GetName().rfind("Default__", 0) != 0)
                store = o;
        if (!store) return;
        mirage::call::Params count(store, "GetProviderCount");
        count.SetName("FieldName", "TdMaps");
        count.Set("bDoNotFilter", true);
        if (!mirage::call::Call(store, count)) return;
        const int n = count.GetInt("ReturnValue");
        for (int i = 0; i < n; ++i) {
            mirage::call::Params value(store, "GetStringValueFromProviderSet");
            value.SetName("ProviderFieldName", "TdMaps");
            value.SetName("SearchTag", "Filename");
            value.Set("ListIndex", i);
            if (!mirage::call::Call(store, value) || !value.GetBool("ReturnValue")) continue;
            Classes::FString text;
            value.GetRaw("OutString", &text, sizeof text);
            files.push_back(text.IsValid() ? text.ToString() : std::string());
        }
    });
    const bool inStore = std::find(files.begin(), files.end(), "MirageTestMap") != files.end();
    s.notes.push_back(fmt::format("the level select's TdMaps list: {} map(s){}", files.size(), inStore ? ", MirageTestMap among them" : ""));
    if (!inStore) p.Problem(fmt::format("MirageTestMap isn't in the data store's TdMaps list ({} entries)", files.size()));

    // The menu's Levels list, with the checkpoints from maps/MirageTestMap.toml
    std::vector<levels::MapView> maps;
    int index = -1;
    WaitFor(10.0, [&] {
        maps = levels::Maps();
        for (int i = 0; i < static_cast<int>(maps.size()); ++i)
            if (maps[i].file == "MirageTestMap") index = i;
        return index >= 0;
    });
    if (index < 0) {
        p.Problem("MirageTestMap isn't in the menu's Levels list");
        End(s);
        return;
    }
    s.notes.push_back("Levels list: " + maps[index].label + fmt::format(", {} checkpoint(s)", maps[index].checkpoints.size()));
    if (maps[index].checkpointNames != std::vector<std::string>{"Edge_Start", "After_Intro"})
        p.Problem("its checkpoints aren't the two of MirageTestMap.toml");

    // Loaded like any level, at its second checkpoint
    const auto load = [&](int map, int checkpoint, const std::string& want) -> bool {
        const int before = g_loadsFinished.load();
        OnRenderThread([map, checkpoint] { levels::Load(map, checkpoint, levels::Method::Game, false); });
        if (!WaitFor(120.0, [&] { return g_loadsFinished.load() > before; })) { p.Problem(want + " didn't load"); return false; }
        if (!WaitFor(60.0, [] { return game::GetStatus().hasPawn && !game::IsLoading(); })) { p.Problem("no player in " + want); return false; }
        if (const std::string why = WaitPlayerReady(30.0); !why.empty()) { p.Problem("in " + want + ": " + why); return false; }
        return true;
    };
    if (load(index, 2, "MirageTestMap")) {
        std::string map;
        OnGameThread([&] { map = game::MapName(); });
        s.notes.push_back("loaded: " + map + ", " + levels::LastMessage());
        if (Lower(map) != "miragetestmap") p.Problem("the level loaded is " + map + ", not MirageTestMap");

        // Restarted from its checkpoint: moved away first, the game's own restart puts a new player back where the
        // checkpoint spawned the first one, in the same map
        if (Module* restart = modules::Find("Restart Checkpoint")) {
            Classes::FVector spawn{};
            const void* pawnBefore = nullptr;
            OnGameThread([&] {
                if (auto* pawn = game::Pawn()) {
                    spawn = pawn->Location;
                    pawnBefore = pawn;
                    pawn->Location.X += 600.f;
                }
            });
            const int loadsBefore = g_loadsFinished.load();
            OnRenderThread([restart] { restart->Toggle(); });
            const void* pawnAfter = nullptr;
            WaitFor(60.0, [&] {
                OnGameThread([&] { pawnAfter = game::Pawn(); }, 1.0);
                return pawnAfter && pawnAfter != pawnBefore && !game::IsLoading();
            });
            // Walking again, with no cutscene or input hold. Not WaitPlayerReady(): the engine leaves the controller's
            // bFrozen set after its own checkpoint restart (see the death test)
            const bool walking = WaitFor(30.0, [&] {
                bool ok = false;
                OnGameThread([&] {
                    auto* pc = game::Controller();
                    ok = pc && game::Pawn() && game::IsInState(pc, "PlayerWalking") && !pc->bCinematicMode && !pc->bIgnoreMoveInput;
                }, 1.0);
                return ok;
            });
            if (!walking) p.Problem("after the restart, the player isn't back in PlayerWalking");
            Classes::FVector at{};
            std::string after;
            OnGameThread([&] {
                if (auto* pawn = game::Pawn()) at = pawn->Location;
                after = game::MapName();
            });
            const float off = std::hypot(at.X - spawn.X, at.Y - spawn.Y);
            s.notes.push_back(fmt::format("restarted from the checkpoint{}: {:.0f} units from the first spawn",
                                          g_loadsFinished.load() > loadsBefore ? " (a level load)" : "", off));
            if (!pawnAfter || pawnAfter == pawnBefore) p.Problem("Restart Checkpoint didn't respawn the player in MirageTestMap");
            else if (Lower(after) != "miragetestmap") p.Problem("Restart Checkpoint left MirageTestMap for " + after);
            else if (off > 150.f) p.Problem(fmt::format("the restart put the player {:.0f} units from the checkpoint's spawn", off));
        } else {
            p.Problem("the Restart Checkpoint module (mirage-levels) is missing");
        }
    }

    // Back to the test level for the steps after this one
    const std::string want = Lower(g_options.map);
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (Lower(maps[i].file) == want) {
            load(i, std::min(g_options.checkpoint, static_cast<int>(maps[i].checkpoints.size())), g_options.map);
            break;
        }
    End(s);
}

double GameSeconds();
bool HoldKey(const std::string& key, double seconds, const std::function<void()>& each = {});

// A map made with the game's own 2008 editor (step 13's milestone): tests/mods/map-test's maps/MirageEditorRoom.me1
// (a room, a light and a player start, built in UnrealEd: tests/maps-src/MirageEditorRoom) is in the level select and
// loads with the player standing in the room; walking forward (the forward key held for a second of game time) takes
// the player across its floor, not off it. The room has no checkpoints: the game's Restart Checkpoint needs one (without
// it, it leaves no player), so restarting it is loading its start again.
void TestEditorMap()
{
    Step& s = Begin("Mirage editor map");
    Probe p(s);
    const auto maps = levels::Maps();
    int index = -1;
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (maps[static_cast<std::size_t>(i)].file == "MirageEditorRoom") index = i;
    if (index < 0) {
        p.Problem("MirageEditorRoom isn't in the Levels list (is map-test loaded?)");
        End(s);
        return;
    }
    const auto loadMap = [&](int map, int checkpoint, const std::string& want) -> bool {
        const int before = g_loadsFinished.load();
        OnRenderThread([map, checkpoint] { levels::Load(map, checkpoint, levels::Method::Game, false); });
        if (!WaitFor(120.0, [&] { return g_loadsFinished.load() > before; })) { p.Problem(want + " didn't load"); return false; }
        if (!WaitFor(60.0, [] { return game::GetStatus().hasPawn && !game::IsLoading(); })) { p.Problem("no player in " + want); return false; }
        if (const std::string why = WaitPlayerReady(30.0); !why.empty()) { p.Problem("in " + want + ": " + why); return false; }
        return true;
    };
    // Where the player is, and whether on the floor (the room: |X|, |Y| < 1024, its floor at Z -96)
    struct Where { Classes::FVector at{}; bool walking = false; std::string map, text; };
    const auto where = [] {
        Where w;
        OnGameThread([&] {
            auto* pawn = game::Pawn();
            auto* pc = game::Controller();
            if (pawn) {
                w.at = pawn->Location;
                w.walking = pawn->Physics == Classes::EPhysics::PHYS_Walking;
            }
            w.map = game::MapName();
            w.text = pawn ? fmt::format("[{:.0f}, {:.0f}, {:.0f}] physics {} vz {:.0f}{}{}", pawn->Location.X, pawn->Location.Y,
                                        pawn->Location.Z, static_cast<int>(pawn->Physics.GetValue()), pawn->Velocity.Z,
                                        pc && game::IsInState(pc, "PlayerWalking") ? ", PlayerWalking" : "",
                                        pc && pc->bIgnoreMoveInput ? ", move input ignored" : "")
                          : std::string("no player");
        });
        return w;
    };
    const auto inRoom = [](const Classes::FVector& v) {
        return std::fabs(v.X) < 1024.f && std::fabs(v.Y) < 1024.f && v.Z > -96.f && v.Z < 704.f;
    };

    if (loadMap(index, 0, "MirageEditorRoom")) {
        Where start;
        WaitFor(10.0, [&] { start = where(); return start.walking; });
        spdlog::info("Self-test: editor map loaded, the player: {}", start.text);
        s.notes.push_back(fmt::format("loaded {}: the player at [{:.0f}, {:.0f}, {:.0f}]{}", start.map, start.at.X, start.at.Y,
                                      start.at.Z, start.walking ? ", on the floor" : ""));
        if (Lower(start.map) != "mirageeditorroom") p.Problem("the level loaded is " + start.map + ", not MirageEditorRoom");
        else if (!inRoom(start.at) || !start.walking) p.Problem("the player isn't standing in the editor's room");

        // Walked forward for a second of game time: somewhere else on the room's floor
        std::string forward;
        OnGameThread([&] { forward = KeyFor(game::Controller(), "GBA_MoveForward", "W"); });
        if (!HoldKey(forward, 1.0)) p.Problem("the forward key couldn't be pressed (no engine input)");
        const double settle = GameSeconds();
        while (!g_aborted && GameSeconds() - settle < 0.5) WaitTicks(1);
        const Where end = where();
        const float walked = std::hypot(end.at.X - start.at.X, end.at.Y - start.at.Y);
        s.notes.push_back(fmt::format("walked {:.0f} units across the room", walked));
        if (walked < 100.f) p.Problem(fmt::format("walking forward moved the player only {:.0f} units", walked));
        else if (!inRoom(end.at) || !end.walking) p.Problem("the walk didn't end on the room's floor: " + end.text);
    }

    // Back to the test level
    const std::string want = Lower(g_options.map);
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (Lower(maps[static_cast<std::size_t>(i)].file) == want) {
            loadMap(i, std::min(g_options.checkpoint, static_cast<int>(maps[static_cast<std::size_t>(i)].checkpoints.size())), g_options.map);
            break;
        }
    End(s);
}

// World access (step 14), with tests/mods/world-access (Lua): an actor (a DynamicSMActor_Spawnable) spawned with the Tag
// "WorldGravityZ" (a name the game has, no class's) comes to the
// mod as actor_added (its class with it); the mod finds it by tag and around the player (the player first: nearest
// first) and an engine class's actors; destroyed, it comes as actor_removed equal (==) to the one the mod kept, which is
// no longer valid and reads as nothing. Handles (object_handle / handle_object): valid while the object lives, invalid
// once it is destroyed, and a handle naming another object at the same address is refused. A checkpoint restart comes
// as restart, and a handle of the old player is invalid after it. The comparison's cost is in the notes.
std::string WorldShared(const char* key)
{
    char v[256]{};
    mirage::mods::game_api::ShareGet("world-access", key, v, sizeof v);
    return v;
}
float WorldSharedNum(const char* key) { return util::ParseFloatOr(WorldShared(key), -1.f); }

// Step 14 from C# (tests/mods/world-cs): the same probe as the Lua step (an actor tagged "WorldGravityZ" beside the player)
// comes to the C# mod as ActorAdded with its class; Mod.Actors finds it by tag (alone), around the player (the player
// first) and by class, a missing class matches nothing; destroyed, ActorRemoved's object equals the kept one and the kept
// handle no longer resolves; a checkpoint restart comes as Restart. The mod reports in its module's settings.
void TestWorldAccessCs()
{
    Step& s = Begin("Mirage world access (C#)");
    Probe p(s);
    Module* m = modules::Find("World access (C#)");
    if (!m) {
        p.Problem("the World access (C#) module is missing (tests/mods/world-cs: compiled in the game, its log says why)");
        End(s);
        return;
    }
    const auto num = [m](const char* name) {
        auto* v = dynamic_cast<ctl::SliderInt*>(m->FindSetting(name));
        return v ? v->Get() : -999;
    };
    OnRenderThread([m] { m->SetEnabled(true); });
    WaitTicks(5);

    Classes::AActor* probe = nullptr;
    OnGameThread([&] {
        auto* pawn = game::Pawn();
        if (!pawn) return;
        Classes::FVector at = pawn->Location;
        at.X += 200.f;
        probe = pawn->Spawn(Classes::ADynamicSMActor_Spawnable::StaticClass(), nullptr, Classes::FName{}, at, pawn->Rotation, nullptr, true);
        if (probe) probe->Tag = Classes::FName("WorldGravityZ");
    });
    if (!probe) {
        p.Problem("the probe actor didn't spawn");
        End(s);
        return;
    }
    if (!WaitFor(10.0, [&] { return num("Queried") == 1; })) p.Problem("the C# mod never queried (no ActorAdded with the probe)");
    if (num("Probe class ok") != 1) p.Problem("ActorAdded didn't bring the probe with its class (DynamicSMActor_Spawnable)");
    if (num("Query tag") != 1) p.Problem(fmt::format("Actors(tag:) didn't find the probe alone ({})", num("Query tag")));
    if (num("Query near") != 1) p.Problem("Actors(near: player, radius:) didn't list the player first with the probe among them");
    if (num("Query class") < 1) p.Problem("Actors(className: \"Engine.PlayerStart\") found none");
    if (num("Query missing") != 0) p.Problem("a class that doesn't exist matched actors");

    OnGameThread([&] { if (probe && !probe->bDeleteMe) probe->Destroy(); });
    if (!WaitFor(5.0, [&] { return num("Probe removed") == 1; })) p.Problem("ActorRemoved didn't come equal to the probe the mod kept");
    else if (num("Handle after") != 0) p.Problem("the kept probe's handle still resolved after it was destroyed");

    const int restarts = num("Restarts");
    const void* pawnBefore = nullptr;
    OnGameThread([&] {
        pawnBefore = game::Pawn();
        levels::RestartFromLastCheckpoint();
    });
    WaitFor(30.0, [&] {
        const void* now = nullptr;
        OnGameThread([&] { now = game::Pawn(); }, 1.0);
        return now && now != pawnBefore && !game::IsLoading();
    });
    if (!WaitFor(10.0, [&] { return num("Restarts") > restarts; })) p.Problem("no Restart event after the checkpoint restart");
    WaitPlayerReady(10.0);
    s.notes.push_back(fmt::format("{} actors added, {} removed, PlayerStarts {}, restarts {}", num("Added"), num("Removed"),
                                  num("Query class"), num("Restarts")));
    OnRenderThread([m] { m->SetEnabled(false); });
    p.Check(1, true);
    End(s);
}

void TestWorldAccess()
{
    Step& s = Begin("Mirage world access");
    Probe p(s);
    namespace ga = mirage::mods::game_api;
    Module* m = modules::Find("World access (Lua)");
    if (!m) {
        p.Problem("the World access (Lua) module is missing (tests/mods/world-access)");
        End(s);
        return;
    }
    OnRenderThread([m] { m->SetEnabled(true); });
    WaitTicks(5);

    // 1. Spawned: an Actor with a tag, beside the player
    Classes::AActor* probe = nullptr;
    MirageHandle h{};
    h.size = sizeof h;
    OnGameThread([&] {
        auto* pawn = game::Pawn();
        if (!pawn) return;
        Classes::FVector at = pawn->Location;
        at.X += 200.f;
        probe = pawn->Spawn(Classes::ADynamicSMActor_Spawnable::StaticClass(), nullptr, Classes::FName{}, at, pawn->Rotation, nullptr, true);
        if (!probe) return;
        probe->Tag = Classes::FName("WorldGravityZ");   // A name the game has (FName only finds names)
        ga::ObjectHandle(reinterpret_cast<MirageObject*>(probe), &h);
    });
    if (!probe) {
        p.Problem("the probe actor didn't spawn");
        End(s);
        return;
    }
    WaitFor(5.0, [] { return WorldSharedNum("queried") == 1.f; });      // Set last, after every query
    if (WorldShared("probe_class") != "DynamicSMActor_Spawnable") p.Problem("actor_added didn't bring the probe with its class (got '" + WorldShared("probe_class") + "')");
    if (WorldSharedNum("probe_valid") != 1.f) p.Problem("the probe wasn't valid in its actor_added event");
    if (WorldSharedNum("query_tag") != 1.f || WorldSharedNum("query_tag_is_probe") != 1.f)
        p.Problem(fmt::format("actors{{tag}} found {} actor(s), not the probe alone", WorldShared("query_tag")));
    if (WorldSharedNum("query_near_has_probe") != 1.f || WorldSharedNum("query_near_sorted") != 1.f)
        p.Problem("actors{near = pawn, radius} didn't list them nearest first with the player and the probe among them");
    if (WorldSharedNum("query_class") < 1.f) p.Problem("actors{class = \"Engine.PlayerStart\"} found none");
    if (WorldSharedNum("query_missing") != 0.f) p.Problem("a class that doesn't exist matched actors");

    // 2. Handles: valid now; another object at that address (a different name) is refused
    MirageObject* back = nullptr;
    MirageObject* forged = nullptr;
    OnGameThread([&] {
        back = ga::HandleObject(&h);
        MirageHandle other = h;
        ++other.name_number;
        forged = ga::HandleObject(&other);
    });
    if (back != reinterpret_cast<MirageObject*>(probe)) p.Problem("the probe's handle didn't give the probe back");
    if (forged) p.Problem("a handle with another name at the same address gave an object");

    // 3. Destroyed: actor_removed is the kept one, which is no longer valid; the handle is invalid too
    const float removedBefore = WorldSharedNum("removed");
    OnGameThread([&] { if (probe && !probe->bDeleteMe) probe->Destroy(); });
    WaitFor(5.0, [] { return WorldSharedNum("probe_removed") == 1.f; });
    if (WorldSharedNum("probe_removed") != 1.f) p.Problem("actor_removed didn't come equal (==) to the probe the mod kept");
    if (WorldSharedNum("kept_valid_after") != 0.f) p.Problem("the kept probe was still valid after it was destroyed");
    if (WorldShared("kept_z_after") != "nil") p.Problem("the destroyed probe read " + WorldShared("kept_z_after") + ", not nothing");
    if (WorldSharedNum("removed") <= removedBefore) p.Problem("no actor_removed counted");
    MirageObject* after = reinterpret_cast<MirageObject*>(1);
    WaitTicks(3);
    OnGameThread([&] { after = ga::HandleObject(&h); });
    if (after) p.Problem("the destroyed probe's handle still gave an object");

    // 4. A checkpoint restart: the restart event; the old player's handle is invalid after it
    MirageHandle oldPawn{};
    oldPawn.size = sizeof oldPawn;
    const float restartsBefore = WorldSharedNum("restarts");
    const void* pawnBefore = nullptr;
    OnGameThread([&] {
        pawnBefore = game::Pawn();
        ga::ObjectHandle(reinterpret_cast<MirageObject*>(game::Pawn()), &oldPawn);
        levels::RestartFromLastCheckpoint();
    });
    WaitFor(30.0, [&] {
        const void* now = nullptr;
        OnGameThread([&] { now = game::Pawn(); }, 1.0);
        return now && now != pawnBefore && !game::IsLoading();
    });
    WaitFor(10.0, [&] { return WorldSharedNum("restarts") > restartsBefore; });
    MirageObject* oldBack = reinterpret_cast<MirageObject*>(1);
    OnGameThread([&] { oldBack = ga::HandleObject(&oldPawn); });
    if (WorldSharedNum("restarts") <= restartsBefore) p.Problem("no restart event after the checkpoint restart");
    // level_streamed: layer-test's own sublevel (LayerTestArea) hidden, then shown again, through the engine's streaming
    {
        const float streamedBefore = WorldSharedNum("streamed");
        const auto show = [](bool visible) {
            bool found = false;
            OnGameThread([&] {
                auto* world = static_cast<Classes::AWorldInfo*>(game::World());
                if (!world) return;
                for (std::size_t i = 0; i < world->StreamingLevels.Num(); ++i)
                    if (Classes::ULevelStreaming* l = world->StreamingLevels[i]; l && l->PackageName.GetName() == "LayerTestArea") {
                        l->bShouldBeVisible = visible;
                        found = true;
                    }
            });
            return found;
        };
        if (!show(false)) p.Problem("LayerTestArea (layer-test's sublevel) isn't streamed in: level_streamed can't be checked");
        else {
            const bool out = WaitFor(10.0, [&] { return WorldShared("last_streamed") == "out LayerTestArea"; });
            show(true);
            const bool in = WaitFor(10.0, [&] { return WorldShared("last_streamed") == "in LayerTestArea"; });
            if (!out) p.Problem("no level_streamed (hidden) when LayerTestArea was hidden");
            else if (!in) p.Problem("no level_streamed (shown) when LayerTestArea was shown again");
            else s.notes.push_back(fmt::format("level_streamed: LayerTestArea out and in ({} events)", WorldSharedNum("streamed") - streamedBefore));
        }
    }

    // The engine's player state hook: a new move (a fall from a little above the floor) comes to mods as player_state
    WaitPlayerReady(10.0);
    const float statesBefore = WorldSharedNum("player_states");
    OnGameThread([] {
        auto* pawn = game::Pawn();
        if (!pawn) return;
        pawn->Location.Z += 150.f;
        pawn->Velocity = Classes::FVector{};
        pawn->Physics = Classes::EPhysics::PHYS_Falling;
        mirage::engine::SetMove(pawn, Classes::EMovement::MOVE_Falling);
    });
    WaitFor(3.0, [&] { return WorldSharedNum("player_states") > statesBefore; });
    if (WorldSharedNum("player_states") <= statesBefore) p.Problem("no player_state event after a new move (the engine's state handler hook)");
    WaitGrounded();
    if (oldBack) p.Problem("the old player's handle still gave an object after the restart");

    const auto st = mirage::world_events::GetStats();
    const std::uint64_t unchanged = st.compares - st.changed;
    s.notes.push_back(fmt::format("{} actors in play; compared on {} ticks, {} with a change: {:.3f} ms a tick on average, "
                                  "an unchanged tick {:.1f} us, max {:.3f} ms (a restart's); seen: {:.0f} added, {:.0f} removed, "
                                  "{:.0f} restart(s), {:.0f} streamed",
                                  st.actors, st.compares, st.changed, st.compares ? st.totalCompareMs / st.compares : 0.0,
                                  unchanged ? st.unchangedCompareMs * 1000.0 / static_cast<double>(unchanged) : 0.0,
                                  st.maxCompareMs, WorldSharedNum("added"), WorldSharedNum("removed"), WorldSharedNum("restarts"),
                                  WorldSharedNum("streamed")));
    OnRenderThread([m] { m->SetEnabled(false); });
    WaitGrounded();
    End(s);
}

// Reversible changes (step 15), with tests/mods/changes-lua: its transaction halves the level's gravity (written twice:
// the first old value is kept) and lowers KillZ, and a write outside it moves StallZ. The mod manager's list has the
// transaction with its 2 values. Off, the mod reverts: gravity and KillZ exactly as before, StallZ still moved. On again,
// then a hot reload: unloading puts the transaction back by itself, so the mod's new code halves the level's own gravity
// once (0.5x, not 0.25x). Off at the end: the level as it was.
float ChangesShared(const char* key)
{
    char v[64]{};
    mirage::mods::game_api::ShareGet("changes-lua", key, v, sizeof v);
    return util::ParseFloatOr(v, -1.f);
}

void TestChangeLog()
{
    Step& s = Begin("Mirage change log");
    Probe p(s);
    Module* m = modules::Find("Changes (Lua)");
    if (!m) {
        p.Problem("the Changes (Lua) module is missing (tests/mods/changes-lua)");
        End(s);
        return;
    }
    struct World { float gravity = 0.f, killZ = 0.f, stallZ = 0.f; };
    const auto read = [] {
        World w;
        OnGameThread([&] {
            if (auto* world = static_cast<Classes::AWorldInfo*>(game::World())) {
                w.gravity = world->WorldGravityZ;
                w.killZ = world->KillZ;
                w.stallZ = world->StallZ;
            }
        });
        return w;
    };
    OnRenderThread([m] { m->SetEnabled(false); });
    WaitTicks(3);
    const World before = read();

    // 1. The transaction, and the write outside it
    const float idBefore = ChangesShared("id");
    OnRenderThread([m] { m->SetEnabled(true); });
    WaitFor(5.0, [&] { return ChangesShared("id") > idBefore; });
    const World on = read();
    if (on.gravity != before.gravity * 0.5f || on.killZ != before.killZ - 1000.f || on.stallZ != before.stallZ + 1.f)
        p.Problem(fmt::format("the mod's writes didn't land: gravity {} -> {}, KillZ {} -> {}, StallZ {} -> {}", before.gravity,
                              on.gravity, before.killZ, on.killZ, before.stallZ, on.stallZ));
    const auto list = mirage::changes::List("changes-lua");
    if (list.empty() || list.back().paths.size() != 2)
        p.Problem(fmt::format("the mod manager's list has {} transaction(s), the last with {} value(s), not 2", list.size(),
                              list.empty() ? 0 : list.back().paths.size()));

    // 2. Off: the mod reverts; the write outside stays
    OnRenderThread([m] { m->SetEnabled(false); });
    WaitFor(5.0, [] { return ChangesShared("reverted") >= 2.f; });
    const World off = read();
    if (off.gravity != before.gravity || off.killZ != before.killZ)
        p.Problem(fmt::format("revert left gravity {} (was {}), KillZ {} (was {})", off.gravity, before.gravity, off.killZ, before.killZ));
    if (off.stallZ != before.stallZ + 1.f) p.Problem("the write outside the transaction was put back too");

    // 3. On again, and a hot reload: the unload puts it back, the new code applies it once
    const float idOn = ChangesShared("id");
    OnRenderThread([m] { m->SetEnabled(true); });
    WaitFor(5.0, [&] { return ChangesShared("id") > idOn; });
    const float idReload = ChangesShared("id");
    std::string reload;
    OnGameThread([&] { reload = mirage::mods::Reload("changes-lua"); }, 10.0);
    if (!reload.empty()) p.Problem("hot reload: " + reload);
    WaitFor(10.0, [&] { return ChangesShared("id") > idReload; });
    const World reloaded = read();
    if (reloaded.gravity != before.gravity * 0.5f)
        p.Problem(fmt::format("after the hot reload the gravity is {} ({}x the level's): the unload didn't put it back",
                              reloaded.gravity, reloaded.gravity / before.gravity));

    // 4. Off: the level as it was (and StallZ, the write outside, by hand)
    OnRenderThread([m] { m->SetEnabled(false); });
    WaitFor(5.0, [&] { return read().gravity == before.gravity; });
    const World end = read();
    OnGameThread([&] {
        if (auto* world = static_cast<Classes::AWorldInfo*>(game::World())) world->StallZ = before.stallZ;
    });
    if (end.gravity != before.gravity || end.killZ != before.killZ) p.Problem("switched off at the end, the level isn't as it was");
    s.notes.push_back(fmt::format("gravity {} -> {} -> {} (reverted), 0.5x again after a hot reload; KillZ {} -> {} -> {}; "
                                  "StallZ moved outside the transaction and kept; the mod manager lists the transaction "
                                  "with {} values", before.gravity, on.gravity, off.gravity, before.killZ, on.killZ, off.killZ,
                                  list.empty() ? 0 : list.back().paths.size()));
    End(s);
}

float StandingJump();

// The game's own clock (WorldInfo.TimeSeconds): what a wait in game time measures. -1 without a world.
double GameSeconds()
{
    double t = -1.0;
    OnGameThread([&] {
        if (auto* w = static_cast<Classes::AWorldInfo*>(game::World())) t = w->TimeSeconds;
    }, 1.0);
    return t;
}

// A key pressed or let go as if on the keyboard, through the engine's input (the loader's input service): the player's
// own bindings decide what it does. False if the engine's input isn't there.
bool KeyEvent(const std::string& key, bool down)
{
    bool ok = false;
    OnGameThread([&] {
        const Classes::FName name(key.c_str());
        tas::engine::RawInput in;
        in.nameIndex = name.Index;
        in.nameNumber = name.Number;
        in.event = down ? 0 : 1;
        in.value = 1.f;
        ok = name.Index != 0 && tas::engine::Inject(in);
    });
    return ok;
}

// Holds `key` for `seconds` of game time (the self-test runs uncapped: a count of ticks is almost no game time),
// calling `each` on the game thread every tick meanwhile. False if the key couldn't be pressed.
bool HoldKey(const std::string& key, double seconds, const std::function<void()>& each)
{
    if (!KeyEvent(key, true)) return false;
    const double start = GameSeconds();
    while (!g_aborted) {
        if (each) OnGameThread(each, 1.0);
        const double now = GameSeconds();
        if (now < 0.0 || now - start >= seconds) break;
        WaitTicks(1);
    }
    KeyEvent(key, false);
    return true;
}

// A standing jump: how high the player gets above where it started (-1: it couldn't be done)
float StandingJump()
{
    std::string jumpKey;
    OnGameThread([&] { jumpKey = KeyFor(game::Controller(), "GBA_Jump", "SpaceBar"); });
    WaitGrounded();
    float startZ = 0.f, topZ = 0.f;
    bool ok = false;
    OnGameThread([&] {
        if (auto* pawn = game::Pawn()) startZ = topZ = pawn->Location.Z, ok = true;
    });
    if (!ok) return -1.f;
    const auto sample = [&] {
        if (auto* pawn = game::Pawn()) topZ = std::max(topZ, pawn->Location.Z);
    };
    // Pressed briefly (a tap), then watched for the rest of a jump's time in the air
    if (!HoldKey(jumpKey, 0.05, sample)) return -1.f;
    const double start = GameSeconds();
    while (!g_aborted && GameSeconds() - start < 1.5) {
        OnGameThread(sample, 1.0);
        WaitTicks(1);
    }
    WaitGrounded();
    return topZ - startZ;
}

// Script injection (step 11), the milestone: two Lua mods (tests/mods/inject-a, inject-b) inject at one place in the
// middle of TdGame.TdMove_Jump.StartMove. inject-a (priority 10) sets the local CurrentWeaponType to 1 before the branch
// that picks BaseJumpZHeavy over BaseJumpZ, so a standing jump changes height; inject-b (priority 0) runs after it and
// sees the 1. A hot reload of inject-a takes its injection out and puts it back; with both off the function is its
// original bytecode again, byte for byte. Higher Jumps (a script patch of the same function) goes under the injections.
void TestScriptInjectionMods()
{
    Step& s = Begin("Mirage script injection (Lua)");
    Probe p(s);
    namespace mm = mirage::mods;
    Module* a = modules::Find("Inject A (Lua)");
    Module* b = modules::Find("Inject B (Lua)");
    Module* higher = modules::Find("Higher Jumps");
    Classes::UStruct* fn = nullptr;
    OnGameThread([&] { fn = mirage::scripts::Find("TdGame.TdMove_Jump.StartMove"); });
    if (!a || !b || !fn) {
        p.Problem(!fn ? "TdGame.TdMove_Jump.StartMove wasn't found" : "the Inject A / Inject B (Lua) modules are missing (tests/mods)");
        End(s);
        return;
    }
    const auto bytes = [&] {
        std::vector<std::uint8_t> out;
        OnGameThread([&] {
            const auto* data = *reinterpret_cast<std::uint8_t* const*>(&fn->Script);
            const std::int32_t count = *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(&fn->Script) + 4);
            out.assign(data, data + count);
        });
        return out;
    };
    const auto intOf = [](Module* m, const char* name) {
        auto* v = dynamic_cast<ctl::SliderInt*>(m->FindSetting(name));
        return v ? v->Get() : -2;
    };
    const auto setOn = [&](Module* m, bool on) {
        OnRenderThread([m, on] { m->SetEnabled(on); });
        WaitTicks(10);		// They inject or remove on their next tick
    };
    const bool higherWas = higher && higher->Enabled();
    if (higher) setOn(higher, false);
    setOn(a, false);
    setOn(b, false);
    OnGameThread([] { player_state::Save(); });

    const std::vector<std::uint8_t> original = bytes();
    // Every jump from the same place (a standing jump's height depends on the floor under it: after Higher Jumps' jump
    // the player lands elsewhere): the state saved above, put back before each
    const auto jumpFromStart = [] {
        OnGameThread([] { player_state::Load(); });
        WaitGrounded();
        return StandingJump();
    };
    const float normal = jumpFromStart();

    setOn(b, true);
    setOn(a, true);
    if (bytes() == original) p.Problem("with both on, StartMove's bytecode is unchanged (the mods' logs say why)");
    // A module's change lands on one of its ticks, which the scheduler may defer: wait for the change itself (who
    // patched StartMove, which injections it has; a rebuild numbers its sites anew, so the bytes differ each time)
    const auto patchedBy = [&] {
        std::string owner;
        OnGameThread([&] { owner = mirage::scripts::Owner(fn); });
        return owner;
    };
    const auto injectedBy = [](const char* mod) {
        for (const auto& i : mirage::inject::List())
            if (i.owner == mod && i.function.find("StartMove") != std::string::npos && i.places > 0) return true;
        return false;
    };
    if (auto* seen = dynamic_cast<ctl::SliderInt*>(b->FindSetting("Seen"))) OnRenderThread([seen] { seen->Set(-1); });
    const int runsBefore = intOf(a, "Runs");
    const float heavy = jumpFromStart();
    if (intOf(a, "Runs") <= runsBefore) p.Problem("inject-a's callback didn't run during the jump");
    if (intOf(b, "Seen") != 1) p.Problem("inject-b saw CurrentWeaponType " + std::to_string(intOf(b, "Seen")) + ", not inject-a's 1 (order)");

    // Higher Jumps patches the same function: under the injections, both apply
    float both = -1.f;
    if (higher) {
        setOn(higher, true);
        if (!WaitFor(10.0, [&] { return patchedBy() == "higher-jumps"; })) p.Problem("Higher Jumps didn't patch under the injections");
        both = jumpFromStart();
        setOn(higher, false);
        if (!WaitFor(10.0, [&] { return patchedBy() != "higher-jumps"; })) p.Problem("Higher Jumps' patch stayed after it went off");
    }

    // Hot reload of inject-a: its injection out, the mod loaded again, its injection back on its next tick
    std::string reload = "timed out";
    OnGameThread([&] { reload = mm::Reload("inject-a"); }, 15.0);
    if (!reload.empty()) p.Problem("hot reload of inject-a: " + reload);
    Module* a2 = modules::Find("Inject A (Lua)");
    if (a2 && !a2->Enabled()) setOn(a2, true);
    if (!WaitFor(10.0, [&] { return injectedBy("inject-a"); })) p.Problem("inject-a's injection didn't come back after its reload");
    if (auto* seen = dynamic_cast<ctl::SliderInt*>(b->FindSetting("Seen"))) OnRenderThread([seen] { seen->Set(-1); });
    const float reloaded = jumpFromStart();
    if (intOf(b, "Seen") != 1) p.Problem("after inject-a's reload inject-b saw " + std::to_string(intOf(b, "Seen")) + ", not 1");

    setOn(a2 ? a2 : a, false);
    setOn(b, false);
    if (!WaitFor(10.0, [&] { return bytes() == original; })) p.Problem("with both off again, StartMove isn't its original bytecode");
    const float after = jumpFromStart();
    OnGameThread([] { player_state::Load(); });
    WaitGrounded();
    if (higher && higherWas) setOn(higher, true);

    char line[240];
    std::snprintf(line, sizeof line, "standing jump: %.0f, heavy by injection %.0f, with Higher Jumps under it %.0f, after inject-a's "
                  "reload %.0f, both off %.0f", normal, heavy, both, reloaded, after);
    s.notes.push_back(line);
    if (normal <= 10.f) p.Problem("the player didn't jump");
    else {
        if (std::fabs(heavy - normal) < normal * 0.05f) p.Problem("the injected local didn't change the jump (" + std::string(line) + ")");
        // The injection back after the reload: the jump is the heavy one, not the normal one. Its exact height varies
        // more than the first heavy jump's (42-58 against 58 in runs), so the check is which of the two it is.
        if (std::fabs(reloaded - heavy) >= std::fabs(reloaded - normal)) p.Problem("after the reload the jump isn't the injected one (" + std::string(line) + ")");
        if (std::fabs(after - normal) > normal * 0.1f) p.Problem("with both off the jump isn't the normal one");
        if (higher && both < heavy * 1.6f) p.Problem("Higher Jumps under the injections didn't raise the injected jump");
    }
    End(s);
}

// Script injection (step 11), the mechanism: Core.Object.FPctByRange (Value - InMin) / (InMax - InMin) called directly.
// An injection at its head changes a parameter, one returns early with its own value, two at one place run by priority,
// and with none left the bytecode is the original again, byte for byte. Then what an injection costs a call, next to a
// function hook doing the same nothing.
void TestScriptInjection()
{
    Step& s = Begin("Mirage script injection");
    Probe p(s);
    namespace inj = mirage::inject;
    std::string why = "not run";
    bool ready = false;
    OnGameThread([&] { ready = inj::Ready(&why); });
    if (!ready) {
        p.Problem("injection isn't available: " + why);
        End(s);
        return;
    }
    const char* kFunction = "Core.Object.FPctByRange";
    Classes::UStruct* fn = nullptr;
    const auto bytes = [&] {
        std::vector<std::uint8_t> out;
        OnGameThread([&] {
            const auto* data = *reinterpret_cast<std::uint8_t* const*>(&fn->Script);
            const std::int32_t count = *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const std::uint8_t*>(&fn->Script) + 4);
            out.assign(data, data + count);
        });
        return out;
    };
    OnGameThread([&] {
        fn = mirage::scripts::Find(kFunction);
        // What the Lua milestone works on: its tokens in the log
        if (auto* jumpFn = mirage::scripts::Find("TdGame.TdMove_Jump.StartMove")) {
            UnrealLib::Script sc;
            std::string e;
            if (mirage::scripts::Read(jumpFn, sc, &e)) spdlog::info("Self-test: StartMove:\n{}", mirage::scripts::Dump(sc));
        }
    });
    if (!fn) {
        p.Problem(std::string(kFunction) + " wasn't found");
        End(s);
        return;
    }
    const std::vector<std::uint8_t> original = bytes();
    // FPctByRange(v, 0, 10) on any object (it is static): v / 10
    const auto pct = [&](float value) {
        float out = -1.f;
        OnGameThread([&] {
            auto* pawn = game::Pawn();
            if (!pawn) return;
            mirage::call::Params prm(pawn, "FPctByRange");
            prm.Set("Value", value);
            prm.Set("InMin", 0.f);
            prm.Set("InMax", 10.f);
            if (mirage::call::Call(pawn, prm)) out = prm.GetFloat("ReturnValue");
        });
        return out;
    };
    const float plainResult = pct(5.f);
    if (std::fabs(plainResult - 0.5f) > 1e-4f) p.Problem("FPctByRange(5, 0, 10) isn't 0.5 before any injection: " + std::to_string(plainResult));

    struct Ctx
    {
        std::vector<int>* order;
        int tag;
    };
    static std::vector<int> order;
    static Ctx high{&order, 1}, low{&order, 2}, early{&order, 3};
    const auto setValue = [](void* user, mirage::hooks::Call& call) {
        auto* c = static_cast<Ctx*>(user);
        c->order->push_back(c->tag);
        if (auto v = call.Param("Value")) *reinterpret_cast<float*>(v.address) = 9.f;
    };
    const auto record = [](void* user, mirage::hooks::Call&) {
        auto* c = static_cast<Ctx*>(user);
        c->order->push_back(c->tag);
    };
    const auto returnEarly = [](void* user, mirage::hooks::Call& call) {
        auto* c = static_cast<Ctx*>(user);
        c->order->push_back(c->tag);
        if (auto r = call.Param("ReturnValue")) *reinterpret_cast<float*>(r.address) = 42.f;
        call.skip = true;
    };

    // 1. Two at the head, by priority: the high one changes Value (5 -> 9), the low one runs after it
    int a = 0, b = 0, c = 0;
    std::string err;
    OnGameThread([&] {
        b = inj::Add(kFunction, "head", 0, +record, &low, "self-test", &err);
        a = inj::Add(kFunction, "head", 10, +setValue, &high, "self-test", &err);
    });
    if (!a || !b) p.Problem("injecting at the head: " + err);
    order.clear();
    const float changed = pct(5.f);
    if (std::fabs(changed - 0.9f) > 1e-4f) p.Problem("a parameter changed at the head didn't change the result: " + std::to_string(changed));
    if (order != std::vector<int>{1, 2}) p.Problem("the two injections didn't run by priority (high first)");
    // 2. One that returns early with its own value, before every return
    OnGameThread([&] { c = inj::Add(kFunction, "return", 0, +returnEarly, &early, "self-test", &err); });
    if (!c) p.Problem("injecting before the returns: " + err);
    const float returned = pct(5.f);
    if (std::fabs(returned - 42.f) > 1e-4f) p.Problem("an early return didn't return its value: " + std::to_string(returned));
    // 3. A point that matches nothing is refused and changes nothing
    int none = 0;
    OnGameThread([&] { none = inj::Add(kFunction, "before:NoSuchFunctionAnywhere", 0, +record, &low, "self-test", &err); });
    if (none) p.Problem("a point that matches nothing was accepted");
    // 4. All out: the original, byte for byte
    OnGameThread([&] {
        inj::Remove(c);
        inj::Remove(a);
        inj::Remove(b);
    });
    if (bytes() != original) p.Problem("with the injections removed the bytecode isn't the original");
    const float again = pct(5.f);
    if (std::fabs(again - 0.5f) > 1e-4f) p.Problem("with the injections removed FPctByRange isn't 0.5 again: " + std::to_string(again));
    s.notes.push_back("FPctByRange: 2 at the head by priority, an early return, all removed: the original " +
                      std::to_string(original.size()) + " bytes");

    // 5. What a call costs: plain, with an injection that does nothing, with a hook that does nothing
    const auto empty = [](void*, mirage::hooks::Call&) {};
    const auto timeCalls = [&] {
        double best = 1e30;
        for (int round = 0; round < 3; ++round)
            OnGameThread([&] {
                auto* pawn = game::Pawn();
                if (!pawn) return;
                mirage::call::Params prm(pawn, "FPctByRange");
                prm.Set("Value", 5.f);
                prm.Set("InMax", 10.f);
                const auto t0 = std::chrono::steady_clock::now();
                for (int i = 0; i < 2000; ++i) mirage::call::Call(pawn, prm);
                best = std::min(best, std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 2000.0);
            });
        return best;
    };
    const double plain = timeCalls();
    int ci = 0;
    OnGameThread([&] { ci = inj::Add(kFunction, "head", 0, +empty, nullptr, "self-test", &err); });
    const double injected = timeCalls();
    OnGameThread([&] { inj::Remove(ci); });
    const int hi = mirage::hooks::Add(kFunction, mirage::hooks::When::Before, 0, +empty, nullptr, &err);
    const double hooked = timeCalls();
    mirage::hooks::Remove(hi);
    const double injCost = injected - plain, hookCost = hooked - plain;
    char line[200];
    std::snprintf(line, sizeof line, "a call: %.2f us; an empty injection adds %.2f us, an empty hook %.2f us", plain, injCost, hookCost);
    s.notes.push_back(line);
    if (injCost > std::max(2.0 * hookCost, 0.5)) p.Problem(std::string("an injection costs more than twice a hook: ") + line);
    if (bytes() != original) p.Problem("after the cost test the bytecode isn't the original");

    // 6. Mod natives: "float(float)" doubles what FPctByRange returns (a script patch wraps its return value in the
    //    native), and "int(bool, byte, object, name)" called at its head receives True, 7, Self and a name
    namespace nat = mirage::natives;
    struct Got
    {
        int calls = 0;
        std::vector<MirageValue> args;
    };
    static Got got;
    got = {};
    const auto twice = [](void*, nat::Call& call) {
        call.result.type = MIRAGE_VALUE_FLOAT;
        call.result.f = call.args.empty() ? 0.f : call.args[0].f * 2.f;
    };
    const auto keep = [](void*, nat::Call& call) {
        ++got.calls;
        got.args = call.args;
        call.result.type = MIRAGE_VALUE_INT;
        call.result.i = 5;
    };
    std::string nerr;
    const int nTwice = nat::Register("self-test", "twice", "float(float)", +twice, nullptr, &nerr);
    const int nKeep = nat::Register("self-test", "keep", "int(bool, byte, object, name)", +keep, nullptr, &nerr);
    const int nAgain = nat::Register("self-test", "twice", "float(float)", +twice, nullptr, &nerr);
    if (!nTwice || !nKeep) p.Problem("registering natives: " + nerr);
    else {
        if (nAgain != nTwice) p.Problem("registering the same native again gave another index");
        if (nat::Register("self-test", "twice", "int(int)", +twice, nullptr, &nerr)) p.Problem("a native's signature changed in a session");
        bool applied = false;
        Classes::UObject* pawnSeen = nullptr;
        OnGameThread([&] {
            pawnSeen = game::Pawn();
            UnrealLib::Script sc;
            if (!mirage::scripts::Read(fn, sc, &nerr)) return;
            using UnrealLib::TokenKind;
            for (const auto st : std::vector<std::uint32_t>(sc.statements)) {
                auto& ret = sc.tokens[st];
                if (ret.kind != TokenKind::Return || ret.children.empty() || sc.tokens[ret.children[0]].kind == TokenKind::Nothing) continue;
                UnrealLib::Token call;
                call.kind = TokenKind::NativeFunction;
                call.native_index = static_cast<std::uint16_t>(nTwice);
                call.children = {ret.children[0]};
                sc.tokens.push_back(call);
                sc.tokens[st].children = {static_cast<std::uint32_t>(sc.tokens.size() - 1)};
                break;
            }
            // keep(True, 7, Self, 'FPctByRange') as the first statement
            const auto add = [&](UnrealLib::Token t) {
                for (std::size_t op = 0; op < mirage::scripts::Map().kinds.size(); ++op)
                    if (mirage::scripts::Map().kinds[op] == t.kind) t.opcode = static_cast<std::uint8_t>(op);
                sc.tokens.push_back(t);
                return static_cast<std::uint32_t>(sc.tokens.size() - 1);
            };
            UnrealLib::Token tTrue, tByte, tSelf, tName, tKeep;
            tTrue.kind = TokenKind::True;
            tByte.kind = TokenKind::ByteConst;
            tByte.byte_value = 7;
            tSelf.kind = TokenKind::Self;
            tName.kind = TokenKind::NameConst;
            tName.name.index = fn->Name.Index;
            tName.name.number = fn->Name.Number;
            tKeep.kind = TokenKind::NativeFunction;
            tKeep.native_index = static_cast<std::uint16_t>(nKeep);
            tKeep.children = {add(tTrue), add(tByte), add(tSelf), add(tName)};
            sc.statements.insert(sc.statements.begin(), sc.tokens.size());
            sc.tokens.push_back(tKeep);
            applied = mirage::scripts::Apply(fn, sc, "self-test", &nerr);
        });
        if (!applied) p.Problem("patching FPctByRange with the natives: " + nerr);
        const float doubled = pct(5.f);
        OnGameThread([&] { mirage::scripts::Restore(fn); });
        if (std::fabs(doubled - 1.f) > 1e-4f) p.Problem("FPctByRange through the native 'twice' returned " + std::to_string(doubled) + ", not 1");
        if (got.calls != 1 || got.args.size() != 4) p.Problem("the native 'keep' ran " + std::to_string(got.calls) + " times");
        else {
            const auto& g = got.args;
            if (g[0].type != MIRAGE_VALUE_BOOL || g[0].i != 1) p.Problem("keep's bool argument isn't True");
            if (g[1].i != 7) p.Problem("keep's byte argument is " + std::to_string(g[1].i) + ", not 7");
            if (reinterpret_cast<Classes::UObject*>(g[2].o) != pawnSeen) p.Problem("keep's object argument isn't Self");
            if (std::string(g[3].text) != "FPctByRange") p.Problem(std::string("keep's name argument is '") + g[3].text + "'");
        }
        if (bytes() != original) p.Problem("after the natives' patch the bytecode isn't the original");
        s.notes.push_back("natives " + std::to_string(nTwice) + " and " + std::to_string(nKeep) + ": FPctByRange(5, 0, 10) " +
                          std::to_string(doubled) + " through 'twice', 'keep' got its 4 arguments");
    }
    nat::Release("self-test");
    End(s);
}

// Mod natives from Lua (tests/mods/native-lua): its module on, FPctByRange returns through the Lua function (halved);
// a hot reload keeps the native's index; off, the original
void TestNativesLua()
{
    Step& s = Begin("Mirage natives (Lua)");
    Probe p(s);
    Module* m = modules::Find("Native (Lua)");
    if (!m) {
        p.Problem("the Native (Lua) module (tests/mods/native-lua) is missing");
        End(s);
        return;
    }
    const auto pct = [&] {
        float out = -1.f;
        OnGameThread([&] {
            auto* pawn = game::Pawn();
            if (!pawn) return;
            mirage::call::Params prm(pawn, "FPctByRange");
            prm.Set("Value", 5.f);
            prm.Set("InMax", 10.f);
            if (mirage::call::Call(pawn, prm)) out = prm.GetFloat("ReturnValue");
        });
        return out;
    };
    const auto index = [](Module* mod) {
        auto* v = mod ? dynamic_cast<ctl::SliderInt*>(mod->FindSetting("Index")) : nullptr;
        return v ? v->Get() : -1;
    };
    const auto setOn = [](Module* mod, bool on) {
        OnRenderThread([mod, on] { mod->SetEnabled(on); });
        WaitTicks(10);
    };
    setOn(m, true);
    const float on = pct();
    const int before = index(m);
    std::string reload = "timed out";
    OnGameThread([&] { reload = mirage::mods::Reload("native-lua"); }, 15.0);
    if (!reload.empty()) p.Problem("hot reload of native-lua: " + reload);
    Module* again = modules::Find("Native (Lua)");
    if (again && !again->Enabled()) setOn(again, true);
    WaitTicks(10);
    const float reloaded = pct();
    const int after = index(again);
    setOn(again ? again : m, false);
    const float off = pct();
    char line[200];
    std::snprintf(line, sizeof line, "FPctByRange(5, 0, 10): %.3f through the Lua native %d, %.3f after a reload (native %d), %.3f off",
                  on, before, reloaded, after, off);
    s.notes.push_back(line);
    if (std::fabs(on - 0.25f) > 1e-4f) p.Problem(std::string("the Lua native didn't halve the result: ") + line);
    if (std::fabs(reloaded - 0.25f) > 1e-4f || after != before) p.Problem(std::string("after the reload: ") + line);
    if (std::fabs(off - 0.5f) > 1e-4f) p.Problem(std::string("off, FPctByRange isn't the original: ") + line);
    End(s);
}

void TestMirageScriptPatch()
{
    Step& s = Begin("Mirage script patch");
    Probe p(s);
    Module* m = modules::Find("Higher Jumps");
    if (!m) {
        p.Problem("the Higher Jumps module (examples/mods/higher-jumps) is missing");
        p.Check(1, true);
        End(s);
        return;
    }
    Classes::UStruct* fn = nullptr;
    OnGameThread([&] { fn = mirage::scripts::Find("TdGame.TdMove_Jump.StartMove"); });
    if (!fn) {
        p.Problem("TdGame.TdMove_Jump.StartMove wasn't found");
        p.Check(1, true);
        End(s);
        return;
    }
    const auto patched = [&] {
        bool on = false;
        OnGameThread([&] { on = mirage::scripts::Patched(fn); });
        return on;
    };
    const auto set = [&](bool on) {
        OnRenderThread([m, on] { m->SetEnabled(on); });
        WaitTicks(10);      // The mod patches or restores on its next tick
    };
    const auto jump = [] { return StandingJump(); };

    OnGameThread([] { player_state::Save(); });
    set(false);
    if (patched()) p.Problem("off: the function is still patched");
    const float before = jump();
    set(true);
    if (!patched()) p.Problem("on: the function wasn't patched (the mod's log says why)");
    const float on = jump();
    set(false);
    if (patched()) p.Problem("off again: the function is still patched");
    const float after = jump();
    OnGameThread([] { player_state::Load(); });
    WaitGrounded();

    char line[200];
    std::snprintf(line, sizeof line, "standing jump: %.0f units high, %.0f with the patch (x%.2f), %.0f after it",
                  before, on, before > 0 ? on / before : 0.0, after);
    s.notes.push_back(line);
    if (before <= 10.f) p.Problem("the player didn't jump (" + std::to_string(before) + " units)");
    else {
        if (on < before * 1.6f) p.Problem("with the patch the jump isn't higher enough: " + std::to_string(on) + " vs " + std::to_string(before));
        if (std::abs(after - before) > before * 0.15f)
            p.Problem("after the patch the jump is " + std::to_string(after) + ", not the " + std::to_string(before) + " of before");
    }
    set(true);      // As the player would have it
    p.Check(1, true);
    End(s);
}

// Mirage step 5, with the self-test's own mods (tests/mods, put in selftest-mods by `mirage game-test`): load order and
// what is left out, the world phase, a crashing mod caught and named while the rest go on (and left out next time),
// hot reload ten times in a row (Lua and native, settings and state kept, no memory left behind), and the file watcher.
void TestMirageLifecycle()
{
    Step& s = Begin("Mirage lifecycle");
    Probe p(s);
    namespace mm = mirage::mods;
    const auto find = [](const std::string& id) {
        const auto all = mm::List();
        const auto it = std::find_if(all.begin(), all.end(), [&](const mm::Info& i) { return i.id == id; });
        return it == all.end() ? std::optional<mm::Info>{} : std::optional<mm::Info>{*it};
    };
    const auto has = [](const std::string& text, const char* part) { return text.find(part) != std::string::npos; };

    // ---- Order: order-c (early), then order-b (wants order-c), then order-a (needs order-b)
    {
        const auto a = find("order-a"), b = find("order-b"), c = find("order-c");
        if (!a || !b || !c || !a->loaded || !b->loaded || !c->loaded) {
            p.Problem("order-a, order-b and order-c should all be loaded");
        } else {
            if (!(c->loadIndex < b->loadIndex && b->loadIndex < a->loadIndex))
                p.Problem("load order " + std::to_string(c->loadIndex) + "/" + std::to_string(b->loadIndex) + "/" +
                          std::to_string(a->loadIndex) + ": expected order-c, then order-b, then order-a");
            s.notes.push_back("order: order-c #" + std::to_string(c->loadIndex) + " (early), order-b #" +
                              std::to_string(b->loadIndex) + ", order-a #" + std::to_string(a->loadIndex));
        }
        for (const auto& [id, why] : {std::pair{"order-missing", "isn't installed"}, {"cycle-1", "cycle"}, {"cycle-2", "cycle"}}) {
            const auto m = find(id);
            if (!m) p.Problem(std::string(id) + " was not found");
            else if (m->loaded || !has(m->problem, why))
                p.Problem(std::string(id) + " should be left out (" + why + "), it is: " + (m->loaded ? "loaded" : m->problem));
        }
    }

    // ---- World phase: loaded once the level was in, its module joined the menu on the render thread
    {
        const auto w = find("world-lua");
        if (!w || !w->loaded) p.Problem("world-lua (world phase) didn't load: " + (w ? w->problem : std::string("not found")));
        else if (!WaitFor(5.0, [] { return modules::Find("Test world-lua") != nullptr; }))
            p.Problem("world-lua's module never joined the menu");
        else s.notes.push_back("world phase: world-lua loaded #" + std::to_string(w->loadIndex));
    }

    // ---- A crash: caught, named, the mod stopped, the others go on, and it won't load next time
    {
        Module* crasher = modules::Find("Crasher");
        Module* hello = modules::Find("Hello (Lua)");
        if (!crasher || !hello) {
            p.Problem("the Crasher or Hello (Lua) module is missing");
        } else {
            p.Expect("[mod crasher] CRASHED");
            OnRenderThread([crasher] { crasher->SetEnabled(true); });
            WaitTicks(3);
            const auto c = find("crasher");
            if (!c || c->loaded || !has(c->problem, "crashed") || !has(c->problem, "crasher.dll+0x"))
                p.Problem("the crash wasn't caught and named: " + (c ? c->problem : std::string("crasher not found")));
            else s.notes.push_back("crasher: " + c->problem);
            if (crasher->Enabled()) p.Problem("Crasher is still on after crashing");
            if (!crasher->Hidden()) p.Problem("Crasher is still listed after crashing");
            const auto crashed = mm::CrashedMods(paths::Dir() / "selftest-mods");
            const auto it = crashed.find("crasher");
            if (it == crashed.end()) p.Problem("crashes.toml doesn't name crasher");
            else {
                // The next start: the same mods, resolved with that list, leave crasher out
                std::vector<mm::Manifest> manifests;
                for (const auto& e : std::filesystem::directory_iterator(paths::Dir() / "selftest-mods"))
                    if (auto parsed = mm::Load(e.path()); parsed.manifest) manifests.push_back(*parsed.manifest);
                const auto next = mm::Resolve(manifests, *mirage::semver::Version::Parse(mm::kRuntimeVersion), crashed);
                if (!next.excluded.count("crasher") || !has(next.excluded.at("crasher"), "crashed the last time"))
                    p.Problem("crasher would load again next time");
            }
            // The rest go on
            auto* ticks = dynamic_cast<ctl::Value<int>*>(hello->FindSetting("Ticks"));
            const int before = ticks ? ticks->Get() : 0;
            OnRenderThread([hello] { hello->SetEnabled(true); });
            WaitTicks(10);
            if (!ticks || ticks->Get() <= before) p.Problem("Hello (Lua) stopped running after crasher crashed");
            OnRenderThread([hello] { hello->SetEnabled(false); });
        }
    }

    // ---- Hot reload, Lua, ten times: new code each time, the setting and the on state kept, no memory left behind
    {
        Module* m = modules::Find("Reload (Lua)");
        const auto file = paths::Dir() / "selftest-mods" / "reload-lua" / "main.lua";
        std::string code;
        {
            std::ifstream in(file, std::ios::binary);
            std::stringstream t;
            t << in.rdbuf();
            code = t.str();
        }
        const auto write = [&](int version) {
            std::string c = code;
            const auto at = c.find("local VERSION = 0");
            if (at != std::string::npos) c.replace(at, 17, "local VERSION = " + std::to_string(version));
            std::ofstream(file, std::ios::binary | std::ios::trunc) << c;
        };
        auto* kept = m ? dynamic_cast<ctl::Value<std::string>*>(m->FindSetting("Kept")) : nullptr;
        if (!m || !kept || code.find("local VERSION = 0") == std::string::npos) {
            p.Problem("Reload (Lua) or its Kept setting is missing");
        } else {
            kept->Set("kept across reloads");
            OnRenderThread([m] { m->SetEnabled(true); });
            WaitTicks(3);
            std::int64_t memoryAfterFirst = 0;
            std::int64_t leftBehind = 0;        // By reloads 2..10 themselves (memory::Tally: their own thread, their own work)
            for (int i = 1; i <= 10; ++i) {
                write(i);
                std::string result = "timed out";
                OnGameThread([&] {
                    mirage::memory::Tally tally;
                    result = mm::Reload("reload-lua");
                    if (i > 1) leftBehind += tally.Bytes();
                }, 10.0);
                if (i == 1) memoryAfterFirst = mirage::memory::LiveBytes();
                auto* status = dynamic_cast<ctl::Value<std::string>*>(m->FindSetting("Status"));
                if (!result.empty()) { p.Problem("reload " + std::to_string(i) + ": " + result); break; }
                if (modules::Find("Reload (Lua)") != m) { p.Problem("reload " + std::to_string(i) + ": a different module"); break; }
                if (!status || status->Get() != "v" + std::to_string(i)) {
                    p.Problem("reload " + std::to_string(i) + ": the new code didn't run (Status '" + (status ? status->Get() : "") + "')");
                    break;
                }
                if (!m->Enabled()) { p.Problem("reload " + std::to_string(i) + ": the module isn't on any more"); break; }
                if (kept->Get() != "kept across reloads") { p.Problem("reload " + std::to_string(i) + ": the setting was reset"); break; }
            }
            // The menu's memory as a whole also moves with what else runs meanwhile (scans, caches, the log): only noted
            const std::int64_t grown = mirage::memory::LiveBytes() - memoryAfterFirst;
            if (leftBehind > 64 * 1024) p.Problem("9 more Lua reloads left " + std::to_string(leftBehind / 1024) + " KB behind");
            s.notes.push_back("Lua: 10 reloads; reloads 2-10 left " + std::to_string(leftBehind / 1024) + " KB behind (the "
                              "whole menu's memory: " + std::to_string(grown / 1024) + " KB)");

            // The watcher: a changed file reloads it by itself
            const int reloadsBefore = find("reload-lua")->reloads;
            write(42);
            if (!WaitFor(5.0, [&] { return find("reload-lua")->reloads > reloadsBefore; }))
                p.Problem("changing main.lua didn't reload the mod by itself");
            else {
                auto* status = dynamic_cast<ctl::Value<std::string>*>(m->FindSetting("Status"));
                if (!status || status->Get() != "v42") p.Problem("the watcher's reload didn't run the new code");
                else s.notes.push_back("watcher: main.lua changed, reloaded by itself");
            }
            write(0);
            OnRenderThread([m] { m->SetEnabled(false); });
        }
    }

    // ---- Hot reload, native (from a copy of its DLL), ten times with the module on
    {
        Module* m = modules::Find("Hello (native)");
        auto* ticks = m ? dynamic_cast<ctl::Value<int>*>(m->FindSetting("Ticks")) : nullptr;
        if (!m || !ticks) {
            p.Problem("Hello (native) is missing");
        } else {
            OnRenderThread([m] { m->SetEnabled(true); });
            WaitTicks(3);
            for (int i = 1; i <= 10; ++i) {
                std::string result = "timed out";
                OnGameThread([&] { result = mm::Reload("hello"); }, 10.0);
                const int before = ticks->Get();
                WaitTicks(3);
                if (!result.empty()) { p.Problem("native reload " + std::to_string(i) + ": " + result); break; }
                if (!m->Enabled()) { p.Problem("native reload " + std::to_string(i) + ": the module isn't on"); break; }
                if (ticks->Get() <= before) { p.Problem("native reload " + std::to_string(i) + ": its on_tick doesn't run"); break; }
            }
            std::size_t copies = 0;
            std::error_code ec;
            for (const auto& e : std::filesystem::directory_iterator(paths::Dir() / "selftest-mods" / "hello" / ".mirage", ec))
                copies += e.path().extension() == ".dll";
            if (copies != 1) p.Problem(std::to_string(copies) + " copies of hello.dll in .mirage (one is loaded)");
            OnRenderThread([m] { m->SetEnabled(false); });
            s.notes.push_back("native: 10 reloads of hello, ticks " + std::to_string(ticks->Get()));
        }
    }
    p.Check(1, true);
    End(s);
}
#endif

void TestModule(Module& m)
{
    Step& s = Begin("module " + m.Tab() + "/" + m.Name());
    // Its column drawn open, however the player left it (a collapsed column draws none of its panels)
    gui::OpenColumnForTest(m.Tab().c_str());
    struct CloseColumn { ~CloseColumn() { gui::OpenColumnForTest(""); } } closeColumn;
    const unsigned n = g_options.ticks;
    Probe p(s);
    const bool drawsHud = kDrawsHud.count(m.Name()) != 0;
    unsigned hudBefore = 0;
    std::string listsBefore;
    // While a HUD is measured, the other HUDs whose text changes as the game runs (Game Status counts ticks, events and
    // health, Collision View the volumes the player is in) are off: they made "still draws after switching off" fail
    // with nothing left of the module's own
    std::vector<Module*> quieted;
    if (drawsHud) {
        std::set<std::string> noisy(kDrawsHud.begin(), kDrawsHud.end());
        noisy.insert({"Game Status", "Info Lines", "Tool Overlay", "Collision View", "Hello (C#)"});	// Hello (C#): the speed
        for (const std::string& name : noisy)
            if (Module* other = modules::Find(name); other && other != &m && other->Enabled()) quieted.push_back(other);
        OnRenderThread([&quieted] { for (Module* q : quieted) q->SetEnabled(false); });
    }
    if (drawsHud) {
        OnRenderThread([&m] { if (m.Enabled()) m.SetEnabled(false); });
        hudBefore = HudVertices();
        listsBefore = hooks::MenuDrawLists();
        OnRenderThread([] { gui::visible = true; });
    }
    const auto runsBefore = [&] { const auto e = sched::Snapshot(); const auto* v = FindEntry(e, m.Name()); return v ? v->runs : 0; }();
#if MIRAGE_GAME
    const Expectation expect = ExpectationFor(m);
    // Main Menu loads a level; Restart Checkpoint respawns the player (a new pawn, usually without a level load)
    const bool loadsLevel = m.Name() == "Main Menu";
    const bool respawns = m.Name() == "Restart Checkpoint";
    const int loadsBefore = g_loadsFinished.load();
    const void* pawnBefore = nullptr;
    if (respawns) OnGameThread([&] { pawnBefore = game::Pawn(); });
    if (expect.setup) OnGameThread(expect.setup);
#endif

    if (m.IsAction()) {
        OnRenderThread([&m] { m.expanded = true; m.Toggle(); });
    } else {
        bool switchedOn = false;   // Read right away: one-shot tools (Kickglitch, ...) may fire and switch off next tick
        OnRenderThread([&m, &switchedOn] {
            m.expanded = true;
            if (m.Enabled()) m.SetEnabled(false);   // On by default: test switching it on too
            m.SetEnabled(true);
            switchedOn = m.Enabled();
        });
        if (!switchedOn) p.Problem("did not switch on");
    }
    WaitTicks(n);
#if MIRAGE_GAME
    if (expect.whileOn && !loadsLevel)
        if (const std::string problem = Verify(expect.whileOn); !problem.empty()) p.Problem(problem);
#endif

    // Every control that can be bound to a key, pressed once per slot (what a key press does)
    OnRenderThread([&m] {
        for (ctl::Setting* st : m.Settings()) {
            if (kNoPress.count(st->Name())) continue;
            for (int slot = 0; slot < 2; ++slot)
                if (st->BindLabel(slot)) st->Press(slot);
        }
    });
    WaitTicks(std::max(2u, n / 2));

    OnRenderThread([] { gui::visible = false; });   // HUD-only drawing (some modules draw differently)
    WaitTicks(std::max(3u, n / 2));
    if (drawsHud) {
        const unsigned hudOn = hooks::MenuVertices();
        if (hudOn <= hudBefore)
            p.Problem("draws nothing with the menu closed (" + std::to_string(hudOn) + " vertices, " +
                      std::to_string(hudBefore) + " before it was on)");
        else s.notes.push_back("draws " + std::to_string(hudOn - hudBefore) + " vertices with the menu closed");
    }
    OnRenderThread([] { gui::visible = true; });
    WaitTicks(2);

#if MIRAGE_GAME
    // Actions may start a level load (Restart Checkpoint, Main Menu): wait for it before judging the game thread
    if (loadsLevel) WaitFor(30.0, [&] { return g_loadsFinished.load() > loadsBefore; });
    if (game::IsLoading() || g_loadsFinished.load() > loadsBefore) SettleAfterLoad(120.0);
    if (loadsLevel && g_loadsFinished.load() <= loadsBefore) p.Problem("no level load happened");
    if (respawns) {
        const void* pawnAfter = nullptr;
        WaitFor(30.0, [&] {
            OnGameThread([&] { pawnAfter = game::Pawn(); }, 1.0);
            return pawnAfter && (pawnAfter != pawnBefore || g_loadsFinished.load() > loadsBefore);
        });
        if (!pawnAfter) p.Problem("no player 30 s after restarting");
        else if (pawnAfter == pawnBefore && g_loadsFinished.load() <= loadsBefore) p.Problem("the player was not respawned");
    }
#endif

    const auto entries = sched::Snapshot();
    if (const auto* e = FindEntry(entries, m.Name())) {
        if (e->state == sched::State::Failed) p.Problem("the scheduler marked it failed (its OnTick threw)");
        if (!m.IsAction() && m.Enabled())
            s.notes.push_back("OnTick ran " + std::to_string(e->runs - runsBefore) + " times, avg " +
                              std::to_string(e->avgUs) + " us; OnRender avg " + std::to_string(e->renderAvgUs) + " us");
    }

    if (!m.IsAction()) {
        OnRenderThread([&m] { m.SetEnabled(false); });
        if (m.Enabled()) p.Problem("did not switch off");
    }
    if (drawsHud) {
        const unsigned hudOff = HudVertices();
        if (hudOff > hudBefore) {
            // Who else is on and draws: the usual cause is another HUD whose text changed meanwhile
            std::string drawing;
            for (const auto& e : sched::Snapshot())
                if (e.renderAvgUs > 0.f)
                    if (Module* other = modules::Find(e.name); other && other != &m && other->Enabled())
                        drawing += (drawing.empty() ? "" : ", ") + e.name;
            p.Problem("still draws after switching off (" + std::to_string(hudOff) + " vertices, " +
                      std::to_string(hudBefore) + " before it was on; now: " + hooks::MenuDrawLists() +
                      "; before: " + listsBefore + "; also on and drawing: " + (drawing.empty() ? "none" : drawing) + ")");
        }
        OnRenderThread([] { gui::visible = true; });
        OnRenderThread([&quieted] { for (Module* q : quieted) q->SetEnabled(true); });
    }
    OnRenderThread([&m] { m.expanded = false; });
    WaitTicks(3);   // OnDisable hands its cleanup to the next tick
#if MIRAGE_GAME
    if (expect.afterOff)
        if (const std::string problem = Verify(expect.afterOff); !problem.empty()) p.Problem("after off: " + problem);
#endif
    p.Check(n, !m.IsAction());
    End(s);
}

void TestAllOn(const std::vector<Module*>& mods)
{
    Step& s = Begin("all modules on");
    Probe p(s);
    OnRenderThread([&] {
        for (Module* m : mods) if (!m->IsAction()) m->SetEnabled(true);   // Groups switch each other off
    });
    WaitTicks(g_options.ticks * 3);
    OnRenderThread([] { gui::visible = false; });
    WaitTicks(g_options.ticks);
    OnRenderThread([] { gui::visible = true; });
    for (const auto& e : sched::Snapshot())
        if (e.state == sched::State::Failed) p.Problem(e.name + ": the scheduler marked it failed");
    OnRenderThread([&] { for (Module* m : mods) if (!m->IsAction()) m->SetEnabled(false); });
    WaitTicks(3);
    p.Check(g_options.ticks * 4, true);
    End(s);
}

void Performance()
{
    Step& s = Begin("performance");
    const auto t = sched::Ticks();
    char line[200];
    std::snprintf(line, sizeof line, "menu work per tick: avg %.3f ms, last %.3f ms, budget %.2f ms, pressure x%.2f, %llu ticks",
                  t.avgMs, t.lastMs, t.budgetMs, t.pressure, static_cast<unsigned long long>(t.ticks));
    s.notes.push_back(line);
    for (const auto& e : sched::Snapshot()) {
        if (e.maxUs < 1000.f && e.renderMaxUs < 1000.f) continue;
        std::snprintf(line, sizeof line, "%s: tick avg %.0f us max %.0f us, render avg %.0f us max %.0f us", e.name.c_str(),
                      e.avgUs, e.maxUs, e.renderAvgUs, e.renderMaxUs);
        s.notes.push_back(line);
    }
    for (const auto& c : cache::Snapshot()) {
        std::snprintf(line, sizeof line, "cache %s (%s): %llu hits, %llu misses, %.1f ms computing", c.name.c_str(),
                      cache::ScopeName(c.scope), static_cast<unsigned long long>(c.hits),
                      static_cast<unsigned long long>(c.misses), c.computeSeconds * 1000.0);
        s.notes.push_back(line);
    }
    End(s);
}

void ReadOptions(std::istream& in)
{
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Lower(Trim(line.substr(0, eq))), value = Trim(line.substr(eq + 1));
        if (key == "map") g_options.map = value;
        else if (key == "checkpoint") g_options.checkpoint = std::max(0, std::atoi(value.c_str()));
        else if (key == "ticks") g_options.ticks = static_cast<unsigned>(std::clamp(std::atoi(value.c_str()), 2, 3600));
        else if (key == "only") g_options.only = SplitList(value);
        else if (key == "modtests") { const auto ids = SplitList(value); g_options.modTests.assign(ids.begin(), ids.end()); }
        else if (key == "skip") g_options.skip = SplitList(value);
        else if (key == "quit") g_options.quit = value != "0";
        else if (key == "fast") g_options.fast = value != "0";
        else spdlog::warn("Self-test: unknown option '{}'", key);
    }
}

BOOL CALLBACK CloseGameWindow(HWND h, LPARAM)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}

// The feature tests, by name (--only / --skip pick them like modules). `fastTas`: its playbacks don't depend on pacing,
// so in fast mode they run as fast as they go. `optIn`: measurements, only when picked by name.
struct Named { const char* name; void (*run)(); bool fastTas; bool optIn = false; };

const Named* FeatureList()
{
#if MIRAGE_GAME
    static const Named tests[] = {
        {"Collision view inspect", &TestCollisionInspect, false},
        {"Collision view static geometry", &TestCollisionStatic, false},
        {"Mirage runtime", &TestMirageRuntime, false},
        {"Mirage layouts", &TestMirageLayouts, false},
        {"Mirage calls", &TestMirageCalls, false},
        {"Mirage function hooks", &TestMirageFunctionHooks, false},
        {"Mirage mods", &TestMirageMods, false},
        {"Mirage lifecycle", &TestMirageLifecycle, false},
        {"Mirage hooks from mods", &TestMirageModHooks, false},
        {"Mirage engine symbols", &TestEngineSymbols, false},
        {"Mirage movement tuning", &TestMovementTuning, false},
        {"Mirage Kismet (read)", &TestKismetRead, false},
        {"Mirage Kismet (edit)", &TestKismetEdit, false},
        {"Mirage Kismet (layer)", &TestKismetLayer, false},
        {"Mirage physics", &TestPhysics, false},
        {"Mirage triggers", &TestTriggers, false},
        {"Mirage UI layer", &TestMirageUi, false},
        {"Cache system", &TestCacheSystem, false},
        {"Memory manager", &TestMemoryManager, false},
        {"Crash reporter", &TestCrashReporter, false},
        {"Mirage C# mods", &TestCSharpMods, false},
        {"Mirage content", &TestMirageContent, false},
        {"Mirage textures", &TestMirageTextures, false},
        {"Mirage script patch", &TestMirageScriptPatch, true},
        {"Mirage script injection", &TestScriptInjection, false},
        {"Mirage script injection (Lua)", &TestScriptInjectionMods, true},
        {"Mirage natives (Lua)", &TestNativesLua, false},
        {"Mirage classes (probe)", &TestClassesProbe, false},
        {"Mirage classes (.uc)", &TestClassesSource, false},
        {"Mirage classes (Lua)", &TestClassesLua, false},
        {"Mirage classes (C#)", &TestClassesCSharp, false},
        {"Mirage classes (example mod)", &TestClassesExample, false},
        {"Mirage level additions", &TestLevelLayers, false},
        {"Mirage maps", &TestModMaps, false},
        {"Mirage editor map", &TestEditorMap, false},
        {"Mirage world access", &TestWorldAccess, false},
        {"Mirage world access (C#)", &TestWorldAccessCs, false},
        {"Mirage change log", &TestChangeLog, false},
        {"Engine script events", &TestEngineScriptEvents, false},
        {"Engine console", &TestEngineConsole, false},
        {"Mod tests", &TestModTests, false},                // After the loader's own steps: a mod's tests may change the game
        {"Engine death hook", &TestEngineDeath, false},     // Last: it restarts the checkpoint
        {nullptr, nullptr, false},
    };
    return tests;
#else
    static const Named none[] = {{nullptr, nullptr, false}};
    return none;
#endif
}

} // namespace

bool Requested()
{
    const auto path = paths::Dir() / "selftest";
    std::ifstream in(path);
    if (!in) return false;
    ReadOptions(in);
    in.close();
    std::error_code ec;
    std::filesystem::remove(path, ec);   // Once: a crash must not make every later start a test
    std::filesystem::remove(ResultPath(), ec);
    g_requested = true;
    g_started = GetTickCount64();
    spdlog::info("Self-test requested: map={} checkpoint={} ticks={} quit={}", g_options.map, g_options.checkpoint,
                 g_options.ticks, g_options.quit);
    WriteResult(false);
    return true;
}

// The engine's frame rate cap (UGameEngine.bSmoothFrameRate, 62 fps): off while testing, so frame and tick counted waits
// go as fast as the PC draws. Put back as it was.
void SetFrameRateCap(bool on)
{
#if MIRAGE_GAME
    static bool was = true, changed = false;
    if (on && !changed) return;
    OnGameThread([&] {
        const auto& objects = Classes::UObject::GetGlobalObjects();
        for (std::size_t i = 0; i < objects.Num(); ++i) {
            auto* o = objects.GetByIndex(i);
            if (!o || !game::IsA(o, Classes::UGameEngine::StaticClass()) || o->GetName().rfind("Default__", 0) == 0) continue;
            auto* engine = static_cast<Classes::UGameEngine*>(o);
            if (on) engine->bSmoothFrameRate = was ? 1 : 0;
            else {
                was = engine->bSmoothFrameRate != 0;
                engine->bSmoothFrameRate = 0;
                changed = true;
            }
            spdlog::info("Self-test: frame rate cap {}", on ? "back on" : "off (fast mode)");
            break;
        }
    });
    if (on) changed = false;
#else
    (void)on;
#endif
}

// The whole test (both ways: the `selftest` file and the Test module). Leaves the menu open when run from the menu.
static void RunTests(bool fromMenu)
{
#if MIRAGE_GAME
    static bool listening = false;      // Once: every run would add another callback
    if (!listening) game::OnLevelLoad(&OnLevelLoad);
    listening = true;
#endif
    const auto saved = SaveAll();
    // The player moving as the game makes it: modules the player left on that change movement (the Level Editor's Fly
    // is No Clip) are off for the run, and back as they were after it (RestoreAll)
    OnRenderThread([] {
        for (const char* name : {"No Clip", "Flight", "Game Speed"})
            if (Module* m = modules::Find(name); m && m->Enabled()) {
                spdlog::info("Self-test: {} was on: off for the run", name);
                m->SetEnabled(false);
            }
    });
    notify::SetMuted(true);         // Nothing on screen but what the modules draw (the HUD checks measure it)
    if (g_options.fast) SetFrameRateCap(false);

    if (TestFrames()) {
#if MIRAGE_GAME
        if (TestGame() && !g_aborted && TestLevel() && !g_aborted &&
            (g_options.keepLevel || Lower(g_options.map) != "none")) {
            TestInputBlocking();
            const Named* tests = FeatureList();
            for (const Named* t = tests; t->name && !g_aborted; ++t) {
                if (!g_options.only.empty() && !g_options.only.count(t->name)) continue;
                if (t->optIn && !g_options.only.count(t->name)) continue;
                if (g_options.skip.count(t->name)) continue;
                t->run();
            }
        }
#endif
        std::vector<Module*> mods;
        // Hidden ones aren't in the menu (a crashed mod's, developer-only ones while developer mode is off)
        // Modules that fail on purpose are left to their own steps (Throw (C#): "Mirage C# mods")
        static const std::set<std::string> kFailsOnPurpose = {"Throw (C#)"};
        modules::ForEach([&](modules::Tab&, Module& m) {
            if (Selected(m) && !m.Hidden() && !kFailsOnPurpose.count(m.Name())) mods.push_back(&m);
        });
        // Actions that leave the level go last
        std::stable_partition(mods.begin(), mods.end(), [](Module* m) { return m->Name() != "Main Menu"; });
        for (Module* m : mods) {
            if (g_aborted) break;
            TestModule(*m);
        }
        if (!g_aborted && g_options.only.empty()) TestAllOn(mods);
    }
    if (!g_aborted) Performance();
    g_restoring = true;                 // Stopped or not: everything goes back the way it was
    if (g_options.fast) SetFrameRateCap(true);
    notify::SetMuted(false);
    RestoreAll(saved);
    g_restoring = false;
    gui::visible = fromMenu;            // From the menu: the menu stays open on the results
}

void Run()
{
    if (!g_requested) return;
    RunTests(false);
    spdlog::info("Self-test: done, unloading");
}

void Finish(bool unloadedCleanly)
{
    if (!g_requested) return;
    Step& s = Begin("unload");
    if (!unloadedCleanly) {
        s.ok = false;
        s.problems.push_back("hooks were not removed cleanly");
    }
    End(s);
    if (g_aborted) {
        Step& a = Begin("aborted");
        a.ok = false;
        a.problems.push_back("unload was requested in the middle of the test (END, or the unload event)");
        End(a);
    }
    WriteResult(true);
    bool passed = true;
    for (const Step& st : g_steps) passed &= st.ok;
    spdlog::info("Self-test {}: {} steps", passed ? "PASSED" : "FAILED", g_steps.size());
    if (g_options.quit) {
        spdlog::info("Self-test: closing the game");
        EnumWindows(CloseGameWindow, 0);
    }
}

// ---- Runs from the menu (developer mode) --------------------------------------------------------------------------------
std::vector<TestInfo> FeatureTests()
{
    std::vector<TestInfo> out;
    for (const Named* t = FeatureList(); t->name; ++t) out.push_back({t->name, t->optIn});
    return out;
}

bool Running() { return g_interactive.load(); }

void Stop()
{
    if (!g_interactive) return;
    g_stop = true;
    spdlog::info("Self-test: stopping...");
}

bool Start(RunOptions o)
{
    if (g_requested || g_interactive.exchange(true)) return false;   // A run (or the file's test) is going
    if (g_worker.joinable()) g_worker.join();                        // The last run's thread: already done

    g_options = Options{};
    g_options.only = std::move(o.only);
    g_options.keepLevel = !o.loadLevel;
    g_options.map = o.map.empty() ? "edge_p" : o.map;
    g_options.checkpoint = std::max(0, o.checkpoint);
    g_options.ticks = std::clamp(o.ticks, 2u, 3600u);
    g_options.fast = o.fast;
    g_options.quit = false;
    g_steps.clear();
    g_current.clear();
    g_aborted = false;
    g_stop = false;
    g_started = GetTickCount64();

    // How many steps to expect, for the progress bar: frames, game, level, input blocking, the picked feature tests and
    // modules, all modules on (full runs), performance
    int planned = 2;
#if MIRAGE_GAME
    planned += 2;
    for (const Named* t = FeatureList(); t->name; ++t)
        if (g_options.only.empty() ? !t->optIn : g_options.only.count(t->name) != 0) ++planned;
#endif
    modules::ForEach([&](modules::Tab&, Module& m) { if (Selected(m)) ++planned; });
    if (g_options.only.empty()) ++planned;

    std::vector<logging::Entry> ignored;
    const std::uint64_t firstLog = logging::CopySince(0, ignored);
    {
        std::lock_guard lock(g_publishMutex);
        g_published = Progress{};
        g_published.running = true;
        g_published.run = ++g_runs;
        g_published.planned = planned;
        g_published.firstLogId = firstLog;
    }
    spdlog::info("Self-test: run {} started: {}, {}, {} ticks per module, {}", g_runs,
                 g_options.only.empty() ? "everything" : std::to_string(g_options.only.size()) + " test(s) picked",
                 g_options.keepLevel ? "current level" : "level " + g_options.map, g_options.ticks,
                 g_options.fast ? "fast mode" : "real speed");

    g_worker = std::thread([] {
        config::SetAutoSave(false);     // The test switches settings: never into the player's config (put back after)
        RunTests(true);
        WriteResult(true);              // selftest-result.json, like `mirage game-test`
        int failed = 0;
        for (const Step& st : g_steps) failed += st.ok ? 0 : 1;
        const double seconds = static_cast<double>(GetTickCount64() - g_started) / 1000.0;
        if (g_aborted)
            spdlog::warn("Self-test: run stopped after {} steps ({} failed), {:.1f} s", g_steps.size(), failed, seconds);
        else if (failed)
            spdlog::error("Self-test: run FAILED: {} of {} steps failed, {:.1f} s", failed, g_steps.size(), seconds);
        else
            spdlog::info("Self-test: run PASSED: {} steps, {:.1f} s", g_steps.size(), seconds);
        config::SetAutoSave(true);
        {
            std::lock_guard lock(g_publishMutex);
            g_published.running = false;
            g_published.stopped = g_aborted;
            g_published.seconds = seconds;
        }
        g_interactive = false;
    });
    return true;
}

void Shutdown()
{
    if (g_interactive) g_stop = true;
    if (g_worker.joinable()) g_worker.join();
}

Progress Snapshot()
{
    std::lock_guard lock(g_publishMutex);
    Progress p = g_published;
    if (p.running) p.seconds = static_cast<double>(GetTickCount64() - g_started) / 1000.0;
    return p;
}

} // namespace selftest
