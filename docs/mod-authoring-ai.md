# Creating a Mirage mod with an AI coding tool

Read this before building or editing a Mirror’s Edge mod. A Mirage mod is a directory with a `mod.toml` and Lua, C#, a native DLL, or game content. Each example below is a separate mod with its own manifest.

## Start by checking the repository

1. Read the repository’s `AGENTS.md` and the authoring commands in [Getting started](getting-started.md).
2. Inspect an existing mod that uses the same mechanism. Use `examples/mods/hello-lua` for Lua modules, `examples/mods/high-steps` for class-default overrides, `tests/mods/layer-test` for actor layers, and `examples/mods/tech-demo` for a multi-system mod with cleanup and in-game tests.
3. Confirm API names, arguments, return values, and callback threads. `loader/src/runtime/mod_lua.cpp` and `loader/src/runtime/lua/lua_*.cpp` define the actual Lua surface. The [C ABI reference](api-reference.md) describes underlying operations. For game classes and fields, use `uv run mirage sdk-browse TdGame.TdPawn` or the checked-in SDK headers. The [SDK guide](documentation/sdk.md) explains metadata export.
4. Choose the smallest mechanism that implements the requested behavior. Choose content files for property defaults, asset replacement, or static level additions. Choose Lua for input, callbacks, UI, or dynamic behavior. Use C# or a native x86 DLL when an existing project or a required interface calls for it.
5. Give the mod a unique lowercase, hyphen-separated ID, a semantic version, and a compatible `mirage` range in `mod.toml`. Keep the manifest beside the files the runtime loads. A Lua mod can use `main.lua` or `src/main.lua`. Lua `require("name")` resolves the helper beside the entry file, and dots in a module name indicate subdirectories.

Use real class and object paths from this checkout. UE3 property layout and SDK metadata describe the engine. The Lua runtime’s bindings determine what a script can call. Generated MirageAPIGen inspection definitions may include additional metadata-only members.

## Simple mod · a toggleable HUD label

**Goal.** Put a switch in the Mods tab. While enabled, it draws a label in the game window. This mod has no engine object access and keeps no state between callbacks.

```text
hello-hud/
  mod.toml
  src/
    main.lua
```

`hello-hud/mod.toml`

```toml
id = "hello-hud"
name = "Hello HUD"
version = "1.0.0"
description = "Draws a label while the module is enabled."
mirage = "^0.1"
hot_reload = true
```

`hello-hud/src/main.lua`

```lua
local white = mirage.rgba(255, 255, 255, 255)

mirage.module {
    name = "Hello HUD",
    tab = "Mods",
    description = "Shows a label in the top-left corner.",
    on_enable = function()
        mirage.log("Hello HUD enabled")
    end,
    on_render = function()
        mirage.draw.text(24, 64, white, "Hello from Mirage")
    end,
}
```

You can start from `uv run mirage new hello-hud --lang lua` and replace the generated manifest and entry script with these files. Keep the draw call inside `on_render`. A separate `on_disable` cleanup is unnecessary here because the module only draws during enabled frames.

```sh
uv run mirage mod check hello-hud
uv run mirage mod build hello-hud
```

After installing with `uv run mirage mod install hello-hud`, open the menu with **Insert** and toggle **Hello HUD**. Check that the label appears while enabled and disappears while disabled. Change the label text and reinstall to check reload behavior. Look for the mod ID in the log and for the enabled module in the Mod Manager.

## Content mod · raise the player’s step height

Generate a working starter with `uv run mirage new high-steps --lang content`. It includes the same property
override and a guide to adding replacement assets. A content-only mod has no source code or compilation step.

**Goal.** Let Faith step over slightly taller edges through a reversible property override. The runtime manages application and undo.

```text
gentle-steps/
  mod.toml
  content/
    defaults.toml
```

`gentle-steps/mod.toml`

```toml
id = "gentle-steps"
name = "Gentle Steps"
version = "1.0.0"
description = "Increases the maximum ledge height the player can step up."
mirage = "^0.1"
content = ["content"]
```

`gentle-steps/content/defaults.toml`

```toml
["TdGame.Default__TdPlayerPawn"]
MaxStepHeight = 52.5
instances = true
```

The object path identifies the player pawn’s class defaults. `MaxStepHeight` is the reflected property name. `instances = true` also applies the override to live matching pawns, so you can test against the pawn already in the level. Future player spawns receive the changed defaults while the mod is enabled.

