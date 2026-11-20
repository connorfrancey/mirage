# Example Mirage mods

A mod is a folder in the menu's `mods` folder (`<game>\Binaries\mirage\mods\<id>`), named after its id, with a
`mod.toml` and its code. Both examples add one module, "Hello", to the menu's **Mods** column. While it is on it draws
a greeting, counts the game's ticks, and counts how many times it was switched on in its own `config.toml`.

| Folder       | Kind   | How to install                                                                     |
|--------------|--------|------------------------------------------------------------------------------------|
| `hello-lua/` | Lua    | Copy the folder into `mods`. No build.                                             |
| `hello/`     | Native | Build it (below), then copy `<build>/mod/hello` into `mods`.                       |

## mod.toml

```toml
id = "hello"              # a-z, 0-9, '-', '_'; the folder's name
name = "Hello"
version = "1.0.0"         # semver
mirage = "^0.1"           # runtime versions it works with
entry = "hello.dll"       # native code (or lua = "main.lua", the default when main.lua exists)

[dependencies]            # other mods, with version ranges ("^1.2", ">=1.0 <2", "*")

[settings.greeting]       # a setting, shown in the module's settings panel; the menu's config keeps its value
type = "string"           # bool, int, float, string, choice or key
name = "Greeting"
description = "What it draws"
default = "Hello"

[settings.height]
type = "float"
default = 0.2
min = 0.0                 # int and float need a range
max = 0.95

[settings.color]
type = "choice"
options = ["Blue", "Orange", "White"]
default = "Blue"
```

Declared settings go into the mod's first module, or into the module named by `module = "..."`. Code reads them by
key: `mirage::Get<std::string>("greeting")` in C++, `mirage.setting("greeting"):get()` in Lua. A module can also make
its own settings in code (`mirage::Slider` members, `module:slider(...)`), which are listed after the declared ones.

The full list of keys is in `loader/src/runtime/manifest.h`. Unknown keys are reported in the menu's log.

## Native mods

Include `loader/include/mirage/mirage.hpp`, which is a header-only C++ wrapper over the C ABI in `loader/include/mirage/api.h`. Write
modules the way the menu's own are written, then add `MIRAGE_MOD()` once:

```cpp
class Hello final : public mirage::Module {
public:
    Hello() : Module("Hello (native)", "Mods", "Draws a greeting") {}
    void OnRender() override { mirage::DrawText(20, 200, mirage::Rgba(255, 255, 255), greeting.Get()); }
private:
    mirage::Text greeting{this, "Greeting", "Hello"};
};
MIRAGE_MODULE(Hello)
MIRAGE_MOD()
```

The game is 32-bit, so build the DLL for x86. Only C types cross into the menu, so the compiler doesn't have to match
the menu's: MSVC and clang-cl both work.

```
cmake -S examples/mods/hello -B build/hello -G Ninja -DCMAKE_BUILD_TYPE=Release     # from an x86 developer prompt
cmake --build build/hello
```

## Lua mods

`main.lua` runs in a sandbox: there is no `io` or `os`, and `require` only loads files from the mod's own folder. It
gets the same API as native mods:

```lua
local m = mirage.module { name = "Hello (Lua)", on_render = function()
    mirage.draw.text(20, 240, mirage.rgba(255, 200, 120), Greeting:get())
end }
Greeting = m:text("Greeting", "Hello from Lua")
```

`on_tick` runs on the game thread and `on_render` on the render thread. If a callback raises an error, the error is
logged and its module is switched off.

## Load order, phases and hot reload

