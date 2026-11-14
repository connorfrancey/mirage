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
#if MEMENU_GAME
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
#include "runtime/natives.h"
#include "runtime/script_patch.h"
#include "runtime/reflect.h"
#include "runtime/layout.h"
#include "runtime/lua_vm.h"
#include "core/events.h"
#include "core/tasks.h"
#include "runtime/crash_reporter.h"
#include "runtime/mod_dotnet.h"
#include "runtime/mem/arena.h"
#include "runtime/mem/manager.h"
#include "runtime/mem/pool.h"
#include "runtime/memory.h"
#include "runtime/mods.h"
#include "runtime/resolve.h"
#include "runtime/packages.h"
#if MEMENU_FEATURE_MOD_MANAGER
#include "features/misc/mod_manager.h"
#include "features/misc/level_editor.h"
#endif
#if MEMENU_FEATURE_SCRIPT_EDITOR
#include "features/misc/script_editor.h"
#endif
#include <shlobj.h>
#if MEMENU_FEATURE_TAS
#include "tas/input_monitor.h"
#include "tas/macro.h"
#include "tas/engine.h"
#include "tas/memory_map.h"
#include "tas/snapshot.h"
#include "tas/timeline.h"
#include "tas/tas.h"
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
#include "features/editor/editor_core.h"
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
    // `memenu game-test` reads the file several times a second: a replace while it is open fails, so try again (the
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

#if MEMENU_GAME
unsigned long long Ticks() { return game::GetStatus().ticks; }
#endif

// Waits until `n` more frames are drawn and the game ticked `n` more times (the slower of the two decides; ticks don't
// count while a level loads). Waiting for ticks and frames instead of seconds keeps the test short on a fast machine
// and fair on a slow one. A stall of `timeout` seconds is logged as an error, which fails the step it happens in.
bool WaitTicks(unsigned n, double timeout = 10.0)
{
    const unsigned frames0 = hooks::EndSceneCalls();
#if MEMENU_GAME
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
#if MEMENU_FEATURE_TAS
    // Where the game's threads are (once per test run: enough to see a hang)
    static bool dumped = false;
    if (!dumped) {
        dumped = true;
        for (const auto& t : tas::memmap::Threads()) {
            if (t.startModule != "mirrorsedge.exe" && t.startModule.rfind("physx", 0) != 0) continue;
            std::string line;
            for (const auto& w : tas::memmap::StackSample(t.id, 28)) line += w + " ";
            spdlog::warn("Stall: thread {} ({}) state {} wait {}: {}", t.id, t.startModule, t.state, t.waitReason, line);
        }
    }
#endif
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

#if MEMENU_GAME
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
        // is evaluated by asking it (memenu/info_lines.eval -> eval.*). The built-in example (a profile without a file),
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
#if MEMENU_FEATURE_SCRIPT_EDITOR
    if (name == "Script Editor") {
        static std::uint64_t framesBefore = 0;
        return {[] { framesBefore = script_editor::Frames(); },
                [] { return script_editor::Frames() == framesBefore ? std::string("its window was never drawn") : std::string(); }};
    }
#endif
#if MEMENU_FEATURE_MOD_MANAGER
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
    // commands the menu hands it (memenu/save_states.*)
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
    // Levels and Sublevels are mods/memenu-levels: what they read, from what it shares (refreshed every 60 ticks)
    if (name == "Levels")
        return {{}, [] {
                    WaitFor(3.0, [] { return SharedInt("memenu-levels", "game_maps") > 0; });
                    return Expect(SharedInt("memenu-levels", "game_maps") > 0 && SharedInt("memenu-levels", "maps") > 0,
                                  "the level list is not the game's (or is empty)"); }, {}};
    if (name == "Sublevels")
        return {{}, [] {
                    WaitFor(3.0, [] { return SharedInt("memenu-levels", "sublevels") > 0; });
                    return Expect(SharedInt("memenu-levels", "sublevels") > 0, "no streaming sublevels listed for this level"); },
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
#if MEMENU_GAME
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
#if MEMENU_GAME
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

#if MEMENU_GAME
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

#if MEMENU_FEATURE_TAS
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

// A macro played through the TAS: the player's forward key held for a second with a jump half way, written with
// tas::Macro instead of recorded. Proves the whole playback path in the game: the replay's key names resolve, the
// inputs reach the player through the engine, every frame runs, and the keys are let go at the end.
void TestTasMacro()
{
    Step& s = Begin("TAS macro");
    Probe p(s);
    if (!tas::Available()) {
        p.Problem("the TAS engine hooks were not found");
        p.Check(1, false);
        End(s);
        return;
    }

    struct Pose { Classes::FVector at{}; int yaw = 0; bool ok = false; };
    std::string forward, jump;
    Pose start;
    OnGameThread([&] {
        auto* pc = game::Controller();
        forward = KeyFor(pc, "GBA_MoveForward", "W");
        jump = KeyFor(pc, "GBA_Jump", "SpaceBar");
        if (auto* pawn = game::Pawn(); pawn && pc) {
            start = {pawn->Location, static_cast<int>(pc->Rotation.Yaw), true};
            player_state::Save();   // Put back afterwards: running on a rooftop can end badly
        }
    });
    if (!start.ok) {
        p.Problem("no player to drive");
        p.Check(1, false);
        End(s);
        return;
    }
    s.notes.push_back("keys: forward " + forward + ", jump " + jump);
    if (const std::string why = WaitPlayerReady(10.0); !why.empty()) {
        p.Problem("the player can't be moved: " + why);
        p.Check(1, false);
        End(s);
        return;
    }

    tas::Macro m(60.f);
    m.Describe("memenu self-test: run forward, jump").Wait(5).Press(forward).Wait(30).Hold(jump, 2).Wait(28)
        .Release(forward).Wait(20);
    const tas::Replay macro = m.Build();

    tas::Replay saved;   // Whatever the user has loaded in the TAS editor
    tas::WithReplay([&](tas::Replay& r) { saved = r; r = macro; });
    OnRenderThread([] { gui::visible = false; });
    tas::Play();

    // Follow it: the highest point the player reaches. Playback paces itself at the replay's 60 fps.
    float topZ = start.at.Z;
    bool airborne = false, started = false, pausedOnce = false;
    tas::Status st{};
    const ULONGLONG deadline = GetTickCount64() + 30000;
    while (GetTickCount64() < deadline && !g_aborted) {
        st = tas::GetStatus();
        if (st.mode == tas::Mode::Playing) started = true;
        if (started && (st.mode != tas::Mode::Playing || st.frame >= st.frames)) break;   // Done (or paused at the end)
        if (started && !pausedOnce && !st.armed && st.frame >= 20) {
            // Paused, the game thread stands still and draws the menu itself, through every overlay's Present hook
            // (a crash on 2026-11-02). Not OnGameThread meanwhile: the game doesn't tick while paused.
            pausedOnce = true;
            gui::visible = true;
            const unsigned before = hooks::StalledFrames();
            const ULONGLONG pausedAt = GetTickCount64();
            tas::TogglePause();
            // 20 menu frames drawn by the paused game thread (or 3 s without them: the problem below)
            WaitFor(3.0, [before] { return hooks::StalledFrames() - before >= 20; });
            if (!tas::GetStatus().paused) p.Problem("pausing the playback didn't pause it");
            const unsigned drawn = hooks::StalledFrames() - before;
            if (drawn == 0) p.Problem("the menu was not drawn while paused");
            s.notes.push_back("paused " + std::to_string(GetTickCount64() - pausedAt) + " ms: " + std::to_string(drawn) +
                              " menu frames drawn meanwhile");
            tas::TogglePause();
            gui::visible = false;
            continue;
        }
        OnGameThread([&] {
            if (auto* pawn = game::Pawn()) {
                topZ = std::max(topZ, pawn->Location.Z);
                airborne |= pawn->Physics == Classes::EPhysics::PHYS_Falling;
            }
        }, 0.5);
    }
    const std::size_t reached = st.frame, frames = macro.FrameCount();
    tas::Stop();
    if (!WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; })) p.Problem("the session did not stop");
    WaitTicks(30);   // Let the player come to a stop

    if (!started) p.Problem("playback never started: " + tas::GetStatus().message);
    else if (reached < frames)
        p.Problem("playback stopped at frame " + std::to_string(reached) + " of " + std::to_string(frames) + ": " +
                  tas::GetStatus().message);

    // Ran the way they were facing (Unreal yaw: 65536 = a full turn), left the ground, and stopped again
    const std::string moved = Verify([&] {
        return WithPlayer([&](ATdPlayerPawn* pawn, ATdPlayerController*) {
            const double yaw = start.yaw * (6.283185307179586 / 65536.0);
            const double dx = pawn->Location.X - start.at.X, dy = pawn->Location.Y - start.at.Y;
            const double along = dx * std::cos(yaw) + dy * std::sin(yaw), dist = std::sqrt(dx * dx + dy * dy);
            const double speed = std::sqrt(pawn->Velocity.X * pawn->Velocity.X + pawn->Velocity.Y * pawn->Velocity.Y);
            char info[200];
            std::snprintf(info, sizeof info, "moved %.0f units (%.0f forward), %.0f units up at most, speed after %.0f",
                          dist, along, topZ - start.at.Z, speed);
            std::string problems;
            if (along < 150.0 || along < 0.7 * dist) problems += "did not run forward; ";
            if (!airborne && topZ - start.at.Z < 30.f) problems += "never jumped; ";
            if (speed > 100.0) problems += "still running after the macro released the key; ";
            return problems.empty() ? std::string("ok: ") + info : problems + info;
        });
    });
    if (moved.rfind("ok: ", 0) == 0) s.notes.push_back(moved.substr(4));
    else p.Problem(moved);

    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnGameThread([] { player_state::Load(); });
    WaitTicks(5);
    OnRenderThread([] { gui::visible = true; });
    p.Check(1, true);
    End(s);
}

// Camera keyframes: a macro with no input but two camera keys 90 degrees apart must turn the player by 90 degrees,
// the TAS setting the look direction itself. The timeline window is open meanwhile (drawn in the game).
void TestTasCamera()
{
    Step& s = Begin("TAS camera keys");
    Probe p(s);
    if (!tas::Available()) {
        p.Problem("the TAS engine hooks were not found");
        p.Check(1, false);
        End(s);
        return;
    }

    std::int32_t startYaw = 0;
    bool ok = false;
    OnGameThread([&] {
        if (auto* pc = game::Controller(); pc && game::Pawn()) {
            startYaw = static_cast<std::int32_t>(pc->Rotation.Yaw);
            ok = true;
            player_state::Save();
        }
    });
    if (!ok) {
        p.Problem("no player to turn");
        p.Check(1, false);
        End(s);
        return;
    }

    constexpr double kUnits = 65536.0 / 360.0;
    const float yaw0 = static_cast<float>(startYaw / kUnits);
    tas::Macro m(60.f);
    m.Describe("memenu self-test: turn 90 degrees with camera keys").Wait(40);
    tas::Replay macro = m.Build();
    tas::timeline::SetCameraKey(macro, {5, yaw0, 0.f, tas::Ease::Linear});
    tas::timeline::SetCameraKey(macro, {30, yaw0 + 90.f, 0.f, tas::Ease::Smooth});

    // The TAS module's timeline window, open while it plays
    ctl::Checkbox* timeline = nullptr;
    modules::ForEach([&](modules::Tab&, Module& mod) {
        for (ctl::Setting* c : mod.Settings())
            if (c->Name() == "Timeline" && mod.Name() == "TAS / Macro") timeline = dynamic_cast<ctl::Checkbox*>(c);
    });
    const bool timelineWas = timeline && timeline->Get();

    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; r = macro; });
    OnRenderThread([timeline] {
        gui::visible = true;
        if (timeline) timeline->Set(true);
    });
    tas::Play();

    bool started = false;
    tas::Status st{};
    const ULONGLONG deadline = GetTickCount64() + 20000;
    while (GetTickCount64() < deadline && !g_aborted) {
        st = tas::GetStatus();
        if (st.mode == tas::Mode::Playing) started = true;
        if (started && (st.mode != tas::Mode::Playing || st.frame >= st.frames)) break;
        Sleep(20);
    }
    tas::Stop();
    if (!WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; })) p.Problem("the session did not stop");

    std::int32_t endYaw = startYaw;
    OnGameThread([&] {
        if (auto* pc = game::Controller()) endYaw = static_cast<std::int32_t>(pc->Rotation.Yaw);
    });
    const double turned = (endYaw - startYaw) / kUnits;
    char info[96];
    std::snprintf(info, sizeof info, "turned %.2f degrees (keys: %.2f -> %.2f)", turned, yaw0, yaw0 + 90.f);
    if (!started) p.Problem("playback never started: " + tas::GetStatus().message);
    else if (std::abs(turned - 90.0) > 0.5) p.Problem(std::string("the camera keys didn't turn the player: ") + info);
    else s.notes.push_back(info);

    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnRenderThread([timeline, timelineWas] {
        if (timeline) timeline->Set(timelineWas);
    });
    OnGameThread([] { player_state::Load(); });
    WaitTicks(5);
    p.Check(1, true);
    End(s);
}

// Go to + frame advance, the replay editor's way: from idle, go to frame 20 of a macro (it plays up to there and
// pauses), step one frame, and stop. The Replay Editor window is open meanwhile (drawn in the game).
void TestTasGoto()
{
    Step& s = Begin("TAS go to and step");
    Probe p(s);
    if (!tas::Available()) {
        p.Problem("the TAS engine hooks were not found");
        p.Check(1, false);
        End(s);
        return;
    }

    tas::Macro m(60.f);
    m.Describe("memenu self-test: go to and step").Wait(60);
    const tas::Replay macro = m.Build();

    ctl::Checkbox* editor = nullptr;
    modules::ForEach([&](modules::Tab&, Module& mod) {
        for (ctl::Setting* c : mod.Settings())
            if (c->Name() == "Replay editor" && mod.Name() == "TAS / Macro") editor = dynamic_cast<ctl::Checkbox*>(c);
    });
    const bool editorWas = editor && editor->Get();

    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; r = macro; });
    OnRenderThread([editor] {
        gui::visible = true;
        if (editor) editor->Set(true);
    });

    // Not OnGameThread while paused: the game doesn't tick then
    const auto pausedOn = [](std::size_t frame) {
        const tas::Status st = tas::GetStatus();
        return st.mode == tas::Mode::Playing && st.paused && st.frame == frame;
    };
    tas::Goto(20);
    if (!WaitFor(10.0, [&] { return pausedOn(20); })) {
        const tas::Status st = tas::GetStatus();
        p.Problem("go to 20 didn't pause on frame 20 (frame " + std::to_string(st.frame) +
                  (st.paused ? ", paused" : ", running") + "): " + st.message);
    } else {
        tas::Advance(1);
        if (!WaitFor(5.0, [&] { return pausedOn(21); }))
            p.Problem("stepping from 20 didn't stop on 21 (frame " + std::to_string(tas::GetStatus().frame) + ")");
        else s.notes.push_back("went to frame 20, stepped to 21");
    }
    tas::Stop();
    if (!WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; })) p.Problem("the session did not stop");

    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnRenderThread([editor, editorWas] {
        if (editor) editor->Set(editorWas);
    });
    WaitTicks(5);
    p.Check(1, true);
    End(s);
}

// Opt-in investigation: the engine's rendering thread switched off (ToggleRenderingThread), then the level loaded again.
// If the load hangs, the game thread's stack is logged.
void TestRenderingThreadOff()
{
    Step& s = Begin("Rendering thread off");
    Probe p(s);
    const auto exeThreads = [] {
        int n = 0;
        for (const auto& t : tas::memmap::Threads()) n += t.startModule == "mirrorsedge.exe" ? 1 : 0;
        return n;
    };
    const int before = exeThreads();
    OnGameThread([] { game::ConsoleCommand("ToggleRenderingThread"); });
    WaitTicks(5);
    const int after = exeThreads();
    s.notes.push_back("exe threads " + std::to_string(before) + " -> " + std::to_string(after));

    std::atomic<bool> done{false};
    DWORD gameThread = 0;
    OnGameThread([&] { gameThread = GetCurrentThreadId(); });
    std::thread watchdog([&] {
        for (int i = 0; i < 80 && !done; ++i) Sleep(100);
        if (done) return;
        for (int round = 0; round < 3; ++round) {
            for (const auto& t : tas::memmap::Threads()) {
                if (t.startModule != "mirrorsedge.exe" && t.id != gameThread) continue;
                std::string line;
                for (const auto& w : tas::memmap::StackSample(t.id, 28)) line += w + " ";
                spdlog::warn("Rendering off: thread {}{} state {} wait {}: {}", t.id, t.id == gameThread ? " (game)" : "",
                             t.state, t.waitReason, line);
            }
            Sleep(1000);
        }
    });
    const int loads = g_loadsFinished.load();
    const auto maps = levels::Maps();
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (Lower(maps[i].label).find(Lower(g_options.map)) != std::string::npos) {
            OnGameThread([&] { levels::Load(i, g_options.checkpoint, levels::Method::Game, false); });
            break;
        }
    const bool loaded = WaitFor(25.0, [&] { return g_loadsFinished.load() > loads && !game::IsLoading(); });
    done = true;
    watchdog.join();
    if (!loaded) p.Problem("the level didn't load with the rendering thread off");
    else {
        const std::string why = WaitPlayerReady(20.0);
        if (!why.empty()) p.Problem("after loading: " + why);
        else s.notes.push_back("loaded with the rendering thread off");
    }
    p.Check(1, false);
    End(s);
}