```sh
uv run mirage mod check gentle-steps
uv run mirage mod package gentle-steps
```

Confirm the packaged ZIP has `mod.toml` and `content/defaults.toml` at the expected paths. In the game, compare the same ledge before and after toggling the mod. Restart the checkpoint to test a fresh pawn, then disable the mod and check that the prior step behavior returns. Check for other mods that touch the same property in the Mod Manager.

For a different content task, use the loader’s existing paths. Place per-map actor declarations under `levels/<package>.toml`, where `<package>` is the map package such as `edge_p`. Use an explicit actor name and an existing fully qualified class; see `tests/mods/layer-test/levels/edge_p.toml` for the schema. Place texture replacements in `content/textures` using the asset’s fully qualified object path as the filename stem. For mesh conversion, follow [Exporting models](models.md). Use authored or licensed assets and keep game packages outside commits.

### Replace a sound

With `content = ["content"]` in `mod.toml`, place an Ogg Vorbis file at
`content/sounds/<full SoundNodeWave path>.ogg`. For example, this filename targets a construction ambience wave.

```text
content/sounds/A_Ambience_Construction.Hammering_RAW.A_Ambience_Construction_Hammering_02.ogg
```

Use a mono or stereo Vorbis stream at up to 48 kHz and 32 MiB per file. This command converts a WAV to mono Vorbis.

```powershell
ffmpeg -i replacement.wav -ac 1 -ar 44100 -c:a libvorbis replacement.ogg
```

Target the wave used by the cue. Mirage replaces the sample and updates its duration, channel count, and sample rate.
The game still controls the cue's volume, looping, and spatial behavior. Enable the mod before opening the level with
the wave. Packages already opened keep their current version until the game restarts.

## Complex mod · a session peak-speed tracker

**Goal.** Sample player movement on the game thread, publish a short HUD string to the render callback, and clear the recorded peak on demand or when the player’s session changes. Keep the peak calculation in a separate Lua module so it can be tested with synthetic values.

```text
session-speed/
  mod.toml
  src/
    main.lua
    meter.lua
```

`session-speed/mod.toml`

```toml
id = "session-speed"
name = "Session Speed"
version = "1.0.0"
description = "Shows current and peak movement speed."
mirage = "^0.1"
hot_reload = true

[actions.reset_peak]
key = "F8"
description = "Clear the recorded session peak."

[settings.hud]
type = "bool"
name = "Show speed HUD"
default = true
```

`session-speed/src/meter.lua`

```lua
local meter = {}

function meter.new()
    return { peak = 0 }
end

function meter.reset(state)
    state.peak = 0
end

function meter.sample(state, speed)
    if type(speed) ~= "number" or not (speed >= 0 and speed < math.huge) then
        return nil
    end
    state.peak = math.max(state.peak, speed)
    return state.peak
end

return meter
```

`session-speed/src/main.lua`

```lua
local meter = require("meter")
local state = meter.new()
local subscriptions = {}
local hud
local snapshot
local reset_requested = false
local white = mirage.rgba(255, 255, 255, 255)

local function request_reset()
    reset_requested = true
end

local function stop()
    for _, subscription in ipairs(subscriptions) do
        subscription:remove()
    end
    subscriptions = {}
    meter.reset(state)
    snapshot = nil
    reset_requested = false
end

local function start()
    stop()
    subscriptions[#subscriptions + 1] = mirage.on("action", function(name, pressed)
        if name == "reset_peak" and pressed then
            request_reset()
        end
    end)
    for _, event in ipairs({ "level_load", "death", "restart", "state_restored" }) do
        subscriptions[#subscriptions + 1] = mirage.on(event, request_reset)
    end
end

mirage.module {
    name = "Session Speed",
    tab = "Mods",
    description = "Tracks movement speed and the session peak.",
    on_enable = start,
    on_disable = stop,
    on_tick = function()
        if reset_requested then
            meter.reset(state)
            reset_requested = false
        end
        local movement = mirage.movement()
        local peak = movement and meter.sample(state, movement.speed)
        snapshot = peak and ("Speed %.0f  |  Peak %.0f"):format(movement.speed, peak) or nil
    end,
    on_render = function()
        if snapshot and hud and hud:get() then
            mirage.draw.text(24, 90, white, snapshot)
        end
    end,
}

hud = mirage.setting("hud")

mirage.test("session peak tracks valid samples and resets", function()
    local sample = meter.new()
    assert(meter.sample(sample, 200) == 200)
    assert(meter.sample(sample, 100) == 200)
    assert(meter.sample(sample, math.huge) == nil)
    meter.reset(sample)
    assert(meter.sample(sample, 30) == 30)
end)
```

