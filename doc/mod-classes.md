# New classes from mods

A mod can add classes to the game: new actors, new objects, subclasses of the game's own. The engine loads them like
its cooked classes: they spawn, take part in collision and reflection, run their script in the game's VM, and show up
everywhere the game's classes do. There are two ways to make one.

## 1. Write it in UnrealScript

Put a `.uc` file in the mod's folder and load it:

```lua
local Pad = mirage.load_class("classes/MirageBouncePad.uc")      -- Lua (game thread: a tick, an event)
```
```csharp
var pad = LoadClass("classes/MirageBouncePad.uc");                // C#
```

The file holds one class, named like the file. The runtime compiles it with UnrealLib's compiler (no SDK, no editor)
into a script package in `memenu\classes\` and the engine loads it from there. The first compile in a session reads the
game's script packages (about 3 s); later ones take a fraction of a second, and unchanged sources aren't compiled again.
`examples/mods/bounce-pad` is a complete mod.

```unrealscript
class MirageBouncePad extends Trigger
    placeable;

var() float Strength;
var int Bounces;

event Touch(Actor Other, PrimitiveComponent OtherComp, Vector HitLocation, Vector HitNormal)
{
    local Pawn P;

    P = Pawn(Other);
    if (P == None)
        return;
    Bounces++;
    P.Velocity.Z = Strength;
    P.SetPhysics(PHYS_Falling);
    `log("bounce" @ Bounces);
}

defaultproperties
{
    Strength=1500.0
    bNoDelete=False     // A Trigger is a level's own actor; a spawned one must not be bNoDelete
}
```

What a class can declare: constants, enums, structs (with `structdefaultproperties`), variables with every modifier
(static arrays sized by a number, an enum or a constant), functions and events (`simulated`, `static`, `exec`,
`singular`, `latent`, `iterator`, optional/out/coerce parameters with defaults), delegates, states (`auto`, `ignores`,
state functions, state code with labels and latent calls like `Sleep`, a state extending another or the parent's),
`defaultproperties` with subobjects (`Begin Object ... End Object`, nested too) and dynamic arrays (`Arr(0)=`,
`Arr.Empty`, `Arr.Add(x)`, `Arr.Remove(x)`), class modifiers (`within`, `config`, `placeable`, `abstract`,
`transient`, `editinlinenew`, `hidecategories`, ...), `implements` of script interfaces, and `interface` declarations.
The compiler follows the UE3 compiler's rules: automatic conversions (bytes and enums compare as ints), constants
inlined, `` `log``/`` `warn``. Not available to script classes (they need C++): replication blocks, native interfaces,
`map{}` variables, native classes.

### Native functions from the mod

A function declared `native("Name")` runs the mod's own native of that name (`mirage.native`, `Mod.Native`): register
it before loading the class.

```lua
mirage.native("double", "float(float)", function(x) return x * 2 end)
-- in the class:  native("double") function float Twice(float X);
```

## 2. Describe it from Lua or C#

The runtime writes the UnrealScript, and the class's functions are the mod's own code:

```lua
local Pad = mirage.define_class {
    name = "MyPad", extends = "Trigger", placeable = true,
    vars = { "var() float Strength", "int Bounces" },
    defaults = { Strength = 1500.0, bNoDelete = false },
    functions = {
        -- An override: the parent's declaration. Its code gets the object, its parameters (read and written by name)
        -- and the call; a value it returns is the function's return value.
        Touch = function(self, params)
            local other = params.Other
            if not other or not mirage.is_a(other, "Pawn") then return end
            self.Bounces = self.Bounces + 1
            other.Velocity.Z = self.Strength
            mirage.call(other, "SetPhysics", { newPhysics = 2 })
        end,
        -- A new function: its declaration and its code
        TimesBounced = { "function int TimesBounced()", function(self) return self.Bounces end },
    },
    extra = "",   -- More UnrealScript as written (states, structs, functions with bodies)
}
```
```csharp
DefineClass(new ClassSpec
{
    Name = "MyPad", Extends = "Trigger", Placeable = true,
    Vars = { "var() float Strength", "int Bounces" },
    Defaults = { "Strength=1500.0", "bNoDelete=False" },
    Functions =
    {
        ["Touch"] = call => { /* call.Object, call.Get("Other"), call.Set(...) */ },
        ["function int TimesBounced()"] = call => { call.Set("ReturnValue", Value.From(3)); call.Skip(); },
    },
});
```

The source it wrote is in `memenu\classes\generated\<mod>\`. Each function's code runs at the head of its (empty) body
through a script injection, so it costs what an injection costs (about 0.1 µs plus the Lua or C# call).

## Spawning

```lua
local pawn = mirage.pawn()
local at = pawn.Location
mirage.call(pawn, "Spawn", { SpawnClass = Pad, bNoCollisionFail = true,
                             ["SpawnLocation.X"] = at.X, ["SpawnLocation.Y"] = at.Y, ["SpawnLocation.Z"] = at.Z })
```

A call from Lua or C# passes every parameter: an optional one left out is zero, not the default UnrealScript would
give it (here: the world's origin, not the spawner's location).

## Lifetime

- A mod's classes go with the mod: when it unloads (or hot reloads), their actors are destroyed and the classes made
  abstract, so nothing spawns them any more. The engine keeps a loaded package for the session; the same source loaded
  again (after a hot reload) gives the same class back, spawnable again; an edited source is a new package with a new
  class of the same name.
- Objects a mod holds can be kept from the garbage collector with `mirage.keep(obj)` / `mirage.release(obj)`
  (`Mod.Keep`/`Release`); the mod's unload releases them. Every other object handle is weak: the API checks it is still
  alive on each use.
- Classes are loaded and objects spawned on the game thread (a tick, an event, a hook).

## How it is checked

Self-test steps "Mirage classes (.uc)", "(Lua)", "(C#)" and "(example mod)": each pad is spawned on the player in the
game, its Touch runs (UnrealScript, Lua or C#) and launches them; the described classes' new function returns the mod's
value, `native("double")` calls the mod's Lua native, and a hot reload destroys the class's actors and brings the
same class back. UnrealLib checks the compiler offline: every function and state of the game's packages decompiled and
compiled again (97.1% byte-identical) and every class (2,338) decompiled, built and decompiled again (93.5% identical;
the rest differ by native-only features or decompiler notation).
