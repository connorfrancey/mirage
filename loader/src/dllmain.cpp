#include <Windows.h>
#include <atomic>
#include <functional>
#include <spdlog/spdlog.h>

#include "config.h"
#include "core/clock.h"
#include "core/hardware.h"
#include "core/module.h"
#include "core/profile.h"
#include "core/scheduler.h"
#include "core/signature_cache.h"
#include "diag.h"
#include "failsafe.h"
#include "hooks.h"
#include "log.h"
#include "paths.h"
#include "selftest.h"
#include "core/devmode.h"
#include "core/events.h"
#include "core/tasks.h"
#include "test_mode.h"
#include "unload.h"
#if MIRAGE_GAME
#include "game/game.h"
#include "game/levels.h"
#include "runtime/content.h"
#include "runtime/detours.h"
#include "runtime/fhooks.h"
#include "runtime/classes.h"
#include "runtime/kismet_layers.h"
#include "runtime/kismet_trace.h"
#include "runtime/triggers.h"
#include "runtime/pieces.h"
#include "runtime/layers.h"
#include "runtime/world_events.h"
#include "runtime/maps.h"
#include "runtime/routes.h"
#include "runtime/file_redirect.h"
#include "runtime/mod_game.h"
#include "runtime/mods.h"
#include "runtime/reflect.h"
#include "runtime/texture_overlays.h"
#include "runtime/inject.h"
#include "runtime/script_patch.h"
#include "runtime/firstparty.h"
#include "runtime/mem/manager.h"
#include "runtime/crash_reporter.h"
#include "crashhandler/symbols.h"
#include "runtime/hang.h"
#endif
#if MIRAGE_FEATURE_INPUT
#include "tas/engine.h"
#include "tas/input_monitor.h"
#endif
#include "theme.h"

// Logs each startup step and how long it took, so a hang or crash shows which step it was in.
template <class F>
static auto Step(const char* name, F&& f)
{
    spdlog::info("Startup: {}...", name);
    const ULONGLONG start = GetTickCount64();
    struct Done {
        const char* name; ULONGLONG start;
        ~Done() { spdlog::info("Startup: {} done ({} ms)", name, GetTickCount64() - start); }
    } done{name, start};
    return f();
}
#define STARTUP_STEP(call) Step(#call, [&] { return call; })