The module samples state through `mirage.movement()` only on the game thread. It stores a Lua string for rendering and avoids keeping player proxies across ticks. A missing player clears the HUD snapshot. The next valid sample after a reset is the new peak, so pressing **F8** while moving can immediately display a nonzero peak. Subscriptions belong to the enabled module. Removing them in `stop` prevents old callbacks from handling later events after disable or reload.

```sh
uv run mirage mod check session-speed
uv run mirage mod build session-speed
uv run mirage mod test session-speed
```

`mirage mod test` installs the mod and launches Mirror’s Edge with the last loader build. It runs the registered calculation test. Verify the integration separately by moving, slowing down, pressing **F8**, restarting a checkpoint, and toggling the module and HUD setting. Repeat a disable/re-enable cycle and watch for duplicate action handling or stale output.

For a mod that also spawns objects, installs hooks, registers callbacks, or modifies reflected properties, track the ownership of each resource and release or restore it on disable and unload. Use a reversible transaction where the existing binding supports it. The Field Lab example shows a session teardown with hook removal and `mirage.revert`. Preserve teardown on partial initialization failures as well as normal disable.

## Declare actions and settings

Use separate TOML files when a mod has more than a few declarations. All paths are relative to the mod folder.
Mod IDs can use dot-separated namespaces. The installed folder name must match the ID.

```toml
id = "cryptigo.field-lab"
name = "Mirage Field Lab"
version = "1.0.0"
mirage = ">=0.1.0"
hot_reload = true
actions = "config/actions.toml"
settings = "config/settings.toml"

[entrypoints]
lua = "scripts/main.lua"
```

In `config/actions.toml`, use an engine key name or a keyboard chord.

```toml
deploy = "F6"
clear = "F7"

[scanner]
key = "Ctrl+F6"
label = "Toggle Scanner"
description = "Enable or disable the scanner"
```

In `config/settings.toml`, a scalar or the `default` field determines the type. `options` infers an enum.
Use `type = "keybind"` for a keyboard or mouse binding. A numeric setting with a range displays a slider.

```toml
hud = true

[volume]
default = 0.5
range = [0.0, 1.0]
step = 0.05

[scanner_mode]
default = "normal"
options = ["normal", "advanced", "experimental"]

[scanner_key]
type = "keybind"
default = "Ctrl+F7"
```

For inline declarations, put compact actions in `[actions]`, expanded actions in `[actions.<id>]`, scalar settings
in `[settings]`, and expanded settings in `[settings.<id>]`. Existing `choice`, `key`, `color`, `min`, and `max`
declarations still work. The legacy top-level `entry`, `lua`, and `dotnet` fields remain supported. The matching
`[entrypoints]` names are `native`, `lua`, and `dotnet`.

With hot reload enabled, valid edits to external action declarations take effect without restarting the code.
Settings-only edits to defaults keep the existing controls and user-changed values. Restart after adding or removing
settings, changing their metadata, or editing their defaults alongside the mod code. Invalid edits leave the active
declarations in use.

Run `uv run mirage mod package <folder> --format miragemod` to build an MPAF archive with the TOML manifest and
its referenced declaration files. If the packer is absent, build it with `uv run mirage build -t mirage_pack`.
The package command still defaults to ZIP.

## Verification and handoff

| Check | What it covers |
| --- | --- |
| `mirage mod check <folder>` | Checks the manifest, entry file types, relative paths, and declared content directories before building or installing. |
| `mirage mod build <folder>` | Buildable artifacts for compiled projects. Lua and content mods pass through as files. |
| `mirage mod package <folder>` | The archive layout a player installs. Inspect it for the manifest, assets, and relative module files. |
| `mirage mod test <folder>` | The mod’s registered tests in the actual game. The command installs the mod and starts the game. |
| In-game smoke check | Visible behavior, level transitions, disable/unload, re-enable, reload, and conflicts with other mods. |

Use `uv run` before the commands in the table from this checkout. Coordinate any install or game launch with the person using the machine. Record the command and result for each check you ran. Include the mod folder, supported runtime, how to enable and remove it, the files or properties it changes, and any behavior still awaiting a real game check.
