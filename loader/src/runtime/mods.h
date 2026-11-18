#pragma once
// Mirage mods (steps 4 and 5): the runtime's side of loader/include/mirage/api.h, and the mods' lifecycle.
//
// Each mod is a folder with a mod.toml (runtime/manifest.h), and a DLL (entry) and/or Lua (main.lua). Its modules are
// ordinary menu modules (Module subclasses): they show in the menu, save their settings in the menu's config and run
// through the scheduler like the built-in ones.
//
// Lifecycle:
//   Discover   reads every mod.toml in the folders and decides what loads, in what order (runtime/resolve.h). A mod
//              that crashed the last time (an entry in <folder>/crashes.toml) is left out, with the crash as reason.
//   LoadPhase  loads the mods of a phase, in that order: early at the menu's start, engine once the game layer is up,
//              world when the first level has loaded. Their modules join the menu as a batch when the load is done.
//   Reload     hot reload: the old code's modules are switched off (its on_disable undoes its changes), the code is
//              unloaded and loaded again, and the modules it registers again are the same menu modules: their settings
//              and on/off state carry over. Lua mods reload when their files change; native ones when mod.toml says
//              hot_reload = true (their DLL runs from a copy, so the build can replace the original).
//   a crash    in a mod's code (caught around every call into it) stops the mod, names it in the log with the
//              exception and the place in its DLL, and is written to crashes.toml so it isn't loaded next time.
//   UnloadAll  in reverse load order, at the menu's unload.

#include "mirage/api.h"
#include "runtime/manifest.h"
#include "runtime/touch.h"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mirage::mods
{
	inline constexpr const char* kRuntimeVersion = "0.1.0";

	// The API table every mod gets (Lua mods too, through runtime/mod_lua.cpp)
	const MirageApi* Api();

	// Reads the mods in `folders` (each subfolder with a mod.toml) and resolves the load order. Once, at the start.
	void Discover(const std::vector<std::filesystem::path>& folders);
	// Loads the discovered mods of `phase`. Early: from the menu's thread, after modules::Init and before config::Load
	// and sched::Init. Engine and world: once modules::Start has run (their modules are published to the menu).
	void LoadPhase(Phase phase);
	// Unloads every mod, last loaded first: after modules::DisableAll and the hooks are gone
	void UnloadAll();

	// Loads a mod folder made while the game runs (the script editor's "New mod"): its mod.toml is read, its required
	// mods must be loaded already, and it loads now whatever its phase. Game thread. "" when it loaded, else why not; a
	// mod whose code failed stays listed (Failed), so fixing its files hot reloads it.
	std::string LoadNew(const std::filesystem::path& dir);

	// Hot reload of a loaded mod. Game thread (between ticks: no on_tick is running). "" when it reloaded, else why not.
	std::string Reload(const std::string& id);
	// Reloads the mods whose files changed. Game thread; runs as a scheduler task every half second, and walks the mods'
	// files only after the mods folders changed (a watcher thread waits on change notifications).
	void ReloadChanged();

	// A crash outside the guarded calls (a thread a mod started): the mod whose DLL holds `address`, "" if none. Writes
	// the crash record for it. Any thread, from the crash handler (which sees exceptions first: one the mod's own
	// __try handles on a thread it started would be blamed too; the calls the runtime guards are told apart: "" and
	// *guarded set, since their guard reports the fault).
	std::string AttributeCrash(const void* address, unsigned long code, bool* guarded);
	// The id of the loaded mod whose native DLL `module` is ("" if none). Any thread, no lock (for the crash handler).
	std::string ModOfModule(const void* module);

	// The mods <folder>/crashes.toml lists, id -> why they won't load (what Discover leaves out)
	std::map<std::string, std::string> CrashedMods(const std::filesystem::path& folder);

	struct Info
	{
		std::string id, name, version, kind;	// kind: "native", "lua", "native + lua" or "content"
		std::filesystem::path dir;
		Phase phase = Phase::Engine;
		bool loaded = false;
		bool script = false;					// Its code is source the game compiles: a C# script (.cs) or Lua
		int loadIndex = -1;						// Order it loaded in (0 first), -1 if it didn't
		int reloads = 0;
		std::string problem;					// Why it isn't loaded (left out, failed, crashed)
		std::vector<std::string> modules;		// Names of the modules it registered
		std::vector<std::string> warnings;
		// For the mod manager
		std::string description, authors;
		std::vector<std::string> required, optional, incompatible;	// "id range" each, as mod.toml says
		std::vector<std::string> dependents;	// The mods that require this one
		bool offNextStart = false;				// Switched off in the mod manager (disabled.toml)
		bool crashed = false;					// In crashes.toml: not loaded until allowed again
		struct Action
		{
			std::string name, key, defaultKey, description;	// key: the one it is on now (the player's, or mod.toml's)
		};
		std::vector<Action> actions;			// Its input actions (mod.toml [actions.<name>])
	};
	// Each loaded Lua mod's Lua memory, biggest first: {id, bytes}. Game thread (reloads replace the states there).
	std::vector<std::pair<std::string, std::size_t>> LuaMemoryByMod();

	// The console (step 27's REPL): a line of Lua run in a loaded Lua mod's own state, guarded like any call into the
	// mod (a crash stops the mod, not the game) and timed by its profiler as "console line". Game thread.
	struct EvalResult
	{
		bool ok = false;
		std::string value;		// The first value it returned, as text ("" for none)
		std::string error;		// Lua's message, or why it couldn't run
		std::string printed;	// What it printed, a line per print
	};
	EvalResult Eval(const std::string& modId, const std::string& code);
	// The loaded mods with a Lua state (for the console's list). Any thread.
	std::vector<std::string> LuaMods();

	// Every mod found, loaded or not, in load order then the rest
	std::vector<Info> List();

	// ---- Safe mode and conflicts (step 10) -----------------------------------------------------------------------------
	// Why no mod loads in this session: the last start didn't reach the first level (<mods>/starting.toml was still
	// there), or Shift was held as the game started. "" normally.
	std::string SafeModeReason();
	// The start got to the first level: the marker goes (the next start is a normal one)
	void StartupFinished();
	// Where the touch sets of the mods that load overlap (runtime/touch), found before any of them loaded
	std::vector<touch::Overlap> Overlaps();

	// The loaded mods' input actions (mod.toml [actions.<name>]) on the key `key` (the engine's name, any case):
	// {mod id, action name} each, in load order
	std::vector<std::pair<std::string, std::string>> ActionsForKey(const std::string& key);
	// Puts one of a mod's input actions on `key` (the engine's key name), saved in the mod's config.toml; "" puts it back
	// on mod.toml's key. False: no such mod or action, or the file couldn't be written.
	bool SetActionKey(const std::string& modId, const std::string& action, const std::string& key);

	// ---- The mod manager (step 9) --------------------------------------------------------------------------------------
	// Loaded at the next start or not (disabled.toml in its mods folder). Switched off, its modules go off now too.
	// The menu's thread (where modules are switched).
	bool SetEnabledNextStart(const std::string& id, bool on);
	// Takes the mod out of crashes.toml: it is tried again at the next start
	bool ForgetCrash(const std::string& id);
}