// Memory snapshots (tas/snapshot.h), on a macro from a level start that runs, jumps and turns:
//   1. a playback keeps keyframes (the loader tracks the game's memory)
//   2. Go to an earlier frame puts the game back without loading the level: the player is exactly (bit for bit) where
//      the recording had them on that frame
//   3. playing on from there to the end gets exactly the same end state; keyframes reached again are compared
//   4. Frame back, ten times: each one exact, and how long one takes
//   5. Restart goes back to frame 0 without a level load
// Then the session stops and the game must still be playable.
void TestTasSnapshots()
{
    Step& s = Begin("TAS snapshots");
    Probe p(s);
    const auto fail = [&](const std::string& why) {
        p.Problem(why);
        p.Check(1, false);
        End(s);
    };
    if (!tas::Available()) return fail("the TAS engine hooks were not found");

    const auto maps = levels::Maps();
    int index = -1;
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (Lower(maps[i].label).find(Lower(g_options.map)) != std::string::npos) { index = i; break; }
    if (index < 0) return fail("no level matching '" + g_options.map + "'");
    const int cp = std::min(g_options.checkpoint, static_cast<int>(maps[index].checkpoints.size()));

    Module* tasModule = nullptr;
    modules::ForEach([&](modules::Tab&, Module& m) { if (m.Name() == "TAS / Macro") tasModule = &m; });
    auto* atEnd = tasModule ? dynamic_cast<ctl::Combo*>(tasModule->FindSetting("At the end")) : nullptr;
    auto* instant = tasModule ? dynamic_cast<ctl::Checkbox*>(tasModule->FindSetting("Instant rewind")) : nullptr;
    if (!atEnd || !instant) return fail("the TAS module's At the end / Instant rewind controls were not found");
    const int atEndWas = atEnd->Get();
    const bool instantWas = instant->Get();

    std::string forward, jump;
    OnGameThread([&] {
        auto* pc = game::Controller();
        forward = KeyFor(pc, "GBA_MoveForward", "W");
        jump = KeyFor(pc, "GBA_Jump", "SpaceBar");
    });
    tas::Macro m(60.f);
    m.Describe("memenu self-test: snapshots").Wait(5).Press(forward).Wait(30);
    for (int i = 0; i < 20; ++i) m.Axis("MouseX", 6.f).Wait(1);     // turning while running
    m.Hold(jump, 3).Wait(50).Release(forward).Wait(40);
    tas::Replay macro = m.Build();
    macro.start = tas::StartKind::Checkpoint;
    macro.map = maps[index].label;
    macro.checkpoint = cp > 0 ? maps[index].checkpoints[static_cast<std::size_t>(cp - 1)] : std::string();
    macro.syncValidUntil = 0;                       // this playback records the reference states
    const std::size_t frames = macro.FrameCount();

    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; r = macro; });
    OnRenderThread([&] {
        gui::visible = false;
        atEnd->Set(static_cast<int>(tas::AtEnd::Pause));
        instant->Set(true);
    });
    WaitTicks(3);

    const auto pausedOn = [](std::size_t frame) {
        const tas::Status st = tas::GetStatus();
        return st.mode == tas::Mode::Playing && st.paused && !st.seeking && st.frame == frame;
    };
    const auto describe = [](const tas::Sync& a) {
        char text[160];
        std::snprintf(text, sizeof text, "(%.3f, %.3f, %.3f) v (%.3f, %.3f, %.3f) move %u", a.x, a.y, a.z, a.vx, a.vy,
                      a.vz, a.state);
        return std::string(text);
    };
    const auto reference = [&](std::size_t frame) {
        tas::Sync want;
        tas::WithReplay([&](tas::Replay& r) { if (frame < r.sync.size()) want = r.sync[frame]; });
        return want;
    };
    // Goes to `frame` and checks the player against the recording; the milliseconds it took (-1: it didn't get there)
    const auto goTo = [&](std::size_t frame, bool back) -> double {
        const ULONGLONG t0 = GetTickCount64();
        if (back) tas::FrameBack(1);
        else tas::Goto(frame);
        if (!WaitFor(20.0, [&] { return pausedOn(frame); })) {
            const tas::Status st = tas::GetStatus();
            p.Problem("going to frame " + std::to_string(frame) + " stopped on " + std::to_string(st.frame) +
                      (st.paused ? " (paused)" : " (running)") + ": " + st.message);
            return -1.0;
        }
        const double ms = static_cast<double>(GetTickCount64() - t0);
        const tas::Sync now = tas::GetStatus().player;
        const tas::Sync want = frame < frames ? reference(frame) : now;
        if (frame < frames && !(now == want))
            p.Problem("frame " + std::to_string(frame) + ": the player is at " + describe(now) + ", recorded " +
                      describe(want));
        return ms;
    };

    // 1. The reference run, keyframes kept
    const int loadsBefore = g_loadsFinished.load();
    tas::Play();
    bool started = false;
    WaitFor(90.0, [&] {
        const tas::Status st = tas::GetStatus();
        started |= st.mode != tas::Mode::Idle;
        return pausedOn(frames) || (started && st.mode == tas::Mode::Idle);
    });
    tas::Status end = tas::GetStatus();
    if (!pausedOn(frames)) {
        tas::Stop();
        tas::WithReplay([&](tas::Replay& r) { r = saved; });
        OnRenderThread([&] { atEnd->Set(atEndWas); instant->Set(instantWas); gui::visible = true; });
        return fail("the playback didn't pause at its end (frame " + std::to_string(end.frame) + " of " +
                    std::to_string(frames) + "): " + end.message);
    }
    if (!end.snapshots) {
        tas::Stop();
        tas::WithReplay([&](tas::Replay& r) { r = saved; });
        OnRenderThread([&] { atEnd->Set(atEndWas); instant->Set(instantWas); gui::visible = true; });
        return fail("no snapshots: " + end.snapshotsOff);
    }
    const tas::Sync endPlayer = end.player;
    char line[200];
    std::snprintf(line, sizeof line, "%zu frames, %zu keyframes, %.1f MB stored; base %.0f ms, last keyframe %.1f ms",
                  frames, end.keyframes, static_cast<double>(end.snapshotBytes) / 1048576.0, end.snapshotBeginMs,
                  end.captureMs);
    s.notes.push_back(line);
    const int loadsAfterPlay = g_loadsFinished.load();
    // 2. Back to an earlier frame
    const double backMs = goTo(37, false);
    if (backMs >= 0) {
        std::snprintf(line, sizeof line, "went back to frame 37 in %.0f ms (restore %.1f ms), exactly as recorded", backMs,
                      tas::GetStatus().restoreMs);
        s.notes.push_back(line);
    }

    // 3. On to the end again: the same end state
    const double fwdMs = goTo(frames, false);
    if (fwdMs >= 0) {
        const tas::Status st = tas::GetStatus();
        if (!(st.player == endPlayer))
            p.Problem("playing on from 37 ended at " + describe(st.player) + ", the first time " + describe(endPlayer));
        std::snprintf(line, sizeof line, "played on to the end in %.0f ms: %s; %zu keyframes reached again differed "
                      "(%zu pages, put back)", fwdMs, st.player == endPlayer ? "same end state" : "DIFFERENT end state",
                      st.snapped, st.snappedPages);
        s.notes.push_back(line);
    }

    // 4. Frame back, ten times
    double total = 0.0, worst = 0.0;
    int steps = 0;
    for (std::size_t f = frames; f > frames - 10 && !g_aborted; --f) {
        const double ms = goTo(f - 1, true);
        if (ms < 0) break;
        total += ms;
        worst = std::max(worst, ms);
        ++steps;
    }
    if (steps) {
        std::snprintf(line, sizeof line, "frame back x%d: %.0f ms each on average, %.0f ms at most", steps,
                      total / steps, worst);
        s.notes.push_back(line);
    }

    // 4b. Early keyframes, the frames right after the level loaded, before frame 0 itself
    for (std::size_t f : {std::size_t{12}, std::size_t{6}}) {
        const double ms = goTo(f, false);
        if (ms >= 0) s.notes.push_back("went back to frame " + std::to_string(f));
        else break;
    }

    // 5. Restart: frame 0 without loading the level
    tas::Restart();
    if (!WaitFor(10.0, [] { const auto st = tas::GetStatus(); return st.mode == tas::Mode::Playing && st.frame <= 3 && !st.armed; }))
        p.Problem("Restart didn't go back to frame 0 (frame " + std::to_string(tas::GetStatus().frame) + ")");
    else if (g_loadsFinished.load() != loadsAfterPlay)
        p.Problem("Restart loaded the level instead of putting frame 0 back");
    else
        s.notes.push_back("restart: back at frame 0 without loading the level");
    (void)loadsBefore;

    // 6. A small memory budget: older keyframes are thinned out, and going back there is still exact (from an earlier
    // keyframe, running the frames between)
    auto* budget = dynamic_cast<ctl::Slider*>(tasModule->FindSetting("Memory"));
    if (budget) {
        const float budgetWas = budget->Get();
        OnRenderThread([&] { budget->Set(256.f); });
        WaitFor(90.0, [&] { return pausedOn(frames); });
        const tas::Status thin = tas::GetStatus();
        std::snprintf(line, sizeof line, "with 256 MB: %zu keyframes, %.1f MB", thin.keyframes,
                      static_cast<double>(thin.snapshotBytes) / 1048576.0);
        s.notes.push_back(line);
        if (thin.snapshotBytes > end.snapshotBytes) p.Problem("the budget didn't thin any keyframe out");
        for (std::size_t f : {std::size_t{10}, std::size_t{20}, std::size_t{100}})
            if (goTo(f, false) < 0) break;
        s.notes.push_back("went back to frames 10, 20 and 100 after thinning");
        OnRenderThread([&] { budget->Set(budgetWas); });
    } else {
        p.Problem("the TAS module's Memory control was not found");
    }

    tas::Stop();
    if (!WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; })) p.Problem("the session did not stop");
    if (tas::GetStatus().snapshots) p.Problem("the snapshots weren't let go of when the session stopped");

    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnRenderThread([&] {
        atEnd->Set(atEndWas);
        instant->Set(instantWas);
        gui::visible = true;
    });
    if (const std::string why = WaitPlayerReady(); !why.empty()) p.Problem("after the session: " + why);
    WaitTicks(10);
    p.Check(1, true);
    End(s);
}

// The TAS's workflow helpers, with a "now" macro recorded on the spot:
//   1. a recording is autosaved when it stops, and counts as not saved until it is
//   2. bookmarks survive a save and a load (and count as changes)
//   3. "At the end: Loop" plays it again from the start
//   4. while a session runs, its keys don't reach module hotkeys (No Clip and TAS pause both on F5 by default)
// The user's macro, settings and files are put back.
void TestTasWorkflow()
{
    Step& s = Begin("TAS workflow");
    Probe p(s);
    const auto fail = [&](const std::string& why) {
        p.Problem(why);
        p.Check(1, false);
        End(s);
    };
    if (!tas::Available()) return fail("the TAS engine hooks were not found");
    Module* tasModule = nullptr;
    Module* noClip = nullptr;
    modules::ForEach([&](modules::Tab&, Module& m) {
        if (m.Name() == "TAS / Macro") tasModule = &m;
        if (m.Name() == "No Clip") noClip = &m;
    });
    auto* atEnd = tasModule ? dynamic_cast<ctl::Combo*>(tasModule->FindSetting("At the end")) : nullptr;
    auto* pauseKey = tasModule ? dynamic_cast<ctl::KeyHold*>(tasModule->FindSetting("Pause / resume")) : nullptr;
    if (!atEnd || !pauseKey) return fail("the TAS module's At the end / Pause key controls were not found");
    const int atEndWas = atEnd->Get();
    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; });

    namespace fs = std::filesystem;
    const auto newestAutosave = [] {
        fs::path newest;
        fs::file_time_type at{};
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(tas::AutosaveFolder(), ec))
            if (e.is_regular_file(ec) && e.last_write_time(ec) > at) { at = e.last_write_time(ec); newest = e.path(); }
        return newest;
    };
    const fs::path autosaveBefore = newestAutosave();
    const auto idle = [] { return tas::GetStatus().mode == tas::Mode::Idle; };

    // 1. Record a second, stop
    OnRenderThread([] { gui::visible = false; });
    tas::Record(tas::StartKind::Now, "", "", "");
    if (!WaitFor(10.0, [] { const auto st = tas::GetStatus(); return st.mode == tas::Mode::Recording && st.frame >= 60; }))
        p.Problem("the recording didn't get going");
    tas::Stop();
    WaitFor(5.0, idle);
    const fs::path autosave = newestAutosave();
    if (autosave.empty() || autosave == autosaveBefore) p.Problem("the recording was not autosaved");
    else s.notes.push_back("autosaved as " + autosave.filename().string());
    if (!tas::Unsaved()) p.Problem("a new recording doesn't count as not saved");

    // 2. Bookmarks through a save and a load
    const fs::path file = tas::Folder() / "memenu self-test.merp";
    tas::WithReplay([](tas::Replay& r) { r.SetMarker(10, "self-test mark"); });
    std::string error;
    if (!tas::SaveFile(file, &error)) p.Problem("saving failed: " + error);
    else if (tas::Unsaved()) p.Problem("still not saved after saving");
    tas::WithReplay([](tas::Replay& r) { r.EraseMarker(10); });
    if (!tas::Unsaved()) p.Problem("deleting a bookmark doesn't count as a change");
    if (!tas::LoadFile(file, &error)) p.Problem("loading failed: " + error);
    bool marked = false;
    WaitFor(5.0, [&] {
        tas::WithReplay([&](tas::Replay& r) { const auto* m = r.MarkerAt(10); marked = m && m->name == "self-test mark"; });
        return marked;
    });
    if (!marked) p.Problem("the bookmark was not in the saved file");
    else if (tas::Unsaved()) p.Problem("a macro just loaded counts as not saved");
    std::error_code ec;
    fs::remove(file, ec);

    // (Undo is the TAS mod's own, in its UI: tests/test_history.cpp covers it)

    // 3. Loop, and 4. the session's keys while it plays. The timeline and the replay editor are open meanwhile (drawn
    // with a macro running: they must not trip over it).
    auto* timelineBox = dynamic_cast<ctl::Checkbox*>(tasModule->FindSetting("Timeline"));
    auto* editorBox = dynamic_cast<ctl::Checkbox*>(tasModule->FindSetting("Replay editor"));
    const bool timelineWas = timelineBox && timelineBox->Get(), editorWas = editorBox && editorBox->Get();
    OnRenderThread([&] {
        atEnd->Set(static_cast<int>(tas::AtEnd::Loop));
        gui::visible = true;
        if (timelineBox) timelineBox->Set(true);
        if (editorBox) editorBox->Set(true);
    });
    WaitTicks(2);
    tas::Play();
    bool nearEnd = false, looped = false;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline && !g_aborted && !looped) {
        const auto st = tas::GetStatus();
        if (st.mode == tas::Mode::Playing && !st.armed && st.frames > 0) {
            if (st.frame + 5 >= st.frames) nearEnd = true;
            else if (nearEnd && st.frame < 20) looped = true;
        }
        Sleep(5);
    }
    if (!looped) p.Problem(std::string("Loop didn't play it again (") + (nearEnd ? "reached the end" : "never reached the end") + ")");
    else s.notes.push_back("looped back to the start");

    if (noClip && tas::GetStatus().mode == tas::Mode::Playing) {
        const int vk = pauseKey->Get(), keyWas = noClip->key;
        const bool onWas = noClip->Enabled();
        bool switched = false;
        OnRenderThread([&] {
            noClip->key = vk;               // The same key as the TAS's pause, like the defaults
            modules::OnKey(vk);
            switched = noClip->Enabled() != onWas;
            if (switched) noClip->SetEnabled(onWas);
            noClip->key = keyWas;
        });
        if (switched) p.Problem("the TAS's pause key also switched No Clip during a session");
        else s.notes.push_back("the pause key didn't reach No Clip during the session");
    }
    tas::Stop();
    WaitFor(5.0, idle);

    // Back as it was
    if (!autosave.empty() && autosave != autosaveBefore) fs::remove(autosave, ec);
    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnRenderThread([&] {
        atEnd->Set(atEndWas);
        if (timelineBox) timelineBox->Set(timelineWas);
        if (editorBox) editorBox->Set(editorWas);
        gui::visible = true;
    });
    WaitTicks(3);
    p.Check(1, true);
    End(s);
}

