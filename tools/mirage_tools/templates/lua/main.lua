-- {{name}}: a Lua mod for Mirage. No build: `mirage mod install` copies this folder into the game's mods folder, and
-- while the game runs, saving this file reloads the mod. The API: docs/api-reference.md (mirage.* mirrors api.h).

local m = mirage.module {
    name = "{{name}}",
    tab = "Mods",
    description = "{{description}}",

    on_enable = function()
        mirage.log("on")
    end,

    on_disable = function()
        mirage.log("off")       -- Undo here whatever on_enable and on_tick changed in the game
    end,
}

-- Declared in mod.toml (looked up once the module is made: its settings are the module's)
local Speed = mirage.setting("speed")
-- Made in code: a counter the panel shows
local Ticks = m:slider_int("Ticks", 0, 0, 1 << 30)

-- The game thread, every tick while the module is on
mirage.on("tick", function()
    if not m:enabled() then return end
    Ticks:set(Ticks:get() + 1)
    local pawn = mirage.pawn()
    if pawn then
        -- Read and write the game's objects by property path:  pawn.Location.Z, pawn.GroundSpeed = ...
        local _ = pawn.Location.Z * Speed:get()
    end
end)

-- A test the game runs with `mirage mod test`: it passes unless it errors or returns false (and why)
mirage.test("ticks while on", function()
    if Ticks:get() == 0 then return false, "no tick was counted with the module on" end
end)

-- More: mirage.hook("Engine.Actor.TakeDamage", ...) runs your code when a script function runs; mirage.actors{...}
-- finds actors; mirage.transaction(...) makes changes the runtime puts back when the mod goes.
