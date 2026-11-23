# C# mods

Mirage mods can be written in C#, next to native (C/C++) and Lua mods, against the same API. The menu bundles the
.NET runtime (10, LTS, x86: the game is a 32-bit process) in `mirage\dotnet\` and starts it the first time a C# mod
loads; a game without C# mods never loads it.

## A mod

```
mirage\mods\hello-cs\
    mod.toml          id = "hello-cs", version = "1.0.0", dotnet = "HelloCs.dll"
    HelloCs.dll       your assembly (and HelloCs.pdb: line numbers in error messages)
```

```csharp
using Mirage;

public sealed class HelloMod : Mod                    // exactly one class derives from Mirage.Mod
{
    public override void OnLoad()
    {
        new SpeedModule(this);
        On(GameEvent.LevelLoad, e => { if (e.Value == 1) Log.Info("Loaded " + e.Text); });
        Hook("TdGame.TdPlayerController.AttackPress", HookWhen.Before, call => Log.Info("attack"));
    }
}

public sealed class SpeedModule : Module
{
    private readonly Setting _max;
    public SpeedModule(Mod mod) : base(mod, "Speed", "Mods", "Shows the speed") => _max = Slider("Max", 1000, 0, 5000);

    protected override void OnTick()                    // game thread
    {
        var pawn = GameObject.Player;
        if (pawn.IsValid && pawn.GetFloat("Velocity.X") > _max.Float) pawn.SetFloat("Velocity.X", _max.Float);
    }

    protected override void OnRender() => Draw.Text(20, 20, 0xFFFFFFFF, "C#!");   // render thread
}
```

Project: a `net10.0` class library that references `Mirage.dll` (`mirage\dotnet\mirage\Mirage.dll`, or
`managed/Mirage/Mirage.csproj` in this repository) without copying it (`Private="false"`). See
`examples/mods/hello-cs`. Build with `dotnet build -c Release`.

## A script mod (no SDK, no build)

```
mirage\mods\hello-script\
    mod.toml          id = "hello-script", version = "1.0.0"   (dotnet = "main.cs" is implied by the file)
    main.cs           every .cs in the folder is compiled together, in the game (Roslyn), when the mod loads
```

`System`, `System.Linq`, `System.Collections.Generic` and `Mirage` are imported already. A compile error doesn't load
the mod; the mod manager shows why with the file and line (`main.cs(5,17): error CS0029: ...`). With
`hot_reload = true` saving a file compiles and reloads it. See `examples/mods/hello-script`. Roslyn is loaded only when a
script compiles: a game with only built (DLL) C# mods never pays for it. A compiled script is kept in
`<mirage>\cache\scripts\` by its sources (and Mirage's and the compiler's versions): the same sources load in
milliseconds instead of compiling again (the log says "loaded from the script cache"), and any change compiles anew.

## What the API covers

| C# | Native API |
|----|-----------|
| `Mod`: `Id`, `Version`, `Folder`, `Log`, `Config`/`SetConfig`, `On(GameEvent, ...)`, `Hook(function, when, ...)`, `Inject`, `Native`, `Notify` | `mod_*`, `log`, `config_*`, `event_subscribe`, `hook_function`, `inject_function`, `native_register`, `notify` |
| `Mod.LoadClass(path)` (a `.uc` file), `Mod.DefineClass(ClassSpec)`, `Mod.Keep/Release` ([new classes](mod-classes.md)) | `class_load`, `class_define`, `object_keep` |
| `Module`: `OnEnable/OnDisable/OnTick/OnRender/OnTrigger/OnPanel`, `Enabled`, `Checkbox/Slider/SliderInt/Key` settings | `module_register`, `setting_*` |
| `GameObject`: `Player`, `Controller`, `World`, `Find`, `Get/Set(path)`, `Name`, `ClassName`, `Path`, `IsA` | `player_*`, `object_*` |
| `Call` (in a hook): `Object`, `Function`, `Get/Set(parameter)`, `Skip` | `call_*` |
| `Draw`: `Screen`, `Text`, `Line`, `Rect`, `RectFilled`, `WorldToScreen` | `draw_*`, `world_to_screen` |

Everything else in the API is reachable through `Mirage.Native.MirageApi` (generated from `loader/include/mirage/api.h` by
`tools/gen_csharp_api.py`: every function, in order, as an unmanaged function pointer); the friendly layer grows as
mods need it.

## Safety

- **Exceptions never reach the game.** A callback that throws is logged with the mod's id, the module and the stack
  (with the .pdb, file and line), and the module is switched off. Event and hook handlers are logged and skipped.
- **Hot reload unloads for real.** Each mod has its own collectible `AssemblyLoadContext`; files are read into memory
  (never locked), so rebuilding while the game runs is picked up. On unload the mod's modules, events and hooks are
  released; the self-test checks that the old context is collected (a mod that keeps a reference to its own objects
  in a static somewhere outside its assembly would keep it alive: the memory report says so).
- **Conservative runtime settings** (`Mirage.runtimeconfig.json`): no background GC thread, memory given back, a
  128 MB GC heap cap, no ICU. The runtime's own memory lives outside the game's (TAS snapshots leave it alone).
- **Threads:** callbacks run on the thread the API documents (game thread for ticks, events and hooks, render thread
  for drawing); game objects are only valid on the game thread, within the tick.
- **ABI:** `Mirage.dll` checks at start that the runtime's API table is at least as large as the one it was built
  against and that the shared structs have the C sizes; a mismatch refuses to start (logged) instead of misbehaving.

## Measured (debug menu build, in the game)

- The runtime starts in 70-150 ms, the first time a C# mod loads.
- It takes **~89 MB of address space** (free address space before and after starting it). The GC heap cap is not
  where it goes (a 32 MB cap measured the same); which part of the runtime reserves it is not known yet. In a 32-bit
  game this is the cost that matters: it is only paid when a C# mod is installed.
- A C# module's tick that reads the player by path: ~10 us.
- After a hot reload the old assembly context is collected (0 alive).
- A script's first compile (Roslyn starting) ~1.2 s, on the game thread while mods load; a check of unsaved text
  afterwards ~13 ms.

## Not yet

The C# layer doesn't wrap the UI layer (`MirageUi`), script patching, detours or the TAS service yet (all reachable
through `Mirage.Native`). The installer doesn't ship the runtime yet: `mirage game-test` copies it, a release build
needs the installer to include `mirage\dotnet\`.