static void Run(HMODULE module)
{
    const ULONGLONG attachedAt = GetTickCount64();
    spdlog::info("MirrorsEdgeMenu attached{}", testmode::Enabled() ? " (test mode: failed checks break instead of a dialog)" : "");
    diag::LogStartup(module);
    diag::InstallCrashLogger(module);
    testmode::Install([](const char* message) {
        spdlog::critical("Debug check failed: {}", message);
        spdlog::default_logger()->flush();
    });
    unload::Requested();   // Creates the unload event before anything can go wrong
    const bool selfTest = selftest::Requested();   // `selftest` file next to the DLL: see selftest.h
    if (selfTest) diag::LogGameCrashesUntil(~0ull);    // Every crash, the game's too, with its stack
    if (selfTest) config::SetAutoSave(false);      // The test changes settings: never into the player's config

    STARTUP_STEP(clk::Init());                // Before anything measures time (scheduler, caches)
    prof::Start();                            // Tracy builds only: profiler threads (stopped after the hooks are gone)
    STARTUP_STEP(theme::Init());              // themes/ folder + built-in theme files
    STARTUP_STEP(modules::Init());            // Every module registered with MEM_MODULE
#if MIRAGE_GAME
    // Mirage mods (mods\<id>\mod.toml): found and ordered now, the early phase loaded before the config and the
    // scheduler see the modules. The self-test also loads its test mods (selftest-mods, put there by `mirage game-test`).
    STARTUP_STEP(mirage::firstparty::Install(paths::Dir() / "mods"));   // The menu's own mods, at this DLL's version
    STARTUP_STEP(mirage::mods::Discover(selfTest ? std::vector{paths::Dir() / "mods", paths::Dir() / "selftest-mods"}
                                                 : std::vector{paths::Dir() / "mods"}));
    STARTUP_STEP(mirage::mods::LoadPhase(mirage::mods::Phase::Early));
    diag::SetCrashAttributor([](const void* address, unsigned long code, bool* guarded) {
        return mirage::mods::AttributeCrash(address, code, guarded);
    });
#endif
    STARTUP_STEP(modules::ApplyDefaults());   // Modules flagged enabledByDefault
    STARTUP_STEP(config::Load());             // Saved state overrides the defaults
    devmode::ApplyBuild();                    // A developer build (mirage --dev) switches developer mode on
    STARTUP_STEP(sched::Init());              // Run order of the modules' OnTick

    const bool installed = STARTUP_STEP(hooks::Install());
#if MIRAGE_GAME
    // Package opens are followed from now on (a package keeps the file it first got: runtime/file_redirect.h), before
    // the engine-phase mods turn their content on
    if (installed) STARTUP_STEP(mirage::files::Install());
#endif
    spdlog::info("Hardware: {}", hw::Get().Summary());   // After the hooks: they add the GPU
    if (installed)
    {
#if MIRAGE_GAME
        // Fatal crashes get a report instead of the game's dialog (runtime/crash_reporter.h): the hooks are up now
        STARTUP_STEP(mirage::crash::Install());
        if (selfTest) mirage::crash::SetUnattended(true);   // A crash mid-test ends the game (the tool sees it), no window waits
#endif
        // Background tasks (core/tasks.h) and the event bus's queued deliveries: the render thread now, the game thread
        // once the game layer is up
        const auto toRender = [](std::function<void()> fn) { hooks::RunOnRenderThread(std::move(fn)); };
        tasks::SetDispatcher(tasks::On::RenderThread, toRender);
        events::SetDispatcher(events::Where::RenderThread, toRender);
        events::SetDispatcher(events::Where::Worker, [](std::function<void()> fn) { tasks::Run(std::move(fn), {tasks::Priority::Normal, "event"}); });
        STARTUP_STEP(tasks::Start());
        // From here modules added later (mods of the engine and world phases, hot reloads) join on the render thread,
        // with their saved state and a place in the scheduler
        modules::Start([](std::function<void()> fn) { hooks::RunOnRenderThread(std::move(fn)); }, [](Module& m) {
            if (!config::ApplySaved(m) && m.enabledByDefault) m.SetEnabled(true);
            sched::AddModule(m);
        });
#if MIRAGE_GAME
        // Module logic runs on the game thread through the engine tick; the menu itself stays on the render thread.
        const bool gameReady = STARTUP_STEP(game::Init());
        if (gameReady)
        {
            mirage::classes::Prepare();          // Mods' classes (step 12): the engine's loader found on a worker
            mirage::maps::Install();             // Mods' maps in the level select (step 13): the config lookup hooked
            game::RunOnGameThread([] { crashhandler::symbols::Refresh(); });   // The crash handler's engine names
            mirage::mods::game_api::WireChanges();   // Mods' transactions (step 15) write through the runtime's writer
            // The world phase's mods load on the first tick after a level has loaded
            static std::atomic<bool> levelLoaded{false}, worldLoaded{false};
            game::OnLevelLoad([](bool finished) {
                mirage::mods::DispatchLevelLoad(finished, finished ? game::MapName() : std::string());
                mirage::mem::OnLevelLoad(finished, finished ? game::MapName() : std::string());
                mirage::hang::SetLoading(!finished);
                events::Bus().Publish(events::LevelLoad{finished, finished ? game::MapName() : std::string()});
                if (!finished) return;
                crashhandler::symbols::Refresh();   // Names for the engine's code in crash reports (classes may have loaded)
                levelLoaded = true;
                mirage::layers::OnLevelLoaded();
                mirage::kismet_layers::OnLevelLoaded();
                mirage::kismet_trace::Invalidate();
                mirage::routes::OnLevelLoaded();
                mirage::world_events::OnLevelLoaded();
                mirage::reflect::ClearCache();   // Classes of the old level's packages are gone
                mirage::hooks::Retry();          // Functions hooked before they were loaded, or gone with the level
                mirage::content::ReapplyAll();   // Content mods' overrides on the level's objects
            });
            game::OnTick([](float seconds) {
                mirage::hang::BeatGame();
                if (levelLoaded && !worldLoaded.exchange(true)) mirage::mods::LoadPhase(mirage::mods::Phase::World);
                mirage::mods::DispatchTick(seconds);
                mirage::world_events::Tick();    // Actors added and removed, levels streamed, restarts (step 14)
                mirage::layers::Tick();          // Mods' level additions: spawned, respawned at restarts, removed
                mirage::kismet_layers::Tick();   // Mods' Kismet layers: applied at level loads, undone at unload
                mirage::kismet_trace::Tick();    // The Kismet execution trace (nothing while no mod has it on)
                mirage::triggers::Tick();        // Mods' triggers: stay callbacks, gone ones dropped
                mirage::pieces::Tick();          // Mods' swing poles and ziplines: the player near one gets it
                mirage::maps::Tick();            // Mods' maps: registered as mods load and unload
                mirage::routes::Tick(seconds);   // Time-trial routes: the run in progress
                modules::Tick();
            });
            // The mods' other events: deaths, the player's move state, keys
            game::OnPlayerDeath([](bool after) { mirage::mods::DispatchDeath(after); });
            game::OnPlayerStateChange([] { return mirage::mods::DispatchPlayerState(); });
            modules::SetKeyListener([](int vk) { mirage::mods::DispatchKey(vk); });
            // A memory snapshot put back (a tool rewinding the game): the mods' objects may be others now
            game::OnStateRestored([] { mirage::mods::DispatchStateRestored(); });
            STARTUP_STEP(levels::Init());
            // The memory manager (runtime/mem/manager.h): a cache scheduler job, after the object queries made theirs.
            // Under pressure it also tells the mods (MIRAGE_EVENT_MEMORY_PRESSURE), and its report lists their Lua memory.
            mirage::mem::AddPressureHandler("Mods", 100, [](mirage::mem::Level level, bool) {
                mirage::mods::DispatchMemoryPressure(static_cast<int>(level));
                return std::size_t{0};
            });
            mirage::mem::SetModMemoryProvider([] { return mirage::mods::LuaMemoryByMod(); });
            STARTUP_STEP(mirage::mem::Start());
            const auto toGame = [](std::function<void()> fn) { game::RunOnGameThread(std::move(fn)); };
            tasks::SetDispatcher(tasks::On::GameThread, toGame);
            events::SetDispatcher(events::Where::GameThread, toGame);
            STARTUP_STEP(mirage::hang::Start());   // Watches the game thread's ticks and the render thread's frames
            STARTUP_STEP(mirage::mods::LoadPhase(mirage::mods::Phase::Engine));   // The game layer is up
            // Hot reload: mods whose files changed, checked twice a second (game thread, between the modules' ticks).
            // Normal priority: free unless the mods folders changed, and a save must not wait out a busy menu's deferrals
            sched::AddTask("Mirage mod reload", {0.5f, sched::Priority::Normal, {}, {}}, [] { mirage::mods::ReloadChanged(); });
#if MIRAGE_FEATURE_INPUT
            // The engine's input, seen (mods' input state) and injected (the self-test's key presses)
            if (!STARTUP_STEP(tas::engine::Init()) || !tas::input::Install())
                spdlog::warn("Input hooks unavailable: their patterns were not found");
#endif
        }
        else
        {
            spdlog::error("Game integration unavailable: the menu works, game modules do nothing");
        }
#else
        spdlog::info("Built without game integration (no game features enabled): menu only");
#endif

        sig::Game().Save();   // The pattern scans of startup: the next start skips them
        spdlog::info("Startup finished in {} ms; waiting for the game to draw a frame (EndScene)",
                     GetTickCount64() - attachedAt);

        // The hooks are on the game's D3D9 device methods: if EndScene never runs, the vtable came from a different
        // d3d9 than the one the game draws with (a wrapper/overlay), or the game is not rendering (minimized).
        const ULONGLONG installedAt = GetTickCount64();
        bool warnedNoFrames = false;
        if (selfTest) selftest::Run();   // Returns when the test is done: unload then
        while (!selfTest && !unload::Requested())
        {
            Sleep(50);
            if (!warnedNoFrames && hooks::EndSceneCalls() == 0 && GetTickCount64() - installedAt > 10000)
            {
                warnedNoFrames = true;
                spdlog::error("EndScene hook has not run 10 s after installing: the menu cannot draw. The game is "
                              "minimized/not rendering, or draws through another d3d9 (see the Module lines above)");
            }
        }
        spdlog::info("Unload requested: unloading (EndScene ran {} times)", hooks::EndSceneCalls());
        selftest::Shutdown();   // A run started from the Test module: stopped, settings put back (needs the hooks)

#if MIRAGE_GAME
        mirage::hang::Stop();   // Unloading stalls the threads on purpose: no hang reports for it
#endif
        config::Save();         // Save before DisableAll so the enabled state is kept
        modules::DisableAll();  // Modules queue their cleanup on the game thread...
#if MIRAGE_GAME
        game::Shutdown();       // ...which we wait for before the hooks go away
#endif
        tasks::Stop();          // Background tasks: queued ones dropped, running ones asked to stop, workers joined
#if MIRAGE_GAME
        mirage::mem::Stop();         // Saves what it learnt about the levels; no pressure events to mods from here
        mirage::hooks::RemoveAll();  // Functions back to their own code before this DLL goes
        mirage::detours::RemoveAll();   // Mods' detours, before the hooks are shut down
        mirage::textures::Shutdown();   // Its worker, and the package file redirects (hooks too)
        mirage::inject::RemoveAll();    // Injections out (their functions rebuilt), the stub's native slot freed
        mirage::scripts::RestoreAll();  // Patched functions back to their own bytecode
        mirage::inject::ReleaseNativeSlots();   // Mod natives out of the engine's table: no bytecode calls them now
        mirage::crash::Uninstall();     // The game's own crash handling back, before the hooks are shut down
#endif
        hooks::Uninstall();
#if MIRAGE_GAME
        mirage::mods::UnloadAll();   // Nothing calls into a mod any more
#endif
        if (selfTest) selftest::Finish(true);
    }
    else
    {
        config::Save();
        modules::DisableAll();
#if MIRAGE_GAME
        mirage::mods::UnloadAll();
#endif
        if (selfTest) selftest::Finish(false);
    }

    prof::Stop();           // After the hooks: nothing records zones any more
    spdlog::info("Unloading");
    testmode::Uninstall();
    diag::RemoveCrashLogger();
}