// Input Overlay is a native first-party mod (mods/memenu-input-overlay); the monitor it reads is the runtime's
// The input overlay during a TAS playback: a macro holds the forward key for half a second; the input the engine gets
// (what the overlay draws) must show it held while it plays and let go after. The overlay writes its layouts on first use.
void TestInputOverlay()
{
    Step& s = Begin("Input overlay");
    Probe p(s);
    const auto fail = [&](const std::string& why) {
        p.Problem(why);
        p.Check(1, false);
        End(s);
    };
    if (!tas::input::Installed()) return fail("the engine's input monitor isn't installed");
    Module* overlay = nullptr;
    modules::ForEach([&](modules::Tab&, Module& m) { if (m.Name() == "Input Overlay") overlay = &m; });
    if (!overlay) return fail("the Input Overlay module was not found");
    const bool enabledWas = overlay->Enabled();
    OnRenderThread([&] { overlay->SetEnabled(true); });

    std::string forward;
    OnGameThread([&] { forward = KeyFor(game::Controller(), "GBA_MoveForward", "W"); });
    std::string key = forward;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    tas::Macro m(60.f);
    m.Describe("memenu self-test: input overlay").Wait(5).Press(forward).Wait(30).Release(forward).Wait(15);
    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; r = m.Build(); });
    OnRenderThread([] { gui::visible = false; });
    const auto pressesOf = [&key](const tas::input::Snapshot& sn) {
        const auto it = sn.keys.find(key);
        return it == sn.keys.end() ? 0u : it->second.presses;
    };
    const unsigned pressesBefore = pressesOf(tas::input::Get());
    tas::Play();

    bool started = false, sawHeld = false;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline && !g_aborted) {
        const auto st = tas::GetStatus();
        if (st.mode == tas::Mode::Playing) started = true;
        if (started && (st.mode != tas::Mode::Playing || st.frame + 1 >= st.frames)) break;
        const auto snap = tas::input::Get();
        if (const auto it = snap.keys.find(key); it != snap.keys.end() && it->second.held) sawHeld = true;
        Sleep(2);
    }
    tas::Stop();
    WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; });
    WaitTicks(5);
    const auto after = tas::input::Get();
    const auto it = after.keys.find(key);
    const unsigned pressed = pressesOf(after) - pressesBefore;
    if (!started) p.Problem("the macro didn't play");
    else if (pressed != 1) p.Problem("the overlay counted " + std::to_string(pressed) + " presses of " + forward + ", not 1");
    else if (it != after.keys.end() && it->second.held) p.Problem(forward + " still shows as held after the playback");
    else s.notes.push_back(forward + " pressed once during the playback" + (sawHeld ? " (seen held)" : "") +
                           ", let go after");

    std::error_code ec;
    // Input Overlay is a native first-party mod: its layouts are in its own folder
    if (!std::filesystem::exists(paths::Dir() / "mods" / "memenu-input-overlay" / "input_layouts" / "Movement.json", ec))
        p.Problem("the layout files were not written (mods\\memenu-input-overlay\\input_layouts\\Movement.json)");

    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnRenderThread([&] {
        overlay->SetEnabled(enabledWas);
        gui::visible = true;
    });
    WaitTicks(3);
    p.Check(1, true);
    End(s);
}

// Desync prevention and fixing, with a macro that starts at the self-test's checkpoint (only those are checked):
//   1. the first playback records the reference player states, with the macro's recorded mouse sensitivity set for it
//      (and the player's own back afterwards)
//   2. the recorded positions are moved 10 units from frame 30 on: with "On desync: Pause" it stops on frame 30 and
//      says the position is off by 10
//   3. with "Correct small drift" it plays to the end, putting the player back on the recording
void TestTasDesync()
{
    Step& s = Begin("TAS desync fixing");
    Probe p(s);
    const auto fail = [&](const std::string& why) {
        p.Problem(why);
        p.Check(1, false);
        End(s);
    };
    if (!tas::Available()) return fail("the TAS engine hooks were not found");

    const auto maps = levels::Maps();
    int index = -1;
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (Lower(maps[i].label).find(Lower(g_options.map)) != std::string::npos) { index = i; break; }
    if (index < 0) return fail("no level matching '" + g_options.map + "'");
    const int cp = std::min(g_options.checkpoint, static_cast<int>(maps[index].checkpoints.size()));

    Module* tasModule = nullptr;
    modules::ForEach([&](modules::Tab&, Module& m) { if (m.Name() == "TAS / Macro") tasModule = &m; });
    auto* onDesync = tasModule ? dynamic_cast<ctl::Combo*>(tasModule->FindSetting("On desync")) : nullptr;
    auto* match = tasModule ? dynamic_cast<ctl::Checkbox*>(tasModule->FindSetting("Match the recording")) : nullptr;
    if (!onDesync || !match) return fail("the TAS module's On desync / Match the recording controls were not found");
    const int onDesyncWas = onDesync->Get();
    const bool matchWas = match->Get();

    std::string forward;
    float sens = 0.f;
    OnGameThread([&] {
        auto* pc = game::Controller();
        forward = KeyFor(pc, "GBA_MoveForward", "W");
        if (pc && pc->PlayerInput) sens = pc->PlayerInput->MouseSensitivity;
    });
    const float recordedSens = sens + 1.f;

    tas::Macro m(60.f);
    m.Describe("memenu self-test: desync fixing").Wait(5).Press(forward).Wait(60).Release(forward).Wait(10);
    tas::Replay macro = m.Build();
    macro.start = tas::StartKind::Checkpoint;
    macro.map = maps[index].label;
    macro.checkpoint = cp > 0 ? maps[index].checkpoints[static_cast<std::size_t>(cp - 1)] : std::string();
    macro.syncValidUntil = 0;                       // the first playback records the reference
    char text[32];
    std::snprintf(text, sizeof text, "%.4f", recordedSens);
    macro.extensions["env.sensitivity"] = text;
    macro.extensions["env.invertMouse"] = "0";
    macro.extensions["env.blockStreaming"] = "1";

    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; r = macro; });
    OnRenderThread([&] {
        gui::visible = false;
        onDesync->Set(static_cast<int>(tas::OnDesync::Pause));
        match->Set(true);
    });
    WaitTicks(3);

    // Plays it (loading the checkpoint), until it ends or pauses; the status then
    float sensDuring = 0.f;
    const auto play = [&]() {
        tas::Play();
        bool started = false, sampled = false;
        tas::Status st{};
        const ULONGLONG deadline = GetTickCount64() + 90000;
        while (GetTickCount64() < deadline && !g_aborted) {
            st = tas::GetStatus();
            if (st.mode == tas::Mode::Playing && !st.armed) started = true;
            if (started && !sampled && st.frame > 10 && !st.paused) {
                OnGameThread([&] {
                    if (auto* pc = game::Controller(); pc && pc->PlayerInput) sensDuring = pc->PlayerInput->MouseSensitivity;
                }, 1.0);
                sampled = true;
            }
            if (started && (st.mode != tas::Mode::Playing || st.paused || st.frame >= st.frames)) break;
            Sleep(20);
        }
        tas::Stop();
        WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; });
        return std::make_pair(started, st);
    };

    // 1. The reference
    const auto [ran, first] = play();
    std::size_t recorded = 0;
    tas::WithReplay([&](tas::Replay& r) { recorded = r.syncValidUntil; });
    float sensAfter = 0.f;
    OnGameThread([&] {
        if (auto* pc = game::Controller(); pc && pc->PlayerInput) sensAfter = pc->PlayerInput->MouseSensitivity;
    });
    if (!ran) p.Problem("the first playback never started: " + tas::GetStatus().message);
    else if (first.desyncFrame >= 0) p.Problem("the first playback desynced, with nothing to compare against yet");
    else if (recorded < macro.FrameCount()) p.Problem("the first playback recorded " + std::to_string(recorded) + " of " +
                                                      std::to_string(macro.FrameCount()) + " reference states");
    if (std::abs(sensDuring - recordedSens) > 1e-3f)
        p.Problem("playback didn't use the recorded mouse sensitivity (" + std::to_string(sensDuring) + ", recorded " +
                  std::to_string(recordedSens) + ")");
    if (std::abs(sensAfter - sens) > 1e-3f)
        p.Problem("the player's own mouse sensitivity wasn't restored (" + std::to_string(sensAfter) + ", was " +
                  std::to_string(sens) + ")");

    // 2. Off by 10 from frame 30: Pause stops there and says so
    tas::WithReplay([&](tas::Replay& r) {
        for (std::size_t f = 30; f < r.sync.size(); ++f) r.sync[f].x += 10.f;
    });
    const auto [ran2, paused] = play();
    if (!ran2) p.Problem("the second playback never started");
    else if (paused.desyncFrame != 30)
        p.Problem("with the recording moved on frame 30, the desync was found on " + std::to_string(paused.desyncFrame));
    else if (paused.desyncInfo.find("position off by 10") == std::string::npos)
        p.Problem("the desync was described as '" + paused.desyncInfo + "'");
    else s.notes.push_back("paused at frame 30: " + paused.desyncInfo);

    // 3. Correct puts the player back and plays to the end
    OnRenderThread([&] { onDesync->Set(static_cast<int>(tas::OnDesync::Correct)); });
    WaitTicks(3);
    const auto [ran3, corrected] = play();
    if (!ran3) p.Problem("the third playback never started");
    else if (corrected.desyncFrame >= 0)
        p.Problem("Correct didn't fix the desync on frame " + std::to_string(corrected.desyncFrame) + ": " +
                  corrected.desyncInfo);
    else if (corrected.corrections == 0) p.Problem("Correct made no corrections");
    else if (corrected.frame < macro.FrameCount()) p.Problem("Correct stopped on frame " + std::to_string(corrected.frame));
    else s.notes.push_back("corrected " + std::to_string(corrected.corrections) + " frames, played to the end");

    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnRenderThread([&] {
        onDesync->Set(onDesyncWas);
        match->Set(matchWas);
        gui::visible = true;
    });
    WaitTicks(5);
    p.Check(1, true);
    End(s);
}
#endif

#if MEMENU_FEATURE_TAS
// Not a check: measures what memory snapshots of the game would hold (opt-in: --only "TAS memory survey"). How much of
// the process is the game's, how much of it changes in one frame and in a second, which threads there are and what
// switching the rendering thread off does. Everything goes to the notes and the log.
void TestTasMemorySurvey()
{
    using namespace tas::memmap;
    Step& s = Begin("TAS memory survey");
    Probe p(s);
    const auto note = [&](std::string text) {
        spdlog::info("Survey: {}", text);
        s.notes.push_back(std::move(text));
    };
    const auto mb = [](std::uint64_t bytes) { return std::to_string(bytes / (1024 * 1024)) + " MB"; };
    if (!Tracking()) note("the allocation tracker is not running: virtual memory can't be attributed");

    // Threads
    const auto threadLines = [&](const char* when) {
        std::map<std::string, int> byModule;
        for (const auto& t : Threads()) {
            ++byModule[t.startModule];
            spdlog::info("Survey: thread {} start {}+{:#x} state {} wait {} stack {:#x}-{:#x} cycles {}", t.id,
                         t.startModule, t.start, t.state, t.waitReason, t.stackLow, t.stackHigh, t.cycles);
        }
        std::string line = std::string("threads ") + when + ":";
        for (const auto& [m, n] : byModule) line += " " + m + " x" + std::to_string(n);
        note(line);
    };
    threadLines("now");

    // The game's own threads: how busy each is over a second, and where it is (a few stack samples)
    {
        std::map<std::uint32_t, std::uint64_t> cycles0;
        for (const auto& t : Threads()) cycles0[t.id] = t.cycles;
        Wait(1.0);
        for (const auto& t : Threads()) {
            if (t.startModule != "mirrorsedge.exe" && t.startModule.rfind("physx", 0) != 0) continue;
            const double mcycles = static_cast<double>(t.cycles - cycles0[t.id]) / 1e6;
            note("thread " + std::to_string(t.id) + " (" + t.startModule + ") " + std::to_string(static_cast<int>(mcycles)) +
                 " Mcycles/s, state " + std::to_string(t.state) + " wait " + std::to_string(t.waitReason));
            if (tas::engine::IsGameThread()) continue;
            for (int sample = 0; sample < 3; ++sample) {
                std::string line = "  ";
                for (const auto& w : StackSample(t.id, 10)) line += w + " ";
                spdlog::info("Survey: thread {} sample {}: {}", t.id, sample, line);
                if (sample == 0) s.notes.push_back(line);
                Wait(0.05);
            }
        }
    }
    // Every heap: its size, and every module whose data holds its handle (who uses it)
    {
        const auto modules = Modules();
        for (const auto& h : Heaps()) {
            std::uint64_t bytes = 0, blocks = 0;
            if (HeapLock(h.handle)) {
                PROCESS_HEAP_ENTRY e{};
                while (HeapWalk(h.handle, &e))
                    if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) { bytes += e.cbData; ++blocks; }
                HeapUnlock(h.handle);
            }
            std::string refs;
            const auto value = reinterpret_cast<std::uintptr_t>(h.handle);
            for (const auto& m : modules) {
                for (std::uintptr_t q = m.base; q < m.base + m.size;) {
                    MEMORY_BASIC_INFORMATION mbi{};
                    if (!VirtualQuery(reinterpret_cast<void*>(q), &mbi, sizeof mbi)) break;
                    const auto lo = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
                    const auto hi = std::min<std::uintptr_t>(lo + mbi.RegionSize, m.base + m.size);
                    bool found = false;
                    if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY)) && !(mbi.Protect & PAGE_GUARD))
                        for (auto a = lo; a + 4 <= hi && !found; a += 4)
                            found = *reinterpret_cast<const std::uintptr_t*>(a) == value;
                    if (found) { refs += " " + m.name; break; }
                    q = hi;
                }
            }
            char text[256];
            std::snprintf(text, sizeof text, "heap %p %s%s: %llu KB in %llu blocks; referenced by:%s", h.handle,
                          h.what.c_str(), h.game ? " (game)" : "", static_cast<unsigned long long>(bytes / 1024),
                          static_cast<unsigned long long>(blocks), refs.empty() ? " nobody" : refs.c_str());
            note(text);
        }
    }

    // Regions by owner, and the biggest kinds
    std::vector<Region> regions;
    double regionsMs = 0.0;
    OnGameThread([&] {
        const ULONGLONG t0 = GetTickCount64();
        regions = Regions();
        regionsMs = static_cast<double>(GetTickCount64() - t0);
    }, 30.0);
    std::map<std::string, std::uint64_t> byOwner, byWhat;
    for (const auto& r : regions) {
        byOwner[OwnerName(r.owner)] += r.size;
        byWhat[std::string(OwnerName(r.owner)) + ": " + r.what] += r.size;
    }
    std::string owners = "writable memory (" + std::to_string(regions.size()) + " regions, listed in " +
                         std::to_string(static_cast<int>(regionsMs)) + " ms):";
    for (const auto& [o, b] : byOwner) owners += " " + o + " " + mb(b) + ";";
    note(owners);
    std::vector<std::pair<std::uint64_t, std::string>> whats;
    for (const auto& [w, b] : byWhat) whats.emplace_back(b, w);
    std::sort(whats.rbegin(), whats.rend());
    for (std::size_t i = 0; i < whats.size() && i < 25; ++i) note("  " + whats[i].second + ": " + mb(whats[i].first));

    std::string callers = "VirtualAlloc callers since the TAS started (bytes, live):";
    int shown = 0;
    for (const auto& c : AllocationCallers())
        if (c.bytes && shown++ < 12) callers += " " + c.module + " " + mb(c.bytes) + "/" + mb(c.live) + ";";
    note(callers);

    // How much changes: page hashes of the game's memory and of the unattributed memory, now, a frame later, a second later
    using Hashes = std::unordered_map<std::uintptr_t, std::pair<std::uint64_t, std::uint8_t>>;   // page -> hash, kind
    const auto kindOf = [](const Region& r) -> int {
        if (r.owner == Owner::GameImage) return 0;
        if (r.owner == Owner::GameHeap) return 1;
        if (r.owner == Owner::GameVirtual) return 2;
        if (r.what == "virtual (unknown)") return 3;
        return -1;
    };
    const char* kindNames[] = {"game image", "game heap", "game virtual", "unknown virtual"};
    const auto hashAll = [&](Hashes& out, double& ms) {
        OnGameThread([&] {
            const ULONGLONG t0 = GetTickCount64();
            out.clear();
            for (const auto& r : Regions()) {
                const int kind = kindOf(r);
                if (kind < 0) continue;
                for (std::uintptr_t a = r.base; a < r.base + r.size; a += kPage) {
                    std::uint64_t h = 0;
                    if (HashPage(reinterpret_cast<const void*>(a), h)) out[a] = {h, static_cast<std::uint8_t>(kind)};
                }
            }
            ms = static_cast<double>(GetTickCount64() - t0);
        }, 60.0);
    };
    const auto compare = [&](const Hashes& a, const Hashes& b, const char* when) {
        std::uint64_t changed[4]{}, total[4]{}, added[4]{};
        for (const auto& [page, hk] : b) {
            total[hk.second] += kPage;
            const auto it = a.find(page);
            if (it == a.end()) added[hk.second] += kPage;
            else if (it->second.first != hk.first) changed[hk.second] += kPage;
        }
        std::string line = std::string("changed ") + when + ":";
        for (int k = 0; k < 4; ++k)
            line += std::string(" ") + kindNames[k] + " " + std::to_string(changed[k] / 1024) + " KB of " + mb(total[k]) +
                    " (+" + std::to_string(added[k] / 1024) + " KB new);";
        note(line);
    };
    Hashes h0, h1, h2;
    double ms0 = 0, ms1 = 0, ms2 = 0;
    hashAll(h0, ms0);
    WaitTicks(1);
    hashAll(h1, ms1);
    compare(h0, h1, "over 1 tick");
    WaitTicks(62);
    hashAll(h2, ms2);
    compare(h1, h2, "over 62 ticks");
    note("hashing took " + std::to_string(static_cast<int>(ms0)) + " / " + std::to_string(static_cast<int>(ms1)) + " / " +
         std::to_string(static_cast<int>(ms2)) + " ms");

    // The rendering thread: off and back on with the engine's own console command
    std::set<std::uint32_t> before, after;
    for (const auto& t : Threads()) before.insert(t.id);
    OnGameThread([] { game::ConsoleCommand("ToggleRenderingThread"); });
    WaitTicks(10);
    for (const auto& t : Threads()) after.insert(t.id);
    std::string gone, added;
    for (auto id : before) if (!after.count(id)) gone += " " + std::to_string(id);
    for (auto id : after) if (!before.count(id)) added += " " + std::to_string(id);
    note("ToggleRenderingThread: threads gone [" + gone + " ] new [" + added + " ]");
    threadLines("after the toggle");
    Hashes r0, r1;
    double rm0 = 0, rm1 = 0;
    hashAll(r0, rm0);
    WaitTicks(1);
    hashAll(r1, rm1);
    compare(r0, r1, "over 1 tick without the rendering thread");
    OnGameThread([] { game::ConsoleCommand("ToggleRenderingThread"); });
    WaitTicks(10);
    std::set<std::uint32_t> back;
    for (const auto& t : Threads()) back.insert(t.id);
    note("toggled back: " + std::to_string(back.size()) + " threads (" + std::to_string(before.size()) + " before)");

    p.Check(1, true);
    End(s);
}
#endif

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
            ok = pawn && pawn->Physics == Classes::EPhysics::PHYS_Walking &&
                 pawn->MovementState != Classes::EMovement::MOVE_Jump;
        }, 1.0);
        return ok;
    });
    WaitTicks(5);
}