- Mods load after the mods in their `[dependencies]` and `[optional]` lists, within their `phase`. A mod that can't
  load (a missing dependency, a cycle, a runtime it doesn't support) is left out, and the menu's log says why.
- `phase = "early"` loads at the menu's start, `"engine"` (the default) once the game layer is ready, and `"world"` when
  the first level has loaded.
- Save a Lua mod's file and the mod reloads by itself: its modules stay on, and their settings keep their values. A
  native mod does the same with `hot_reload = true`. Its DLL runs from a copy, so a new build can replace it while the
  game runs. Its `OnDisable` must undo everything the mod changed.
- If a mod crashes, it is stopped and named in the log, and the rest of the menu goes on. The crash is written to
  `mods\crashes.toml`, and the mod isn't loaded again until its entry there is deleted.

## The game: objects, events and hooks

Lua:

```lua
local pawn = mirage.pawn()                       -- the player (nil while loading); mirage.controller(), mirage.world()
pawn.Location.Z = pawn.Location.Z + 500          -- properties and struct members by name
pawn.Velocity = { X = 0, Y = 0, Z = 0 }          -- a struct from a table
mirage.call(pawn, "SetLocation", { NewLocation = pawn.Location })   -- a function, with its parameters by name

mirage.on("level_load", function(finished, map) if finished then mirage.log("now in", map) end end)
-- events: tick (seconds), level_load (finished, map), death (after), player_state (new, old), key (vk), state_restored,
--         actor_added (actor, class), actor_removed (actor), level_streamed (visible, level), restart

-- The world (step 14): actors in play, every filter optional; with a radius, nearest first
for _, bot in ipairs(mirage.actors { class = "TdBotPawn", near = pawn, radius = 2000 }) do mirage.info(mirage.name(bot)) end
mirage.actors { tag = "Door" }                   -- by Actor.Tag; also name = "TdBotPawn_3", near = {x, y, z}
mirage.valid(obj)                                -- false once the object is gone (or another took its place)

-- Vectors: mirage.vec, a {x, y, z} table with math (anything that takes {x, y, z} takes one; the API's locations,
-- velocities and hits are vectors)
local v = mirage.vec(1, 2, 3)                    -- or mirage.vec{x = 1, y = 2, z = 3}, mirage.vec{1, 2, 3}, mirage.vec(5)
local ahead = (v + mirage.vec.forward * 100):normalized()   -- + - * / and unary -, by a number or each component
-- v:len(), v:len2(), v:dist(w), v:dot(w), v:cross(w), v:lerp(w, 0.5), v:copy(), v:unpack()
-- mirage.vec.zero, .up (the game's Z is up), .forward, .right, mirage.vec.is(x), tostring(v) == "vec(1, 2, 3)"

-- A setting in the game's own menu: a slider on Pause > Options > Mod settings (the game's UI draws it; its Defaults
-- puts back `value`, its Cancel undoes a change). C: menu_slider / menu_slider_value; C#: MenuSlider / MenuSliderValue
local strength = mirage.menu_slider { label = "Tint strength", min = 0, max = 1, value = 0.5, step = 0.1 }
mirage.on("tick", function() local v = mirage.menu_slider_value(strength) end)

local hook = mirage.hook("Engine.Actor.CheckMaxEffectDistance", function(call)
    call.params.CullDistance = 0                 -- before the function: change its parameters...
    -- call:skip()                               -- ...or don't run it
end, { when = "before", priority = 10 })         -- "after" sees call.params.ReturnValue; "replace" sets it
hook:remove()
```

C++ (`mirage.hpp`): `mirage::Object::Pawn().Set("Location.Z", 500.f)`, `mirage::On(MIRAGE_EVENT_TICK, ...)`,
`mirage::Hook("Engine.Actor.SetLocation", MIRAGE_HOOK_BEFORE, 0, [](mirage::Call& c) { ... })`, and for native code
`mirage::FindPattern(...)` and `mirage::Detour(target, &Mine, priority, &original)`. Several mods can detour one
function, and they run by priority.

Hooks run on the game thread. A mod's hooks, events and detours go away when it unloads, reloads or crashes. A TAS
rewind puts the game's memory back but never a mod's: after `state_restored`, look game objects up again.

Objects kept across ticks are safe to keep: a Lua object knows the object it was made for (its slot in the engine's
object list, its name and its class), and once that object is gone (destroyed, a level unloaded, garbage collected, or
another object at the same address) it reads as nothing and calls on it fail, instead of reaching another object.
`mirage.valid(obj)` says which.

Reversible changes (step 15): writes inside a transaction are recorded with the values they replace, and put back when
the mod asks, and by the runtime when it unloads, reloads or crashes. Writes outside one stay as they are (moving the
player, setting its health: gameplay a hot reload mustn't undo).

```lua
local id = mirage.transaction("low gravity", function()
    local world = mirage.world()
    world.WorldGravityZ = world.WorldGravityZ * 0.5      -- recorded: put back by mirage.revert(id) or the unload
end)
mirage.revert(id)          -- or mirage.revert(): every transaction of the mod; mirage.changes() lists them
```

C# has `Transaction(name, () => ...)`, `Revert(id)` and `ChangesJson`; C, `change_begin` / `change_end` / `change_revert`
/ `change_list`. The mod manager lists each mod's transactions, with "Put back". Native mods get the same with `object_handle` / `handle_object` (a `MirageHandle` is
plain data), C# with `GameObject.HandleOf()` / `GameObject.Resolve()`. `actor_removed` gives the actor that went, `==`
to the one a mod kept (and no longer valid). Subscribing to `actor_added` / `actor_removed` makes the runtime compare
the levels' actor lists each tick: subscribe only while you need them.

## Content mods: data, no code

A mod with `content = ["content"]` in its `mod.toml` changes the game through files in that folder, and gets a module
named after it that turns them on and off.

| Folder        | What it shows                                                                   |
|---------------|---------------------------------------------------------------------------------|
| `high-steps/` | `content/defaults.toml`: default property overrides (the player steps up higher) |
| `retexture/`  | `content/textures/M_Rooftops.Textures.T_Roof_02_D.png`: a replaced texture       |

A replacement texture is named by the texture's full path (outer packages and groups, dots between them). PNG, TGA, BMP
and JPG images are encoded to the original's format with a full mip chain; a DDS (DXT1/3/5, 32-bit ARGB or 8-bit
luminance) is used as stored. Sides must be powers of two. The game's files are never written: the packages holding the
texture are opened as rewritten copies kept in `mirage\cache\textures`. A change shows on the next load of a level not
loaded yet this session, and everywhere after a restart.