// One menu per game process. Two copies (the installed one from the game's loader plus an injected build) both hook
// the frame timing, input and D3D9: neither TAS works and both menus fight over the mouse. The launcher's "already
// loaded" check can't catch two copies arriving at the same moment, so the first one to get here owns the process.
// False when another copy holds the process. `claim` is closed at unload (nullptr if the mutex couldn't be created at
// all: then nothing is guarded, and this copy runs anyway).
static bool ClaimProcess(HANDLE& claim)
{
    wchar_t name[64];
    swprintf_s(name, L"Local\\MirrorsEdgeMenu-%lu", GetCurrentProcessId());
    claim = CreateMutexW(nullptr, FALSE, name);
    if (claim && GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(claim);
        claim = nullptr;
        return false;
    }
    return true;
}

// Unloads this copy if another one owns the process (not inline in MainThread: __try can't share it with objects).
static HANDLE ClaimOrLeave(HMODULE module)
{
    HANDLE claim = nullptr;
    if (ClaimProcess(claim)) return claim;
    spdlog::error("Another copy of the menu is already running in this game: this one ({}) does nothing and unloads. "
                  "Use one copy: the installed one, or an injected build with the installed one disabled",
                  diag::ModulePath(module));
    OutputDebugStringW(L"MirrorsEdgeMenu: another copy is already running in this process; not starting\n");
    logging::Shutdown();
    FreeLibraryAndExitThread(module, 0);
}

static DWORD WINAPI MainThread(LPVOID param)
{
    const auto module = static_cast<HMODULE>(param);
    paths::Init(module);
    logging::Init();
    HANDLE claim = ClaimOrLeave(module);

    failsafe::Start([](const char* message) {   // First: it must work even if anything after this hangs
        spdlog::critical("{}", message);
        spdlog::default_logger()->flush();
    });
    spdlog::info("Failsafe: {} force-quits the game, even when it stops responding", failsafe::kName);
#ifdef _MSC_VER
    __try { Run(module); }
    __except (diag::LogFatal(GetExceptionInformation(), "Menu thread")) {}   // Logs, then the crash goes on as before
#else
    Run(module);
#endif
    failsafe::Stop();
    logging::Shutdown();
    if (claim) CloseHandle(claim);   // Last: a copy injected after this unload may run
    FreeLibraryAndExitThread(module, 0);
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (HANDLE h = CreateThread(nullptr, 0, MainThread, module, 0, nullptr))
            CloseHandle(h);
    }
    return TRUE;
}