// Kickglitch Helper is mods/memenu-trainer (helper.lua): what it measured comes from what it shares (kg_helper.*).
// A kick is faked (the wallrun kick's move state on a player standing on the ground: "landed" the next tick), then the
// game is told to jump like the Kickglitch tool does. The helper must count one attempt and time the jump.
void TestKickglitchHelper()
{
    Step& s = Begin("Kickglitch helper");
    Probe p(s);
    Module* helper = modules::Find("Kickglitch Helper");
    if (!helper) {
        p.Problem("the Kickglitch Helper module was not found");
        p.Check(1, false);
        End(s);
        return;
    }
    const bool enabledWas = helper->Enabled();
    WaitGrounded();
    OnRenderThread([&] { helper->SetEnabled(false); helper->SetEnabled(true); });
    WaitTicks(3);

    // The game's real kick move can't be started without a wall (in the open it never ends, and the game then ignores
    // the jump), so only its state is shown, for a few ticks of a real fall. The Kickglitch tool is armed in the air (it
    // does nothing while falling), so on the game's own landing it jumps on the first walking frame, as after a real
    // kick: the helper must time that jump and take it as its calibration.
    Module* tool = modules::Find("Kickglitch");
    auto* frame = dynamic_cast<ctl::SliderInt*>(helper->FindSetting("Perfect frame"));
    const int frameWas = frame ? frame->Get() : 1;
    if (frame) OnRenderThread([&] { frame->Set(5); });   // Must be overwritten by the calibration
    OnGameThread([tool] {
        auto* pawn = game::Pawn();
        if (!pawn) return;
        pawn->Location.Z += 150.f;
        pawn->Velocity = Classes::FVector{};
        pawn->Physics = Classes::EPhysics::PHYS_Falling;
        mirage::engine::SetMove(pawn, Classes::EMovement::MOVE_Falling);
        if (tool) tool->SetEnabled(true);
    });
    WaitTicks(1);
    OnGameThread([] { if (auto* pawn = game::Pawn()) pawn->MovementState = Classes::EMovement::MOVE_MeleeWallrun; });
    WaitTicks(2);
    OnGameThread([] { if (auto* pawn = game::Pawn()) pawn->MovementState = Classes::EMovement::MOVE_Falling; });
    struct Last { int attempts, jumpFrame; bool calibrated; };
    const auto get = [] {
        return Last{SharedInt("memenu-trainer", "kg_helper.attempts"), SharedInt("memenu-trainer", "kg_helper.jump_frame"),
                    SharedInt("memenu-trainer", "kg_helper.calibrated") != 0};
    };
    WaitFor(5.0, [&] { return get().attempts > 0; });

    const Last last = get();
    if (last.attempts != 1) p.Problem("it counted " + std::to_string(last.attempts) + " attempts, not 1");
    else if (last.jumpFrame < 0) p.Problem("it saw the kick and the landing but not the jump");
    else s.notes.push_back("jump timed " + std::to_string(last.jumpFrame) + " frame(s) after landing (the Kickglitch tool's jump)");
    if (!tool) p.Problem("the Kickglitch tool was not found (no calibration)");
    else if (last.jumpFrame >= 0 && !last.calibrated)
        p.Problem("the attempt was not taken as the Kickglitch tool's (no calibration)");
    else if (frame && last.jumpFrame >= 0 && frame->Get() != std::clamp(last.jumpFrame, 0, 5))
        p.Problem("the tool's attempt did not calibrate the perfect frame (" + std::to_string(frame->Get()) + ")");

    OnRenderThread([&] {
        if (tool) tool->SetEnabled(false);
        if (frame) frame->Set(frameWas);
        helper->SetEnabled(enabledWas);
    });
    OnGameThread([] {
        if (auto* pawn = game::Pawn(); pawn && game::Controller()) player_state::ResetMovement(pawn, game::Controller());
    });
    WaitGrounded();     // Before the next test
    p.Check(1, false);
    End(s);
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

// Auto Kickglitch is a first-party mod (mods/memenu-trainer). Same faked kick as the helper's test (the kick's state for a few ticks of a real fall): on the game's landing the
// module must jump by itself, with nothing else armed.
void TestAutoKickglitch()
{
    Step& s = Begin("Auto kickglitch");
    Probe p(s);
    Module* autoKg = modules::Find("Auto Kickglitch");
    if (!autoKg) {
        p.Problem("the Auto Kickglitch module was not found");
        p.Check(1, false);
        End(s);
        return;
    }
    const bool enabledWas = autoKg->Enabled();
    WaitGrounded();     // A test before may have left the player in the air
    OnRenderThread([&] { autoKg->SetEnabled(true); });
    OnGameThread([] {
        auto* pawn = game::Pawn();
        if (!pawn) return;
        pawn->Location.Z += 150.f;
        pawn->Velocity = Classes::FVector{};
        pawn->Physics = Classes::EPhysics::PHYS_Falling;
        mirage::engine::SetMove(pawn, Classes::EMovement::MOVE_Falling);
    });
    WaitTicks(1);
    OnGameThread([] { if (auto* pawn = game::Pawn()) pawn->MovementState = Classes::EMovement::MOVE_MeleeWallrun; });
    WaitTicks(2);
    OnGameThread([] { if (auto* pawn = game::Pawn()) pawn->MovementState = Classes::EMovement::MOVE_Falling; });

    // Landed, then up again on its own: the jump
    bool landed = false, jumped = false;
    WaitFor(5.0, [&] {
        OnGameThread([&] {
            auto* pawn = game::Pawn();
            if (!pawn) return;
            if (pawn->Physics == Classes::EPhysics::PHYS_Walking) landed = true;
            else if (landed && (pawn->MovementState == Classes::EMovement::MOVE_Jump || pawn->Velocity.Z > 100.f))
                jumped = true;
        }, 1.0);
        return jumped;
    });
    if (!landed) p.Problem("the player never landed");
    else if (!jumped) p.Problem("no jump after the kick's landing");

    // The kick buffer: a kick pressed in the air (the console runs AttackPress the way a key binding does) is held
    // back, then let go late when no wall comes within the window. The wall kick itself needs a real wall.
    auto* buffer = dynamic_cast<ctl::Checkbox*>(autoKg->FindSetting("Kick buffer"));
    auto* window = dynamic_cast<ctl::SliderInt*>(autoKg->FindSetting("Buffer window"));
    auto* onMiss = dynamic_cast<ctl::Combo*>(autoKg->FindSetting("No wall in time"));
    if (!buffer || !window || !onMiss) {
        p.Problem("the kick buffer settings were not found");
    } else {
        const bool bufferWas = buffer->Get();
        const int windowWas = window->Get(), onMissWas = onMiss->Get();
        OnRenderThread([&] {
            buffer->Set(true);
            window->Set(100);
            onMiss->Set(0);         // Kick anyway
        });
        WaitGrounded();
        const auto before = KickBuffer();
        OnGameThread([] {
            auto* pawn = game::Pawn();
            if (!pawn) return;
            pawn->Location.Z += 400.f;
            pawn->Velocity = Classes::FVector{};
            pawn->Physics = Classes::EPhysics::PHYS_Falling;
            mirage::engine::SetMove(pawn, Classes::EMovement::MOVE_Falling);
        });
        WaitTicks(2);
        bool airKick = false;
        OnGameThread([&] {
            game::ConsoleCommand("AttackPress");
            game::ConsoleCommand("AttackRelease");
            if (auto* pawn = game::Pawn()) airKick = pawn->MovementState == Classes::EMovement::MOVE_MeleeAir;
        });
        WaitTicks(1);
        const auto held = KickBuffer();
        WaitFor(3.0, [&] {
            const auto st = KickBuffer();
            return st.late + st.dropped > before.late + before.dropped;
        });
        const auto after = KickBuffer();
        char note[160];
        std::snprintf(note, sizeof note, "kick buffer: hooked %s, held %d, let go late %d, dropped %d",
                      after.hooked ? "yes" : "no", after.held - before.held, after.late - before.late,
                      after.dropped - before.dropped);
        s.notes.push_back(note);
        if (!after.hooked) p.Problem("the kick press can't be held back (AttackPress not watched)");
        else if (held.held <= before.held) p.Problem("a kick pressed in the air was not held back");
        else if (airKick) p.Problem("the held kick still kicked in the air at once");
        else if (after.late <= before.late) p.Problem("the held kick was not let go when no wall came");
        OnRenderThread([&] {
            buffer->Set(bufferWas);
            window->Set(windowWas);
            onMiss->Set(onMissWas);
        });
    }

    OnRenderThread([&] { autoKg->SetEnabled(enabledWas); });
    OnGameThread([] {
        if (auto* pawn = game::Pawn(); pawn && game::Controller()) player_state::ResetMovement(pawn, game::Controller());
    });
    WaitGrounded();     // Before the next test
    p.Check(1, false);
    End(s);
}

// Kill Bots is a first-party mod (mods/memenu-trainer). Only checks the kills where the level has enemies (start game-test with --map at one); elsewhere, that it runs.
void TestKillBots()
{
    Step& s = Begin("Kill bots");
    Probe p(s);
    Module* kill = modules::Find("Kill Bots");
    if (!kill) {
        p.Problem("the Kill Bots module was not found");
        p.Check(1, false);
        End(s);
        return;
    }
    const auto alive = [] {
        int n = 0;
        OnGameThread([&] {
            auto* world = game::World();
            for (Classes::APawn* pawn = world ? world->PawnList : nullptr; pawn; pawn = pawn->NextPawn)
                if (pawn != game::Pawn() && game::IsA(pawn, Classes::ATdBotPawn::StaticClass()) && pawn->Health > 0 &&
                    !pawn->bDeleteMe)
                    ++n;
        });
        return n;
    };
    const int before = alive();
    OnRenderThread([&] { kill->Toggle(); });
    WaitTicks(10);
    const int after = alive();
    if (before == 0) s.notes.push_back("no enemies in this level: only checked that it runs");
    else if (after > 0) p.Problem(std::to_string(after) + " of " + std::to_string(before) + " enemies still alive");
    else s.notes.push_back("killed " + std::to_string(before) + (before == 1 ? " enemy" : " enemies"));
    p.Check(1, false);
    End(s);
}

// ---- Engine hooks ---------------------------------------------------------------------------------------------------
Step& BeginEngine(const char* name, Probe*& probe, std::unique_ptr<Probe>& holder)
{
    Step& s = Begin(name);
    holder = std::make_unique<Probe>(s);
    probe = holder.get();
    return s;
}

// Strang is the state handler hook's user: armed, a new move (falling, here) must make it jump and disarm itself.
void TestEngineStateHook()
{
    Probe* p;
    std::unique_ptr<Probe> holder;
    Step& s = BeginEngine("Engine state hook", p, holder);
    Module* strang = modules::Find("Strang");
    if (!game::PlayerStateHooked()) p->Problem("the player state handler is not hooked");
    else if (!strang) p->Problem("the Strang module was not found");
    else {
        OnRenderThread([&] { strang->SetEnabled(true); });
        WaitTicks(2);
        OnGameThread([] {
            auto* pawn = game::Pawn();
            if (!pawn) return;
            pawn->Location.Z += 150.f;
            pawn->Velocity = Classes::FVector{};
            pawn->Physics = Classes::EPhysics::PHYS_Falling;
            mirage::engine::SetMove(pawn, Classes::EMovement::MOVE_Falling);
        });
        if (!WaitFor(3.0, [&] { return !strang->Enabled(); })) p->Problem("Strang was still armed after a new move began");
        else s.notes.push_back("Strang fired on the state change");
        OnRenderThread([&] { strang->SetEnabled(false); });
        WaitGrounded();     // Strang jumped
    }
    p->Check(1, false);
    End(s);
}

// Mirage step 9's UI layer, with tests/mods/ui-lua: its window (most widgets, every frame), its module panel, and a
// window and a color it leaves open on purpose, which the runtime closes for it: frames keep coming, and the menu's
// own windows still draw after it.
void TestMirageUi()
{
    Step& s = Begin("Mirage UI layer");
    Probe p(s);
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

// Frame Rate Limit (mods/memenu-visual): on, the engine's cap is the module's; off, the engine has exactly what it had
// before (the game's 62, or the test's own uncapped fast mode): normal play is never left at another frame rate.
void TestFrameRate()
{
    Step& s = Begin("Frame rate limit");
    Probe p(s);
    Module* fps = modules::Find("Frame Rate Limit");
    auto* cap = fps ? dynamic_cast<ctl::Slider*>(fps->FindSetting("Cap")) : nullptr;
    auto* uncapped = fps ? dynamic_cast<ctl::Checkbox*>(fps->FindSetting("Uncapped")) : nullptr;
    if (!fps || !cap || !uncapped) {
        p.Problem("the Frame Rate Limit module or its settings were not found");
        p.Check(1, false);
        End(s);
        return;
    }
    struct Cap { bool smooth = false; float max = 0.f; bool found = false; };
    const auto read = [] {
        Cap c;
        OnGameThread([&] {
            const auto& objects = Classes::UObject::GetGlobalObjects();
            for (std::size_t i = 0; i < objects.Num(); ++i) {
                auto* o = objects.GetByIndex(i);
                if (!o || !game::IsA(o, Classes::UGameEngine::StaticClass()) || o->GetName().rfind("Default__", 0) == 0)
                    continue;
                auto* e = static_cast<Classes::UGameEngine*>(o);
                c = {e->bSmoothFrameRate != 0, e->MaxSmoothedFrameRate, true};
                break;
            }
        });
        return c;
    };
    const bool enabledWas = fps->Enabled(), uncappedWas = uncapped->Get();
    const float capWas = cap->Get();
    if (enabledWas) {
        OnRenderThread([&] { fps->SetEnabled(false); });
        WaitTicks(3);
    }
    const Cap before = read();
    if (!before.found) p.Problem("no game engine object");

    OnRenderThread([&] {
        cap->Set(30.f);
        uncapped->Set(false);
        fps->SetEnabled(true);
    });
    WaitTicks(5);
    const Cap on = read();
    OnRenderThread([&] { fps->SetEnabled(false); });
    WaitTicks(5);
    const Cap after = read();

    char note[200];
    std::snprintf(note, sizeof note, "before: cap %s %.0f; module at 30: %s %.0f; off again: %s %.0f",
                  before.smooth ? "on" : "off", before.max, on.smooth ? "on" : "off", on.max,
                  after.smooth ? "on" : "off", after.max);
    s.notes.push_back(note);
    if (!on.smooth || std::fabs(on.max - 30.f) > 0.01f) p.Problem("the module's 30 fps cap was not set on the engine");
    if (after.smooth != before.smooth || std::fabs(after.max - before.max) > 0.01f)
        p.Problem("switching the module off did not put the engine's cap back");

    OnRenderThread([&] {
        cap->Set(capWas);
        uncapped->Set(uncappedWas);
        fps->SetEnabled(enabledWas);
    });
    WaitTicks(3);
    p.Check(1, false);
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
// function and state of Core, Engine and TdGame. Every difference goes to memenu\mirage-layouts.txt.
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
        // Both sides of the structs named in MEMENU_LAYOUT_DETAIL (comma separated paths), property by property
        out << "\n";
        char* detail = nullptr;
        std::size_t len = 0;
        if (_dupenv_s(&detail, &len, "MEMENU_LAYOUT_DETAIL") == 0 && detail) {
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
    OnGameThread([&] {
        auto* pawn = game::Pawn();
        auto* pc = game::Controller();
        if (!pawn || !pc) return;
        pawnBefore = pawn;
        Classes::FTraceHitInfo hit{};
        pawn->TakeDamage(pawn->Health + 10000, nullptr, pawn->Location, Classes::FVector{},
                         Classes::UTdDmgType_Fell::StaticClass(), hit, nullptr);
    });
#if MEMENU_FEATURE_TAS
    // With the rendering thread off (TAS snapshots) the hooked functions aren't reached: a respawn is the death then
    if (tas::snapshot::RenderingThreadOff()) {
        bool respawned = false;
        WaitFor(10.0, [&] {
            OnGameThread([&] { auto* pawn = game::Pawn(); respawned = pawn && pawn != pawnBefore && pawn->Health > 0; }, 1.0);
            return respawned;
        });
        if (!respawned) p->Problem("the player didn't die and come back (rendering thread off: the hook can't see it)");
        else s.notes.push_back("rendering thread off (TAS snapshots): death seen as a respawn, the hook isn't reached");
        WaitPlayerReady();
        p->Check(1, false);
        End(s);
        return;
    }
#endif
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
            OnGameThread([&] { auto* pw = game::Pawn(); ok = pw && pw != pawnBefore && pw->Health > 0; }, 1.0);
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

// A kept alternate key the game doesn't have (as after a restart) must be put back at the next reset moment (the
// module switched on), on a key nothing else uses. Uses a file of its own, and the game's ini is written back after.
void TestKeepKeyBindings()
{
    Step& s = Begin("Keep key bindings");
    Probe p(s);
    Module* keep = modules::Find("Keep Key Bindings");
    const auto ini = TdInputIni();
    std::string iniWas;
    const bool hadIni = !ini.empty() && std::filesystem::exists(ini);
    if (hadIni) {
        std::ifstream in(ini, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        iniWas = ss.str();
    }
    if (!keep) {
        p.Problem("the Keep Key Bindings module was not found");
        p.Check(1, false);
        End(s);
        return;
    }
    const bool enabledWas = keep->Enabled();
    OnRenderThread([&] { keep->SetEnabled(false); });

    // A key nothing is bound to, and the bindings the game has now
    std::string freeKey;
    std::string list;       // "Key=Command;..." as the mod keeps them
    OnGameThread([&] {
        auto* pc = game::Controller();
        if (!pc || !pc->PlayerInput) return;
        std::set<std::string> used;
        auto& binds = pc->PlayerInput->Bindings;
        for (std::size_t i = 0; i < binds.Num(); ++i) {
            const auto& b = binds[i];
            used.insert(b.Name.GetName());
            if (b.Control || b.Shift || b.Alt || !b.Command.IsValid()) continue;
            const std::string key = b.Name.GetName(), cmd = b.Command.ToString();
            if (cmd.rfind("GBA_", 0) == 0 && key.rfind("GBA_", 0) != 0 && key.rfind("XboxTypeS_", 0) != 0)
                list += key + "=" + cmd + ";";
        }
        for (const char* k : {"NumPadNine", "NumPadEight", "NumPadSeven", "Home", "End", "PageUp"})
            if (!used.count(k)) { freeKey = k; break; }
    });
    if (freeKey.empty()) p.Problem("no free key to test with");
    else {
        // Keep Key Bindings is mods/memenu-misc: it takes this list instead of its own config (and never writes it)
        list += freeKey + "=GBA_Jump";
        mirage::mods::Share("keep_bindings.test", list);
        OnRenderThread([&] { keep->SetEnabled(true); });     // A reset moment: what is missing comes back
        const auto bound = [&] {
            bool found = false;
            OnGameThread([&] {
                auto* pc = game::Controller();
                if (!pc || !pc->PlayerInput) return;
                auto& binds = pc->PlayerInput->Bindings;
                for (std::size_t i = 0; i < binds.Num() && !found; ++i)
                    found = binds[i].Name.GetName() == freeKey && binds[i].Command.IsValid() &&
                            binds[i].Command.ToString() == "GBA_Jump";
            }, 1.0);
            return found;
        };
        if (!WaitFor(5.0, bound)) p.Problem("the kept binding " + freeKey + " = GBA_Jump was not put back");
        else s.notes.push_back("put back " + freeKey + " = GBA_Jump");

        // Undo: the module off first (it would put it back), then the binding gone and the game's ini as it was
        OnRenderThread([&] { keep->SetEnabled(false); });
        mirage::mods::Share("keep_bindings.test", "");
        // Taken out of the list, not blanked: the game saves TdInput.ini from memory later (on a level load), and a
        // blanked entry ("NumPadNine", Command=) would end up in the player's file. Removing needs no allocation: the
        // entries after it move down one and the count drops (its command string is left to leak, a few bytes).
        OnGameThread([&] {
            auto* pc = game::Controller();
            if (!pc || !pc->PlayerInput) return;
            auto& binds = pc->PlayerInput->Bindings;
            struct RawArray { Classes::FKeyBind* data; int count; int max; };
            static_assert(sizeof(RawArray) == sizeof(binds), "TArray layout");
            auto* raw = reinterpret_cast<RawArray*>(&binds);
            for (int i = raw->count - 1; i >= 0; --i) {
                if (raw->data[i].Name.GetName() != freeKey) continue;
                std::memmove(&raw->data[i], &raw->data[i + 1], sizeof(Classes::FKeyBind) * (raw->count - i - 1));
                --raw->count;
            }
        });
        if (bound()) p.Problem("could not remove the test binding again");
    }
    if (hadIni) {
        std::ofstream out(ini, std::ios::binary | std::ios::trunc);
        out << iniWas;
    }
    OnRenderThread([&] { keep->SetEnabled(enabledWas); });
    p.Check(1, false);
    End(s);
}

// Collision View is a native first-party mod (mods/memenu-collision): what it sees comes from what it shares (JSON).
nlohmann::json CollisionShared(const char* key)
{
    std::string v(1 << 16, '\0');
    const uint32_t n = mirage::mods::game_api::ShareGet("memenu-collision", key, v.data(), static_cast<uint32_t>(v.size()));
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

void TestCollisionInspect()
{
    Step& s = Begin("Collision view inspect");
    Probe p(s);
    Module* view = modules::Find("Collision View");
    auto* inspect = view ? dynamic_cast<ctl::Checkbox*>(view->FindSetting("Inspect")) : nullptr;
    if (!view || !inspect) {
        p.Problem("the Collision View module or its Inspect section was not found");
        p.Check(1, false);
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
        p.Problem("the Collision View module or its static geometry settings were not found");
        p.Check(1, false);
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
// (memenu-speedrun/run.*) and the commands the menu hands it (memenu/speedrun.command):
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

void TestSpeedrunTimer()
{
    using namespace speedrun_test;
    Step& s = Begin("Speedrun timer");
    Probe p(s);
    const auto fail = [&](const std::string& why) {
        p.Problem(why);
        p.Check(1, false);
        End(s);
    };

    Module* timer = modules::Find("Speedrun Timer");
    auto* category = timer ? dynamic_cast<ctl::Combo*>(timer->FindSetting("Category")) : nullptr;
    auto* autoStart = timer ? dynamic_cast<ctl::Checkbox*>(timer->FindSetting("Auto start")) : nullptr;
    auto* autoReset = timer ? dynamic_cast<ctl::Checkbox*>(timer->FindSetting("Auto reset")) : nullptr;
    if (!category || !autoStart || !autoReset) return fail("the Speedrun Timer module or its controls were not found");
    constexpr int kIndividualLevel = 3, kAnyPercent = 0;

    const auto maps = levels::Maps();
    int index = -1;
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (Lower(maps[i].label).find(Lower(g_options.map)) != std::string::npos) { index = i; break; }
    if (index < 0) return fail("no level matching '" + g_options.map + "'");
    const int cp = std::min(g_options.checkpoint, static_cast<int>(maps[index].checkpoints.size()));

    const bool enabledWas = timer->Enabled();
    const int categoryWas = category->Get();
    const bool autoStartWas = autoStart->Get(), autoResetWas = autoReset->Get();
    OnRenderThread([&] {
        category->Set(kIndividualLevel);
        autoStart->Set(true);
        autoReset->Set(true);
        if (timer->Enabled()) timer->SetEnabled(false);     // A clean start: no run left from before
        timer->SetEnabled(true);
    });
    WaitTicks(10);
    Command("reset");
    WaitTicks(5);

    if (!WaitFor(10.0, [] { return Get("run.hooked") == "1"; }))
        p.Problem("the level completed event is not hooked (no auto splits): see the log");
    else s.notes.push_back("level completed event hooked");

    // Its record file (in the mod's folder), kept to put back
    const auto file = paths::Dir() / "mods" / "memenu-speedrun" / std::filesystem::path(Get("run.file"));
    std::string fileWas;
    const bool hadFile = std::filesystem::exists(file);
    if (hadFile) {
        std::ifstream in(file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        fileWas = ss.str();
    }

    const auto reload = [&]() -> bool {
        const int before = g_loadsFinished.load();
        OnRenderThread([&] { levels::Load(index, cp, levels::Method::Game, false); });
        if (!WaitFor(120.0, [&] { return g_loadsFinished.load() > before; })) { p.Problem("the level did not reload"); return false; }
        if (!WaitFor(60.0, [] { return game::GetStatus().hasPawn && !game::IsLoading(); })) { p.Problem("no player after reloading"); return false; }
        WaitPlayerReady(30.0);
        return true;
    };

    if (reload()) {
        WaitFor(5.0, [] { return Get("run.phase") == "running" && Num("run.lrt") > 0.1; });
        if (Get("run.phase") != "running")
            p.Problem("no run started when the level loaded (map '" + Get("run.map") + "', last: " + Get("run.event") + ")");
        else {
            s.notes.push_back("started: " + Get("run.event"));
            OnRenderThread([&] { autoReset->Set(false); });
            WaitTicks(3);
            const double lrt0 = Num("run.lrt"), rta0 = Num("run.rta");
            if (reload()) {
                WaitTicks(2);
                const double lrt = Num("run.lrt") - lrt0, rta = Num("run.rta") - rta0;
                char buf[160];
                std::snprintf(buf, sizeof buf, "across a reload: real time +%.2f s, load removed time +%.2f s", rta, lrt);
                s.notes.push_back(buf);
                if (Get("run.phase") != "running") p.Problem("the run stopped on a reload with auto reset off");
                else if (rta - lrt < 0.3) p.Problem(std::string("the load was not removed: ") + buf);
                else if (lrt <= 0.0) p.Problem(std::string("the load removed time did not move: ") + buf);

                const int completedBefore = std::atoi(Get("run.completed").c_str());
                Command("start_or_split");               // The only segment: finishes the run
                WaitTicks(5);
                if (Get("run.phase") != "finished") p.Problem("a split on the only segment did not finish the run");
                else if (std::atoi(Get("run.completed").c_str()) != completedBefore + 1) p.Problem("the finished run was not counted");
                else if (Num("run.pb") < 0.0) p.Problem("the finished run left no personal best");
                else {
                    char fin[96];
                    std::snprintf(fin, sizeof fin, "finished in %.3f s%s", Num("run.lrt"), Get("run.new_pb") == "1" ? ", a personal best" : "");
                    s.notes.push_back(fin);
                }

                Command("undo");
                WaitTicks(5);
                if (Get("run.phase") != "running") p.Problem("undoing the final split did not resume the run");
                else if (std::atoi(Get("run.completed").c_str()) != completedBefore) p.Problem("undoing the final split did not take back the finish");
            }
        }
    }

    // Back as it was: a different category first, so the restored file is read again when the old one comes back
    Command("reset");
    WaitTicks(3);
    OnRenderThread([&] { category->Set(kAnyPercent); });
    WaitTicks(5);
    std::error_code ec;
    if (hadFile) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << fileWas;
    } else std::filesystem::remove(file, ec);
    OnRenderThread([&] {
        category->Set(categoryWas);
        autoStart->Set(autoStartWas);
        autoReset->Set(autoResetWas);
        timer->SetEnabled(enabledWas);
    });
    WaitTicks(5);
    p.Check(1, true);
    End(s);
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

#if MEMENU_GAME
// Mirage step 4: the example mods (examples/mods: hello built by its own CMake project with clang-cl, hello-lua), put in
// selftest-mods by `memenu game-test`, are loaded; their modules are in the menu, run, draw, save and load their
// settings like built-in ones, and keep their own config.
void TestMirageMods()
{
    Step& s = Begin("Mirage mods");
    Probe p(s);
    const auto mods = mirage::mods::List();
    for (const char* id : {"hello", "hello-lua"}) {
        const auto it = std::find_if(mods.begin(), mods.end(), [&](const auto& m) { return m.id == id; });
        if (it == mods.end()) {
            p.Problem(std::string(id) + " was not found (is selftest-mods there? memenu game-test puts it)");
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
    if (lookupNs > 100.0) p.Problem("the detour's lookup costs " + std::to_string(lookupNs) + " ns (budget 100)");
    if (nativeUs > 5.0) p.Problem("a native hook adds " + std::to_string(nativeUs) + " us a call (budget 5)");
    if (luaUs > 50.0) p.Problem("a Lua hook adds " + std::to_string(luaUs) + " us a call (budget 50)");
    s.notes.push_back("map " + (lastMap ? lastMap->Get() : std::string()) + ", " + std::to_string(ticks ? ticks->Get() : 0) + " tick events");
#if MEMENU_FEATURE_TAS
    // A TAS rewind puts back the game's memory only: no mod's code or data is ever counted as the game's
    {
        std::size_t mods = 0;
        for (const auto& m : tas::memmap::Modules()) {
            if (m.path.find("\\selftest-mods\\") == std::string::npos) continue;
            ++mods;
            if (m.game) p.Problem(m.name + " (a mod) is counted as the game's memory: a TAS rewind would put it back");
        }
        if (mods == 0) p.Problem("no mod DLL was found among the process's modules");
        else s.notes.push_back(std::to_string(mods) + " mod DLLs, none in TAS snapshots");
    }
#endif
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
    const std::wstring report = mirage::crash::WriteReport(&ep, "self-test (not a real crash)");
    if (report.empty()) p.Problem("no report was written");
    else {
        const std::string text = mirage::crash::ReadReport(fs::path(report));
        for (const char* part : {"What: access violation", "Where: medge_menu.dll+", "Access: writing 0x00000010", "Call stack", "Modules ("})
            if (text.find(part) == std::string::npos) p.Problem(std::string("the report has no \"") + part + "\"");
        if (!fs::exists(fs::path(report).replace_extension(".dmp"))) p.Problem("no minidump next to the report");
        s.notes.push_back("report: " + fs::path(report).filename().string() + ", " + std::to_string(text.size()) + " bytes");
        std::error_code ec;
        fs::remove(fs::path(report), ec);
        fs::remove(fs::path(report).replace_extension(".dmp"), ec);
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

#if MEMENU_FEATURE_SCRIPT_EDITOR
// The script editor's work without its window: a new mod from the template is made and loaded while the game runs
// (mirage::mods::LoadNew), the check finds an error in unsaved text and says where, a saved error makes the hot reload
// fail with the file and line, and saving the fix loads it again.
void TestScriptEditor()
{
    Step& s = Begin("Mirage script editor");
    Probe p(s);
    namespace mm = mirage::mods;
    namespace fs = std::filesystem;
    const std::string id = "selftest-new";
    const fs::path dir = paths::Dir() / "selftest-mods" / id;
    std::error_code ec;
    fs::remove_all(dir, ec);
    const auto info = [&]() -> std::optional<mm::Info> {
        for (const mm::Info& i : mm::List())
            if (i.id == id) return i;
        return std::nullopt;
    };

    // Parsing what the compilers say
    const auto cs = script_editor::ParseDiagnostics("main.cs(5,17): error CS0029: no\nmain.cs(2,1): warning CS0168: x\n(12 ms)");
    if (cs.size() != 2 || cs[0].line != 5 || cs[0].column != 17 || !cs[0].error || cs[1].error || cs[0].file != "main.cs")
        p.Problem("C# diagnostics parsed wrong (" + std::to_string(cs.size()) + ")");
    const auto lua = script_editor::ParseDiagnostics("main.lua:7: '=' expected near 'x'");
    if (lua.size() != 1 || lua[0].line != 7 || lua[0].file != "main.lua") p.Problem("Lua diagnostics parsed wrong");

    // 1. A new mod, made and loaded now
    if (const std::string why = script_editor::CreateModFiles(dir.parent_path(), id); !why.empty()) {
        p.Problem("making the mod: " + why);
        End(s);
        return;
    }
    if (script_editor::CreateModFiles(dir.parent_path(), id).empty()) p.Problem("a second mod with the same id was made");
    if (script_editor::CreateModFiles(dir.parent_path(), "Bad Id").empty()) p.Problem("an id with spaces was accepted");
    std::string loaded = "timed out";
    const auto t0 = std::chrono::steady_clock::now();
    OnGameThread([&] { loaded = mm::LoadNew(dir); }, 30.0);
    char line[160];
    std::snprintf(line, sizeof line, "new %s mod made and loaded in %.0f ms", mirage::dotnet::ScriptLanguage(),
                  std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    s.notes.push_back(line);
    if (!loaded.empty()) p.Problem("the new mod didn't load: " + loaded);
    if (!WaitFor(3.0, [&] { return modules::Find(id) != nullptr; })) p.Problem("the new mod's module isn't in the menu");
    std::string again = "timed out";
    OnGameThread([&] { again = mm::LoadNew(dir); });
    if (again.empty()) p.Problem("the same folder loaded twice");

    // 2. The check sees unsaved text
    const fs::path main = dir / mirage::dotnet::ScriptFile();
    const std::string good = script_editor::Template(id);
    std::string bad = good;
    const std::string anchor = mirage::dotnet::kEnabled ? "if (!pawn.IsValid) return;" : "on_tick = function()";
    bad.insert(bad.find(anchor), mirage::dotnet::kEnabled ? "int broken = \"text\"; " : "local = ");
    const int badLine = static_cast<int>(std::count(bad.begin(), bad.begin() + static_cast<std::ptrdiff_t>(bad.find(anchor)), '\n')) + 1;
    const auto checked = script_editor::Check(dir, main, bad);
    if (!checked.ran) p.Problem("the check didn't run: " + checked.note);
    else if (checked.Errors() == 0 || checked.diagnostics.front().line != badLine)
        p.Problem("the check of broken text: " + std::to_string(checked.Errors()) + " error(s), first at line " +
                  (checked.diagnostics.empty() ? std::string("-") : std::to_string(checked.diagnostics.front().line)) +
                  " (expected " + std::to_string(badLine) + ")");
    else s.notes.push_back("check of unsaved text: " + checked.diagnostics.front().text + " (" + checked.note + ")");
    if (const auto ok = script_editor::Check(dir, main, good); ok.Errors() != 0) p.Problem("the template doesn't compile: " + ok.diagnostics.front().text);

    // 3. Saved broken: the hot reload fails and says where; saved fixed: it loads again
    const auto write = [&](const std::string& text) { std::ofstream(main, std::ios::binary | std::ios::trunc) << text; };
    write(bad);
    const std::string where = std::string(mirage::dotnet::ScriptFile()) + (mirage::dotnet::kEnabled ? "(" : ":") + std::to_string(badLine);
    if (!WaitFor(10.0, [&] { const auto i = info(); return i && !i->loaded && i->problem.find(where) != std::string::npos; }))
        p.Problem("saving broken code didn't fail the reload with " + where + ": " + (info() ? info()->problem : "gone"));
    write(good);
    if (!WaitFor(10.0, [&] { const auto i = info(); return i && i->loaded; }))
        p.Problem("saving the fix didn't load it again: " + (info() ? info()->problem : "gone"));
    if (const auto i = info()) {
        if (!i->script) p.Problem("the new mod isn't listed as a script mod");
        s.notes.push_back("reloaded " + std::to_string(i->reloads) + "x by saving");
    }
    End(s);
}
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
    const auto respawn = [&] {
        void* before = nullptr;
        float c = 0, pw = 0;
        readSteps(c, pw, &before);
        OnGameThread([] { levels::RestartFromLastCheckpoint(); });
        void* after = before;
        WaitFor(30.0, [&] { readSteps(c, pw, &after); return after && after != before; });
        WaitTicks(10);      // Not WaitPlayerReady: after a restart the controller's bFrozen stays set (engine leftover)
        return after != before;
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

#if MEMENU_FEATURE_TAS
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
    for (int i = 0; i < 60 && out.bounces == 0; ++i)
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
            if (auto* o = objects.GetByIndex(i); o && o->Class == cls) pad = o;
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
                if (auto* o = objects.GetByIndex(i); o && o->Class == cls) pad = o;
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
    for (int i = 0; i < 120 && !pad; ++i)    // About 10 s: the first compile takes 3-4 s
    {
        WaitTicks(5);
        OnGameThread([&] {
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
    float velocityZ = 0.f;
    for (int i = 0; i < 80 && bounces == 0; ++i)
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
                if (i.name == "PadA") out.a = i.actor;
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
    WaitFor(10.0, [&] { reloaded = look(); return reloaded.spawned >= 2 && reloaded.a != again.a; });
    if (reloaded.spawned != 2 || reloaded.a == again.a) p.Problem("the hot reload didn't replace the layer's actors");
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
            p.Problem("the Restart Checkpoint module (memenu-levels) is missing");
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

// A map made with the game's own 2008 editor (step 13's milestone): tests/mods/map-test's maps/MirageEditorRoom.me1
// (a room, a light and a player start, built in UnrealEd: tests/maps-src/MirageEditorRoom) is in the level select and
// loads with the player standing in the room. A TAS walking forward records, then plays back verified, the player
// having walked (not fallen) somewhere else in the room. The room has no checkpoints: restarting it is loading its start
// again, which each TAS run does (the game's Restart Checkpoint needs a checkpoint: without one it leaves no player).
void TestEditorMap()
{
    Step& s = Begin("Mirage editor map");
    Probe p(s);
    const auto maps = levels::Maps();
    int index = -1;
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (maps[static_cast<std::size_t>(i)].file == "MirageEditorRoom") index = i;
    if (index < 0 || !tas::Available()) {
        p.Problem(index < 0 ? "MirageEditorRoom isn't in the Levels list (is map-test loaded?)" : "the TAS isn't available");
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

        // A TAS from the level start: walks forward, recorded, then played back verified
        std::string forward;
        OnGameThread([&] { forward = KeyFor(game::Controller(), "GBA_MoveForward", "W"); });
        tas::Replay saved;
        tas::WithReplay([&](tas::Replay& r) { saved = r; });
        tas::Macro m(60.f);
        m.Describe("memenu self-test: walk in the editor's room").Wait(5).Press(forward).Wait(60).Release(forward).Wait(20);
        tas::Replay macro = m.Build();
        macro.start = tas::StartKind::Checkpoint;
        macro.map = maps[static_cast<std::size_t>(index)].label;
        macro.checkpoint.clear();              // The level's start
        macro.syncValidUntil = 0;              // The first playback records the reference
        tas::WithReplay([&](tas::Replay& r) { r = macro; });
        OnRenderThread([] { gui::visible = false; });
        const auto play = [&] {
            tas::Play();
            bool started = false;
            tas::Status st{};
            const ULONGLONG deadline = GetTickCount64() + 90000;
            while (GetTickCount64() < deadline && !g_aborted) {
                st = tas::GetStatus();
                if (st.mode == tas::Mode::Playing && !st.armed) started = true;
                if (started && (st.mode != tas::Mode::Playing || st.paused || st.frame >= st.frames)) break;
                Sleep(10);
            }
            WaitFor(5.0, [&] { st = tas::GetStatus(); return st.mode != tas::Mode::Playing || st.paused; });
            tas::Stop();        // At its end the TAS holds the game paused: where the player is, read after it let go
            WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; });
            WaitTicks(3);
            return std::make_tuple(started, st, where());
        };
        const auto [ran1, first, end1] = play();
        spdlog::info("Self-test: editor map, after the recording run: {}", end1.text);
        const auto [ran2, second, end2] = play();      // From the level's start again: the room restarted
        spdlog::info("Self-test: editor map, after the playback: {}", end2.text);
        const float walked = std::hypot(end2.at.X - start.at.X, end2.at.Y - start.at.Y);
        s.notes.push_back(fmt::format("TAS: walked {:.0f} units; playback {} ({} frames checked), ended {:.3f} units from the recording",
                                      walked, second.verified ? "verified" : "not verified", second.syncChecked,
                                      std::hypot(end2.at.X - end1.at.X, end2.at.Y - end1.at.Y)));
        if (!ran1 || !ran2) p.Problem("the TAS didn't play in the editor's room: " + tas::GetStatus().message);
        else if (!second.verified || second.desyncFrame >= 0)
            p.Problem(fmt::format("the playback wasn't verified (desync on frame {}: {})", second.desyncFrame, second.desyncInfo));
        if (walked < 100.f) p.Problem(fmt::format("the TAS walked the player only {:.0f} units", walked));
        else if (!inRoom(end2.at) || !end2.walking) p.Problem("the walk didn't end on the room's floor: " + end2.text);
        if (Lower(end2.map) != "mirageeditorroom") p.Problem("after the TAS the level is " + end2.map);
        tas::WithReplay([&](tas::Replay& r) { r = saved; });
        OnRenderThread([] { gui::visible = true; });
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

// Time-trial routes (step 13), the milestone: tests/mods/route-test has a route on the Prologue and the same one on
// map-test's MirageTestMap (a copy of the Prologue). For each, a TAS macro starts at After_Intro and runs forward through
// the start, the checkpoint and the finish: its first playback records the reference, the second plays back verified
// (every recorded frame matched) and finishes the route in the very same game time; the speedrun timer follows (its
// "TT - <route>" run splits and finishes) and the best run is kept as the ghost.
void TestTimeTrials()
{
    Step& s = Begin("Mirage time trials");
    Probe p(s);
    namespace routes = mirage::routes;
    Module* timer = modules::Find("Speedrun Timer");
    Module* trials = modules::Find("Time Trials");
    if (!tas::Available() || !timer || !trials) {
        p.Problem(!tas::Available() ? "the TAS isn't available" : "the Speedrun Timer or Time Trials module is missing");
        End(s);
        return;
    }
    std::vector<routes::Route> list;
    WaitFor(10.0, [&] {
        list.clear();
        for (const auto& r : routes::List())
            if (r.mod == "route-test") list.push_back(r);
        return list.size() >= 2;
    });
    if (list.size() < 2) {
        p.Problem(fmt::format("{} route-test route(s) found, not 2 (edge_p and miragetestmap)", list.size()));
        End(s);
        return;
    }
    const bool timerWas = timer->Enabled(), trialsWas = trials->Enabled();
    OnRenderThread([&] {
        timer->SetEnabled(true);
        trials->SetEnabled(true);
        gui::visible = false;
    });
    std::string forward;
    OnGameThread([&] { forward = KeyFor(game::Controller(), "GBA_MoveForward", "W"); });
    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; });
    const auto maps = levels::Maps();
    const auto shared = [](const char* key) {
        char v[256]{};
        mirage::mods::game_api::ShareGet("memenu-speedrun", key, v, sizeof v);
        return std::string(v);
    };

    for (const char* mapFile : {"edge_p", "miragetestmap"}) {
        const auto route = std::find_if(list.begin(), list.end(), [&](const routes::Route& r) { return r.map == mapFile; });
        const auto map = std::find_if(maps.begin(), maps.end(), [&](const levels::MapView& m) { return Lower(m.file) == mapFile; });
        if (route == list.end() || map == maps.end()) {
            p.Problem(std::string("no route or no level for ") + mapFile);
            continue;
        }
        const auto cp = std::find(map->checkpointNames.begin(), map->checkpointNames.end(), "After_Intro");
        if (cp == map->checkpointNames.end()) {
            p.Problem(std::string(mapFile) + " has no After_Intro checkpoint in the level list");
            continue;
        }
        tas::Macro m(60.f);
        m.Describe(std::string("memenu self-test: route on ") + mapFile).Wait(5).Press(forward).Wait(100).Release(forward).Wait(20);
        tas::Replay macro = m.Build();
        macro.start = tas::StartKind::Checkpoint;
        macro.map = map->label;
        macro.checkpoint = map->checkpoints[static_cast<std::size_t>(cp - map->checkpointNames.begin())];
        macro.syncValidUntil = 0;              // The first playback records the reference
        macro.extensions["env.blockStreaming"] = "1";
        tas::WithReplay([&](tas::Replay& r) { r = macro; });
        routes::Forget(route->Id());           // No best and no ghost yet: the first run sets them
        routes::Arm(route->Id());
        WaitTicks(2);

        // Plays it to the end; where the player was on some frames (to place routes)
        const auto play = [&](std::vector<std::string>* where) {
            tas::Play();
            bool started = false;
            tas::Status st{};
            std::size_t sampled = 0;
            const ULONGLONG deadline = GetTickCount64() + 90000;
            while (GetTickCount64() < deadline && !g_aborted) {
                st = tas::GetStatus();
                if (st.mode == tas::Mode::Playing && !st.armed) started = true;
                if (where && started && st.frame >= sampled) {
                    const std::size_t frame = st.frame;
                    OnGameThread([&] {
                        if (auto* pawn = game::Pawn())
                            where->push_back(fmt::format("f{}: [{:.0f}, {:.0f}, {:.0f}]", frame, pawn->Location.X, pawn->Location.Y, pawn->Location.Z));
                    }, 0.5);
                    sampled = frame + 20;
                }
                if (started && (st.mode != tas::Mode::Playing || st.paused || st.frame >= st.frames)) break;
                Sleep(10);
            }
            // The end of the replay: its verdict comes with the pause there (PlaybackEnded)
            WaitFor(5.0, [&] { st = tas::GetStatus(); return st.mode != tas::Mode::Playing || st.paused; });
            tas::Stop();        // (Not a tick wait first: at its end the TAS holds the game paused)
            WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; });
            WaitTicks(3);
            return std::make_pair(started, st);
        };
        std::vector<std::string> where;
        const auto [ran, first] = play(&where);
        const routes::State one = routes::Current();
        std::string path;
        for (const auto& w : where) path += w + " ";
        s.notes.push_back(std::string(mapFile) + " path: " + path);
        if (!ran) {
            p.Problem(std::string(mapFile) + ": the macro didn't play: " + tas::GetStatus().message);
            continue;
        }
        if (one.phase != routes::Phase::Finished) {
            p.Problem(fmt::format("{}: the route didn't finish on the recording run (phase {}, {} split(s), {:.3f} s)", mapFile,
                                  static_cast<int>(one.phase), one.splits.size(), one.time));
            continue;
        }
        WaitTicks(5);
        const std::string runFile = shared("run.file"), phase = shared("run.phase");
        if (runFile.find("TT") == std::string::npos || phase != "finished")
            p.Problem(fmt::format("{}: the speedrun timer didn't follow the route ({}, {})", mapFile, runFile, phase));

        // The same again: verified against the recording, the same time to the tick
        routes::Arm(route->Id());
        WaitTicks(2);
        const auto [ran2, second] = play(nullptr);
        const routes::State two = routes::Current();
        s.notes.push_back(fmt::format("{}: {} in {:.4f} s, then {:.4f} s; playback {} ({} frames checked){}", mapFile, route->name,
                                      one.time, two.time, second.verified ? "verified" : "not verified", second.syncChecked,
                                      two.ghost ? ", raced the ghost" : ""));
        if (!ran2) p.Problem(std::string(mapFile) + ": the second playback didn't run");
        else if (!second.verified || second.desyncFrame >= 0)
            p.Problem(fmt::format("{}: the playback wasn't verified (desync on frame {}: {})", mapFile, second.desyncFrame,
                                  second.desyncInfo));
        if (two.phase != routes::Phase::Finished) p.Problem(std::string(mapFile) + ": the route didn't finish on the playback");
        else if (two.time != one.time) p.Problem(fmt::format("{}: {:.6f} s on playback, {:.6f} s recorded", mapFile, two.time, one.time));
        if (!two.ghost) p.Problem(std::string(mapFile) + ": no ghost of the first run to race");
        if (two.best != one.time) p.Problem(fmt::format("{}: the best time is {:.6f}, not the first run's", mapFile, two.best));
        else if (two.newBest) p.Problem(std::string(mapFile) + ": the same time again counted as a new best");
        if (!one.newBest) p.Problem(std::string(mapFile) + ": the first run (no best before it) wasn't the best");
    }

    routes::Stop();
    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    OnRenderThread([&] {
        timer->SetEnabled(timerWas);
        trials->SetEnabled(trialsWas);
        gui::visible = true;
    });
    // Back to the test level
    for (int i = 0; i < static_cast<int>(maps.size()); ++i)
        if (Lower(maps[static_cast<std::size_t>(i)].file) == Lower(g_options.map)) {
            const int checkpoint = std::min(g_options.checkpoint, static_cast<int>(maps[static_cast<std::size_t>(i)].checkpoints.size()));
            const int before = g_loadsFinished.load();
            OnRenderThread([i, checkpoint] { levels::Load(i, checkpoint, levels::Method::Game, false); });
            WaitFor(120.0, [&] { return g_loadsFinished.load() > before; });
            WaitFor(60.0, [] { return game::GetStatus().hasPawn && !game::IsLoading(); });
            WaitPlayerReady(30.0);
            break;
        }
    End(s);
}

// The in-game Level Editor (step 13): in tests/mods/layer-test's layer for the Prologue, it places a pad of the mod's
// own class where the camera looks (saved to levels/edge_p.toml, spawned from it), shows its gizmo, moves it live, and
// an edit of its location, scale and a property is saved and spawned again with them. The Collision View (when
// memenu-collision is loaded) draws the placed pad, a mod's class, as a live cylinder. The layer file is put back as it was.
void TestLevelEditor()
{
    Step& s = Begin("Mirage level editor");
    Probe p(s);
    namespace layers = mirage::layers;
    Module* editor = modules::Find("Level Editor");
    Module* view = modules::Find("Collision View");
    auto* inspect = view ? dynamic_cast<ctl::Checkbox*>(view->FindSetting("Inspect")) : nullptr;
    auto* cylinders = view ? dynamic_cast<ctl::Checkbox*>(view->FindSetting("Cylinders")) : nullptr;
    std::filesystem::path file;
    for (const auto& m : mirage::mods::List())
        if (m.id == "layer-test") file = m.dir / "levels" / "edge_p.toml";
    if (!editor || file.empty()) {
        p.Problem("the Level Editor or the layer-test mod is missing");
        End(s);
        return;
    }
    std::string original;
    {
        std::ifstream in(file, std::ios::binary);
        original.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    // The Collision View (memenu-collision) is optional: switched off in the mod manager, its check is left out
    const bool withView = view && inspect && cylinders;
    if (!withView) s.notes.push_back("Collision View check left out: memenu-collision isn't loaded");
    const bool editorWas = editor->Enabled();
    const bool viewWas = withView && view->Enabled(), inspectWas = withView && inspect->Get();
    const bool expandedWas = withView && view->expanded, cylindersWas = withView && cylinders->Get();
    game::SetCameraTracking(true);
    OnRenderThread([&] {
        editor->SetEnabled(true);
        gui::visible = true;
    });
    level_editor::Select("layer-test", "");
    WaitTicks(5);

    // 1. Placed where the camera looks: in the file, spawned from it
    const std::string name = level_editor::PlaceWhereLooking("classes/LayerPad.uc");
    Classes::AActor* placed = nullptr;
    WaitFor(15.0, [&] { OnGameThread([&] { placed = layers::Find("layer-test", name); }); return placed != nullptr; });
    std::vector<layers::EditActor> entries;
    OnGameThread([&] { layers::Read("layer-test", entries); });
    const auto entry = std::find_if(entries.begin(), entries.end(), [&](const layers::EditActor& a) { return a.name == name; });
    s.notes.push_back(fmt::format("placed {} ({} entries in the layer now)", name, entries.size()));
    if (name.empty() || entry == entries.end()) {
        p.Problem("the placed actor isn't in levels/edge_p.toml");
    } else if (!placed) {
        p.Problem("the placed actor wasn't spawned from the layer");
    } else {
        // 2. Its gizmo, with it selected
        level_editor::Select("layer-test", name);
        const std::uint64_t gizmo = level_editor::GizmoFrames();
        WaitFor(5.0, [&] { return level_editor::GizmoFrames() > gizmo + 3; });
        float displayW = 0.f;
        OnRenderThread([&] { displayW = ImGui::GetIO().DisplaySize.x; });
        if (level_editor::GizmoFrames() <= gizmo + 3) {
            if (displayW >= 200.f) p.Problem("the gizmo wasn't drawn on the selected actor");
            else s.notes.push_back("gizmo not checked: the game is minimized (no screen to draw on)");
        }

        // 3. Moved live (the gizmo's drag): the same actor, somewhere else at once
        layers::EditActor moved = *entry;
        Classes::FVector player{};
        OnGameThread([&] { if (auto* pawn = game::Pawn()) player = pawn->Location; });
        moved.location[0] = player.X + 150.f;
        moved.location[1] = player.Y;
        moved.location[2] = player.Z;
        float liveX = 0.f;
        OnGameThread([&] {
            layers::Place(placed, moved.location, moved.rotation, moved.scale);
            liveX = placed->Location.X;
        });
        if (std::fabs(liveX - moved.location[0]) > 1.f) p.Problem(fmt::format("moving it live left it at X {}", liveX));

        // 4. An edit saved: location, scale and a property, spawned again with them
        moved.scale = 1.5f;
        moved.properties = {{"Strength", "1234.0"}};
        *std::find_if(entries.begin(), entries.end(), [&](const layers::EditActor& a) { return a.name == name; }) = moved;
        std::string error;
        bool written = false;
        OnGameThread([&] { written = layers::Write("layer-test", entries, &error); });
        if (!written) p.Problem("saving failed: " + error);
        Classes::AActor* again = nullptr;
        WaitFor(10.0, [&] {
            OnGameThread([&] { again = layers::Find("layer-test", name); });
            return again && again != placed;
        });
        float x = 0.f, scale = 0.f, strength = 0.f;
        std::string objectName;
        if (again)
            OnGameThread([&] {
                x = again->Location.X;
                scale = again->DrawScale;
                strength = mirage::reflect::GetFloat(mirage::reflect::Resolve(again->Class, again, "Strength"));
                objectName = again->GetName();
            });
        s.notes.push_back(fmt::format("saved and spawned again: X {:.0f}, scale {:.2f}, Strength {:.0f}", x, scale, strength));
        if (!again || again == placed) p.Problem("the saved layer wasn't spawned again");
        else if (std::fabs(x - moved.location[0]) > 1.f || std::fabs(scale - 1.5f) > 1e-3f || strength != 1234.f)
            p.Problem("the spawned actor doesn't have the saved location, scale and Strength");

        // 5. The editor's tools: the class list, the class's properties, duplicate, undo / redo, drop to floor, snap
        if (again) {
            namespace le = level_editor;
            const auto layerNow = [] { return le::Entries(); };
            const auto entryOf = [&](const std::string& n) {
                for (const auto& e : layerNow()) if (e.name == n) return e;
                return layers::EditActor{};
            };
            const auto spawned = [](const std::string& n) {
                Classes::AActor* a = nullptr;
                OnGameThread([&] { a = layers::Find("layer-test", n); });
                return a;
            };
            // Placeable classes: an engine trigger yes, the abstract Actor no
            std::vector<std::string> classes;
            OnGameThread([&] { classes = le::PlaceableClasses(); });
            const bool hasTrigger = std::find(classes.begin(), classes.end(), "Engine.Trigger") != classes.end();
            const bool hasActor = std::find(classes.begin(), classes.end(), "Engine.Actor") != classes.end();
            if (!hasTrigger || hasActor)
                p.Problem(fmt::format("the class list ({} classes) {} Engine.Trigger and {} Engine.Actor", classes.size(),
                                      hasTrigger ? "has" : "lacks", hasActor ? "has" : "doesn't have"));
            // Its class's editable properties, with the value the layer gave it
            le::Select("layer-test", name);
            WaitFor(5.0, [] { return le::Selected() >= 0; });
            std::vector<le::Property> props;
            OnGameThread([&] { props = le::PropertiesOf(name); });
            const auto strengthProp = std::find_if(props.begin(), props.end(), [](const le::Property& x) { return x.name == "Strength"; });
            if (strengthProp == props.end()) p.Problem(fmt::format("Strength isn't among its {} editable properties", props.size()));
            else if (util::ParseFloatOr(strengthProp->value, 0.f) != 1234.f) p.Problem("its Strength reads " + strengthProp->value + ", the layer says 1234");

            // Duplicate: a second entry, spawned; undo takes it away (and its actor), redo brings it back
            const std::size_t before = layerNow().size();
            const std::size_t undoBefore = le::UndoDepth();
            le::Duplicate();
            WaitFor(5.0, [&] { return layerNow().size() == before + 1; });
            const auto after = layerNow();
            const std::string copy = after.size() == before + 1 ? after.back().name : std::string();
            WaitFor(10.0, [&] { return !copy.empty() && spawned(copy) != nullptr; });
            if (copy.empty() || !spawned(copy)) p.Problem("Duplicate didn't add a spawned copy");
            le::Undo();
            WaitFor(10.0, [&] { return layerNow().size() == before && spawned(copy) == nullptr; });
            if (layerNow().size() != before || spawned(copy)) p.Problem("Undo didn't take the copy away");
            le::Redo();
            WaitFor(10.0, [&] { return layerNow().size() == before + 1 && spawned(copy) != nullptr; });
            if (layerNow().size() != before + 1) p.Problem("Redo didn't bring the copy back");
            if (le::UndoDepth() <= undoBefore) p.Problem("the editor's history didn't grow");

            // Drop to floor: down onto the floor (it starts at the player's height), and again: it stays
            le::Select("layer-test", name);
            WaitFor(5.0, [&] { const int i = le::Selected(); return i >= 0 && layerNow()[static_cast<std::size_t>(i)].name == name; });
            const float z0 = entryOf(name).location[2];
            le::DropToFloor();
            WaitFor(5.0, [&] { return entryOf(name).location[2] != z0; });
            const float z1 = entryOf(name).location[2];
            WaitFor(10.0, [&] { return spawned(name) != nullptr; });
            le::DropToFloor();
            WaitTicks(10);
            const float z2 = entryOf(name).location[2];
            if (!(z1 < z0)) p.Problem(fmt::format("Drop to floor left it at Z {:.0f} (from {:.0f})", z1, z0));
            else if (std::fabs(z2 - z1) > 2.f) p.Problem(fmt::format("dropped again it moved from Z {:.1f} to {:.1f}: not on the floor", z1, z2));

            // Snap: onto a 64 grid
            le::SetSnap(64.f, 45.f, 0.f);
            le::SnapSelected();
            WaitFor(5.0, [&] { const auto e = entryOf(name); return std::fmod(std::fabs(e.location[0]), 64.f) < 0.01f; });
            const auto snapped = entryOf(name);
            le::SetSnap(0.f, 0.f, 0.f);
            bool onGrid = true;
            for (float v : snapped.location) onGrid &= std::fabs(v - std::round(v / 64.f) * 64.f) < 0.01f;
            if (!onGrid) p.Problem(fmt::format("Snap left it at [{:.1f}, {:.1f}, {:.1f}]", snapped.location[0], snapped.location[1], snapped.location[2]));
            s.notes.push_back(fmt::format("{} placeable classes; {} editable properties (Strength {}); duplicate, undo, redo; "
                                          "dropped to the floor {:.0f} -> {:.0f}; snapped to [{:.0f}, {:.0f}, {:.0f}]",
                                          classes.size(), props.size(), strengthProp != props.end() ? strengthProp->value : "?", z0, z1,
                                          snapped.location[0], snapped.location[1], snapped.location[2]));
            // The pad as it is now, for the Collision View check below
            WaitFor(10.0, [&] { again = spawned(name); return again != nullptr; });
            if (again) OnGameThread([&] { objectName = again->GetName(); });
        }

        // 6. The Collision View draws it: among the volumes near the player
        if (again && withView) {
            OnRenderThread([&] {
                view->expanded = true;
                inspect->Set(true);
                cylinders->Set(true);       // A mod's class is drawn as a live cylinder, found only with these on
                if (!view->Enabled()) view->SetEnabled(true);
            });
            bool listed = false;
            WaitFor(10.0, [&] {
                const auto info = CollisionShared("inspect");
                for (const auto& n : info.value("nearby", nlohmann::json::array()))
                    listed |= n.value("name", std::string()).find(objectName) != std::string::npos;
                for (const auto& n : info.value("live", nlohmann::json::array()))     // A mod's class: drawn live
                    listed |= n.is_string() && n.get<std::string>() == objectName;
                return listed;
            });
            if (!listed) p.Problem(objectName + " isn't drawn by the Collision View (nearby volumes or live cylinders)");
            else s.notes.push_back("the Collision View lists " + objectName);
        }
    }

    // The layer as it was
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << original;
    }
    OnRenderThread([&] {
        if (withView) {
            inspect->Set(inspectWas);
            cylinders->Set(cylindersWas);
            view->expanded = expandedWas;
            view->SetEnabled(viewWas);
        }
        editor->SetEnabled(editorWas);
    });
    WaitFor(10.0, [&] {
        Classes::AActor* left = nullptr;
        OnGameThread([&] { left = layers::Find("layer-test", name); });
        return left == nullptr;
    });
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
    if (oldBack) p.Problem("the old player's handle still gave an object after the restart");

    const auto st = mirage::world_events::GetStats();
    s.notes.push_back(fmt::format("{} actors in play; compared on {} ticks, {} with a change: {:.3f} ms a tick on average, "
                                  "max {:.3f} (a restart's); seen: {:.0f} added, {:.0f} removed, {:.0f} restart(s), {:.0f} streamed",
                                  st.actors, st.compares, st.changed, st.compares ? st.totalCompareMs / st.compares : 0.0,
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

// The in-game editors (step 16): the World Explorer lists the player among the actors in play; selected, the Object
// Inspector shows its live GroundSpeed, a Vector it opens into X/Y/Z, its Controller as an object to select; typed edits
// of GroundSpeed and JumpZ change the pawn and Undo puts both back exactly; a Lua mod's transaction (tests/mods/
// inspector-lua: JumpZ doubled) shows in the inspector and its revert too; a selected actor that is destroyed reads as
// gone (never another object); both windows draw.
void TestInspector()
{
    Step& s = Begin("Mirage inspector");
    Probe p(s);
    Module* explorer = modules::Find("World Explorer");
    Module* inspector = modules::Find("Object Inspector");
    Module* modTest = modules::Find("Inspector (Lua)");
    if (!explorer || !inspector || !modTest) {
        p.Problem("the World Explorer, the Object Inspector or the Inspector (Lua) test mod is missing");
        End(s);
        return;
    }
    const bool explorerWas = explorer->Enabled(), inspectorWas = inspector->Enabled();
    OnRenderThread([&] {
        explorer->SetEnabled(true);
        inspector->SetEnabled(true);
        modTest->SetEnabled(false);
        gui::visible = true;
    });
    const auto rowOf = [](const std::vector<editor::Row>& rows, const std::string& path) {
        const auto it = std::find_if(rows.begin(), rows.end(), [&](const editor::Row& r) { return r.path == path; });
        return it == rows.end() ? editor::Row{} : *it;
    };
    const auto rows = [](const std::set<std::string>& open = {}) {
        std::vector<editor::Row> out;
        OnGameThread([&] { out = editor::Rows(editor::Selected(), "", open); });
        return out;
    };
    const auto fmtFloat = [](float f) {
        char b[48];
        std::snprintf(b, sizeof b, "%.6g", f);
        return std::string(b);
    };

    // 1. The explorer lists the player; selected from there
    std::vector<editor::ActorRow> actors;
    Classes::ATdPlayerPawn* pawn = nullptr;
    OnGameThread([&] {
        pawn = game::Pawn();
        actors = editor::Actors();
    });
    const auto me = std::find_if(actors.begin(), actors.end(), [&](const editor::ActorRow& a) { return a.actor == pawn; });
    if (!pawn || me == actors.end()) p.Problem(fmt::format("the explorer's {} actors don't include the player", actors.size()));
    OnGameThread([&] { editor::Select(pawn); });
    if (editor::SelectedLabel().find("TdPlayerPawn") == std::string::npos) p.Problem("selecting the player: " + editor::SelectedLabel());

    // 2. Its properties, live
    float ground = 0.f, jump = 0.f;
    OnGameThread([&] { ground = pawn->GroundSpeed; jump = pawn->JumpZ; });
    auto now = rows({"Location"});
    const editor::Row speedRow = rowOf(now, "GroundSpeed");
    if (speedRow.value != fmtFloat(ground) || !speedRow.editable)
        p.Problem(fmt::format("GroundSpeed reads '{}' (editable {}), the pawn has {}", speedRow.value, speedRow.editable, ground));
    if (rowOf(now, "Location").kind != "Vector" || rowOf(now, "Location.X").path.empty())
        p.Problem("Location isn't a Vector row that opens into X, Y, Z");
    const editor::Row controller = rowOf(now, "Controller");
    void* pc = nullptr;
    OnGameThread([&] { pc = game::Controller(); });
    if (controller.ref != pc) p.Problem("the Controller row doesn't point to the player's controller");
    s.notes.push_back(fmt::format("{} actors listed; the player has {} rows (Location opened), GroundSpeed {}", actors.size(), now.size(), speedRow.value));

    // 3. Typed edits, and Undo
    const std::size_t depthBefore = editor::UndoDepth();
    std::string error;
    bool okSpeed = false, okJump = false;
    OnGameThread([&] {
        okSpeed = editor::SetValue(editor::Selected(), "GroundSpeed", "1234", &error);
        okJump = editor::SetValue(editor::Selected(), "JumpZ", "777", &error);
    });
    float speedSet = 0.f, jumpSet = 0.f;
    OnGameThread([&] { speedSet = pawn->GroundSpeed; jumpSet = pawn->JumpZ; });
    if (!okSpeed || !okJump || speedSet != 1234.f || jumpSet != 777.f)
        p.Problem(fmt::format("the edits didn't land: GroundSpeed {}, JumpZ {} ({})", speedSet, jumpSet, error));
    if (editor::UndoDepth() != depthBefore + 2) p.Problem("two edits didn't make two undo steps");
    OnGameThread([&] { editor::Undo(); editor::Undo(); });
    float speedBack = 0.f, jumpBack = 0.f;
    OnGameThread([&] { speedBack = pawn->GroundSpeed; jumpBack = pawn->JumpZ; });
    if (speedBack != ground || jumpBack != jump)
        p.Problem(fmt::format("Undo left GroundSpeed {} (was {}), JumpZ {} (was {})", speedBack, ground, jumpBack, jump));

    // 4. A mod's change shows in the inspector, and its revert too
    OnRenderThread([&] { modTest->SetEnabled(true); });
    WaitFor(5.0, [&] { return rowOf(rows(), "JumpZ").value == fmtFloat(jump * 2.f); });
    const std::string modded = rowOf(rows(), "JumpZ").value;
    OnRenderThread([&] { modTest->SetEnabled(false); });
    WaitFor(5.0, [&] { return rowOf(rows(), "JumpZ").value == fmtFloat(jump); });
    const std::string reverted = rowOf(rows(), "JumpZ").value;
    if (modded != fmtFloat(jump * 2.f)) p.Problem("the inspector didn't show the mod's JumpZ (" + modded + ")");
    if (reverted != fmtFloat(jump)) p.Problem("the inspector didn't show the mod's revert (" + reverted + ")");

    // 5. A selected actor that is destroyed: gone, never another object
    Classes::AActor* probe = nullptr;
    OnGameThread([&] {
        Classes::FVector at = pawn->Location;
        at.X += 300.f;
        probe = pawn->Spawn(Classes::ADynamicSMActor_Spawnable::StaticClass(), nullptr, Classes::FName{}, at, pawn->Rotation, nullptr, true);
        editor::Select(probe);
    });
    Classes::UObject* selectedProbe = nullptr;
    OnGameThread([&] { selectedProbe = editor::Selected(); if (probe) probe->Destroy(); });
    Classes::UObject* afterDestroy = reinterpret_cast<Classes::UObject*>(1);
    OnGameThread([&] { afterDestroy = editor::Selected(); });
    if (!probe || selectedProbe != probe) p.Problem("selecting a spawned actor didn't hold it");
    if (afterDestroy) p.Problem("the destroyed actor is still the selection");
    if (!rows().empty()) p.Problem("the inspector still lists properties of the destroyed actor");

    // The windows drew
    const std::uint64_t e0 = editor::Frames(editor::Window::Explorer), i0 = editor::Frames(editor::Window::Inspector);
    WaitFor(5.0, [&] { return editor::Frames(editor::Window::Explorer) > e0 + 3 && editor::Frames(editor::Window::Inspector) > i0 + 3; });
    if (editor::Frames(editor::Window::Explorer) <= e0 + 3 || editor::Frames(editor::Window::Inspector) <= i0 + 3)
        p.Problem("the World Explorer or the Object Inspector window wasn't drawn");

    OnGameThread([] { editor::Select(nullptr); });
    OnRenderThread([&] {
        explorer->SetEnabled(explorerWas);
        inspector->SetEnabled(inspectorWas);
    });
    End(s);
}

// The Mirage Showcase (mods/memenu-showcase), each of its parts doing what it says in the game: its window draws; it
// reads the player live; its hook on TdPlayerPawn.Landed counts a jump's landing; its injection into
// TdMove_Jump.StartMove makes a standing jump lower and its low gravity makes one higher (each measured against a jump
// without it in the same run, then put back); a bounce pad of the class it describes in Lua launches the player; and
// switching it off puts the gravity back and removes its pads. Its World section lists the actors around the player, and
// watching actors, the dropped pad comes as actor_added.
float ShowcaseShared(const char* key)
{
    char v[64]{};
    mirage::mods::game_api::ShareGet("memenu-showcase", key, v, sizeof v);
    return util::ParseFloatOr(v, -1.f);
}

float StandingJump();

void TestShowcase()
{
    Step& s = Begin("Mirage showcase");
    Probe p(s);
    Module* m = modules::Find("Mirage Showcase");
    const auto box = [&](const char* name) { return m ? dynamic_cast<ctl::Checkbox*>(m->FindSetting(name)) : nullptr; };
    ctl::Checkbox *lowGravity = box("Low gravity"), *weighted = box("Weighted jumps"), *drop = box("Drop a pad"),
                  *watch = box("Watch actors");
    if (!m || !lowGravity || !weighted || !drop || !watch) {
        p.Problem("the Mirage Showcase module or its settings are missing (mods/memenu-showcase)");
        End(s);
        return;
    }
    const bool wasOn = m->Enabled();
    OnRenderThread([&] {
        lowGravity->Set(false);
        weighted->Set(false);
        m->SetEnabled(true);
        gui::visible = true;
    });

    // The window, and the player read live
    WaitFor(10.0, [] { return ShowcaseShared("frames") > 3.f; });
    if (ShowcaseShared("frames") <= 3.f) p.Problem("the showcase window wasn't drawn");
    float z = 0.f;
    OnGameThread([&] { if (auto* pawn = game::Pawn()) z = pawn->Location.Z; });
    WaitTicks(3);
    if (std::fabs(ShowcaseShared("pawn_z") - z) > 50.f)
        p.Problem(fmt::format("it read the player at Z {:.0f}, the player is at {:.0f}", ShowcaseShared("pawn_z"), z));
    // The world: the actors around the player (refreshed 4 times a second)
    WaitFor(3.0, [] { return ShowcaseShared("nearby") > 0.f; });
    if (ShowcaseShared("nearby") <= 0.f) p.Problem("its World section found no actors around the player");

    // Hook: a jump's landing; a jump without anything, to compare with
    const float landingsBefore = ShowcaseShared("landings");
    const float plain = StandingJump();
    WaitTicks(5);
    const float landings = ShowcaseShared("landings") - landingsBefore;
    if (plain <= 0.f) p.Problem("the plain standing jump didn't happen");
    if (landings < 1.f) p.Problem("its Landed hook didn't see the jump's landing");

    // Injection: weighted jumps are lower
    OnRenderThread([&] { weighted->Set(true); });
    WaitTicks(3);
    const float heavy = StandingJump();
    const float runs = ShowcaseShared("injected_runs");
    OnRenderThread([&] { weighted->Set(false); });
    WaitTicks(3);
    if (runs < 1.f) p.Problem("its injection into TdMove_Jump.StartMove never ran");
    else if (heavy >= plain - 20.f) p.Problem(fmt::format("a weighted jump went {:.0f} high, a plain one {:.0f}", heavy, plain));

    // Writing: low gravity, higher jumps, the level's value back after
    float gravityBefore = 0.f, gravityOn = 0.f, gravityAfter = 0.f;
    OnGameThread([&] { if (auto* w = game::World()) gravityBefore = w->WorldGravityZ; });
    OnRenderThread([&] { lowGravity->Set(true); });
    WaitFor(5.0, [] { return ShowcaseShared("gravity") != 0.f; });
    OnGameThread([&] { if (auto* w = game::World()) gravityOn = w->WorldGravityZ; });
    const float floaty = StandingJump();
    OnRenderThread([&] { lowGravity->Set(false); });
    WaitFor(5.0, [] { return ShowcaseShared("gravity") == 0.f; });
    WaitTicks(3);
    OnGameThread([&] { if (auto* w = game::World()) gravityAfter = w->WorldGravityZ; });
    if (std::fabs(gravityOn - gravityBefore * 0.35f) > 1.f)
        p.Problem(fmt::format("low gravity set {:.0f}, not 0.35 of the level's {:.0f}", gravityOn, gravityBefore));
    if (floaty <= plain + 20.f) p.Problem(fmt::format("a low-gravity jump went {:.0f} high, a plain one {:.0f}", floaty, plain));
    if (gravityAfter != gravityBefore) p.Problem(fmt::format("the gravity is {:.0f} after, {:.0f} before", gravityAfter, gravityBefore));

    // A class made in Lua: a pad dropped under the player launches it (the class compiles the first time: seconds)
    WaitGrounded();
    const float bouncesBefore = ShowcaseShared("bounces");
    OnRenderThread([&] { watch->Set(true); });      // Watching actors: the pad comes as actor_added
    WaitTicks(3);
    const float addedBefore = ShowcaseShared("actors_added");
    OnRenderThread([&] { drop->Set(true); });
    WaitFor(60.0, [] { return ShowcaseShared("pads") >= 1.f; });
    WaitFor(10.0, [&] { return ShowcaseShared("bounces") > bouncesBefore; });
    const float bounced = ShowcaseShared("bounces") - bouncesBefore;
    if (ShowcaseShared("pads") < 1.f) p.Problem("no pad was dropped (its class didn't compile? the log says)");
    else if (bounced < 1.f) p.Problem("the pad didn't launch the player");
    WaitFor(3.0, [&] { return ShowcaseShared("actors_added") > addedBefore; });
    const float addedSeen = ShowcaseShared("actors_added") - addedBefore;
    if (addedSeen < 1.f) p.Problem("watching actors, it didn't see the pad come (actor_added)");
    OnRenderThread([&] { watch->Set(false); });

    s.notes.push_back(fmt::format("standing jump {:.0f}, weighted {:.0f} (injection ran {:.0f} times), low gravity {:.0f} "
                                  "(gravity {:.0f} -> {:.0f}); {:.0f} landing(s) hooked; pad bounces {:.0f}; {:.0f} event(s) logged; "
                                  "{:.0f} actors within 30 m, {:.0f} added while watching",
                                  plain, heavy, runs, floaty, gravityBefore, gravityOn, landings, bounced, ShowcaseShared("events"),
                                  ShowcaseShared("nearby"), addedSeen));

    // Off: its pads go (the player stops bouncing) and nothing of it stays
    OnRenderThread([&] { m->SetEnabled(wasOn); });
    if (!wasOn) {
        int left = -1;
        WaitFor(5.0, [&] {
            OnGameThread([&] {
                left = 0;
                auto& objects = Classes::UObject::GetGlobalObjects();
                for (int i = 0; i < static_cast<int>(objects.Num()); ++i)
                    if (auto* o = objects.GetByIndex(i); o && o->Class && o->Class->GetName() == "MirageShowcasePad" &&
                                                         o->GetName().rfind("Default__", 0) != 0 &&
                                                         !static_cast<Classes::AActor*>(o)->bDeleteMe)
                        ++left;
            });
            return left == 0;
        });
        if (left != 0) p.Problem(fmt::format("{} pad(s) left after switching the showcase off", left));
    }
    WaitGrounded();
    End(s);
}

// A standing jump, by TAS macro: how high the player gets above where it started (-1: it couldn't be done)
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
    tas::Macro mac(60.f);
    mac.Describe("memenu self-test: standing jump").Wait(5).Hold(jumpKey, 2).Wait(80);
    tas::Replay saved;
    tas::WithReplay([&](tas::Replay& r) { saved = r; r = mac.Build(); });
    tas::Play();
    bool started = false;
    const ULONGLONG deadline = GetTickCount64() + 20000;
    while (GetTickCount64() < deadline && !g_aborted) {
        const tas::Status st = tas::GetStatus();
        if (st.mode == tas::Mode::Playing) started = true;
        if (started && (st.mode != tas::Mode::Playing || st.frame >= st.frames)) break;
        OnGameThread([&] {
            if (auto* pawn = game::Pawn()) topZ = std::max(topZ, pawn->Location.Z);
        }, 0.5);
    }
    tas::Stop();
    WaitFor(5.0, [] { return tas::GetStatus().mode == tas::Mode::Idle; });
    tas::WithReplay([&](tas::Replay& r) { r = saved; });
    WaitGrounded();
    return started ? topZ - startZ : -1.f;
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
    const float normal = StandingJump();

    setOn(b, true);
    setOn(a, true);
    if (bytes() == original) p.Problem("with both on, StartMove's bytecode is unchanged (the mods' logs say why)");
    if (auto* seen = dynamic_cast<ctl::SliderInt*>(b->FindSetting("Seen"))) OnRenderThread([seen] { seen->Set(-1); });
    const int runsBefore = intOf(a, "Runs");
    const float heavy = StandingJump();
    if (intOf(a, "Runs") <= runsBefore) p.Problem("inject-a's callback didn't run during the jump");
    if (intOf(b, "Seen") != 1) p.Problem("inject-b saw CurrentWeaponType " + std::to_string(intOf(b, "Seen")) + ", not inject-a's 1 (order)");

    // Higher Jumps patches the same function: under the injections, both apply
    float both = -1.f;
    if (higher) {
        setOn(higher, true);
        both = StandingJump();
        setOn(higher, false);
    }

    // Hot reload of inject-a: its injection out, the mod loaded again, its injection back on its next tick
    std::string reload = "timed out";
    OnGameThread([&] { reload = mm::Reload("inject-a"); }, 15.0);
    if (!reload.empty()) p.Problem("hot reload of inject-a: " + reload);
    Module* a2 = modules::Find("Inject A (Lua)");
    if (a2 && !a2->Enabled()) setOn(a2, true);
    WaitTicks(10);
    if (auto* seen = dynamic_cast<ctl::SliderInt*>(b->FindSetting("Seen"))) OnRenderThread([seen] { seen->Set(-1); });
    const float reloaded = StandingJump();
    if (intOf(b, "Seen") != 1) p.Problem("after inject-a's reload inject-b saw " + std::to_string(intOf(b, "Seen")) + ", not 1");

    setOn(a2 ? a2 : a, false);
    setOn(b, false);
    if (bytes() != original) p.Problem("with both off again, StartMove isn't its original bytecode");
    const float after = StandingJump();
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
        if (std::fabs(reloaded - heavy) > normal * 0.1f) p.Problem("after the reload the jump isn't the injected one");
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
#endif

// Mirage step 5, with the self-test's own mods (tests/mods, put in selftest-mods by `memenu game-test`): load order and
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
#if MEMENU_GAME
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
#if MEMENU_GAME
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

#if MEMENU_GAME
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
#if MEMENU_GAME
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
#if MEMENU_GAME
    static const Named tests[] = {
#if MEMENU_FEATURE_TAS
        {"TAS macro", &TestTasMacro, true},
        {"TAS camera keys", &TestTasCamera, true},
        {"TAS go to and step", &TestTasGoto, true},
        {"TAS desync fixing", &TestTasDesync, true},
        {"TAS snapshots", &TestTasSnapshots, true},
        {"TAS workflow", &TestTasWorkflow, false},      // Records and loops in real time
        {"TAS memory survey", &TestTasMemorySurvey, false, true},   // Measurements only: --only
        {"Rendering thread off", &TestRenderingThreadOff, false, true},
        {"Input overlay", &TestInputOverlay, true},
#endif
        {"Kickglitch helper", &TestKickglitchHelper, false},
        {"Auto kickglitch", &TestAutoKickglitch, false},
        {"Kill bots", &TestKillBots, false},
        {"Keep key bindings", &TestKeepKeyBindings, false},
        {"Collision view inspect", &TestCollisionInspect, false},
        {"Collision view static geometry", &TestCollisionStatic, false},
        {"Speedrun timer", &TestSpeedrunTimer, false},
        {"Engine state hook", &TestEngineStateHook, false},
        {"Mirage runtime", &TestMirageRuntime, false},
        {"Mirage layouts", &TestMirageLayouts, false},
        {"Mirage calls", &TestMirageCalls, false},
        {"Mirage function hooks", &TestMirageFunctionHooks, false},
        {"Mirage mods", &TestMirageMods, false},
        {"Mirage lifecycle", &TestMirageLifecycle, false},
        {"Mirage hooks from mods", &TestMirageModHooks, false},
        {"Mirage UI layer", &TestMirageUi, false},
        {"Cache system", &TestCacheSystem, false},
        {"Memory manager", &TestMemoryManager, false},
        {"Crash reporter", &TestCrashReporter, false},
        {"Mirage C# mods", &TestCSharpMods, false},
#if MEMENU_FEATURE_SCRIPT_EDITOR
        {"Mirage script editor", &TestScriptEditor, false},
#endif
        {"Mirage content", &TestMirageContent, false},
        {"Mirage textures", &TestMirageTextures, false},
#if MEMENU_FEATURE_TAS
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
        {"Mirage level editor", &TestLevelEditor, false},
        {"Mirage maps", &TestModMaps, false},
        {"Mirage editor map", &TestEditorMap, false},
        {"Mirage world access", &TestWorldAccess, false},
        {"Mirage change log", &TestChangeLog, false},
        {"Mirage inspector", &TestInspector, false},
        {"Mirage showcase", &TestShowcase, false},
        {"Mirage time trials", &TestTimeTrials, false},
#endif
        {"Engine script events", &TestEngineScriptEvents, false},
        {"Frame rate limit", &TestFrameRate, false},
        {"Engine console", &TestEngineConsole, false},
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
#if MEMENU_GAME
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
#if MEMENU_GAME
    static bool listening = false;      // Once: every run would add another callback
    if (!listening) game::OnLevelLoad(&OnLevelLoad);
    listening = true;
#endif
#if MEMENU_FEATURE_TAS
    // Hang watchdog: no frame for 8 s, the game's threads' stacks go to the log (once a hang)
    static std::atomic<bool> watching{true};
    watching = true;                    // Again for every run
    std::thread watchdog([] {
        unsigned last = hooks::EndSceneCalls();
        ULONGLONG since = GetTickCount64();
        bool dumped = false;
        while (watching) {
            Sleep(250);
            const unsigned now = hooks::EndSceneCalls();
            static ULONGLONG loadingSince = 0;
            if (game::IsLoading()) {
                if (!loadingSince) loadingSince = GetTickCount64();
            } else {
                loadingSince = 0;
            }
            if (now != last && !(loadingSince && GetTickCount64() - loadingSince > 10000)) {
                last = now;
                since = GetTickCount64();
                dumped = false;
                continue;
            }
            if (dumped || (GetTickCount64() - since < 8000 && !loadingSince)) continue;
            dumped = true;
            spdlog::warn("Hang: no frame for 8 s, or loading for 10 s (step: {})", g_current);
            for (int round = 0; round < 2; ++round) {
                for (const auto& t : tas::memmap::Threads()) {
                    if (t.startModule != "mirrorsedge.exe" && t.startModule.rfind("physx", 0) != 0) continue;
                    std::string line;
                    for (const auto& w : tas::memmap::StackSample(t.id, 32)) line += w + " ";
                    spdlog::warn("Hang: thread {} ({}) state {} wait {}: {}", t.id, t.startModule, t.state, t.waitReason, line);
                }
                spdlog::default_logger()->flush();
                Sleep(1000);
            }
        }
    });
    // Ends before the test does: the menu unloads after it
    struct StopWatchdog {
        std::thread& t;
        ~StopWatchdog() { watching = false; if (t.joinable()) t.join(); }
    } stopWatchdog{watchdog};
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
#if MEMENU_GAME
        if (TestGame() && !g_aborted && TestLevel() && !g_aborted &&
            (g_options.keepLevel || Lower(g_options.map) != "none")) {
            TestInputBlocking();
            const Named* tests = FeatureList();
            for (const Named* t = tests; t->name && !g_aborted; ++t) {
                if (!g_options.only.empty() && !g_options.only.count(t->name)) continue;
                if (t->optIn && !g_options.only.count(t->name)) continue;
                if (g_options.skip.count(t->name)) continue;
#if MEMENU_FEATURE_TAS
                const ScopedTasSpeed speed(g_options.fast && t->fastTas);
#endif
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
#if MEMENU_GAME
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
        WriteResult(true);              // selftest-result.json, like `memenu game-test`
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
