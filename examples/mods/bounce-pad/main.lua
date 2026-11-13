-- Bounce Pad: switching the module on drops a MirageBouncePad (classes/MirageBouncePad.uc) where the player stands;
-- walk off and back on, or jump on it. Every switch-on drops another one. Classes are loaded and actors spawned on the
-- game thread: in a tick.
local pad

local m = mirage.module {
    name = "Bounce Pad", tab = "Mods",
    description = "Drops a bounce pad (a class written in UnrealScript) where you stand",
}
Dropped = m:slider_int("Dropped", 0, 0, 1000)

mirage.on("tick", function()
    if not m:enabled() then return end
    m:set_enabled(false)
    -- The class: compiled the first time (a few seconds while the game's packages are read), then from the cache
    if not pad then
        local why
        pad, why = mirage.load_class("classes/MirageBouncePad.uc")
        if not pad then
            mirage.error("the bounce pad didn't compile: " .. why)
            return
        end
    end
    local pawn = mirage.pawn()
    if pawn then
        -- Where the player stands. (A call from Lua passes every parameter: an optional one left out is zero, not the
        -- default UnrealScript would give it, so the location is given.)
        local at = pawn.Location
        mirage.call(pawn, "Spawn", {
            SpawnClass = pad, bNoCollisionFail = true,
            ["SpawnLocation.X"] = at.X, ["SpawnLocation.Y"] = at.Y, ["SpawnLocation.Z"] = at.Z,
        })
        Dropped:set(Dropped:get() + 1)
    end
end)
