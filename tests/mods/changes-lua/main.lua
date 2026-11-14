-- Self-test mod (step 15): with its module on, one transaction halves the level's gravity and lowers its KillZ, and one
-- write outside any transaction moves its StallZ. Off, it puts its transactions back (mirage.revert); the write outside
-- stays. Unloading (a hot reload) puts them back by itself. What it did is shared for the self-test.
local m = mirage.module { name = "Changes (Lua)", tab = "Mods", description = "Self-test: reversible changes (step 15)" }
local applied = false

mirage.on("tick", function()
    local world = mirage.world()
    if not world then return end
    if m:enabled() and not applied then
        applied = true
        local id = mirage.transaction("low gravity", function()
            world.WorldGravityZ = world.WorldGravityZ * 0.5
            world.WorldGravityZ = world.WorldGravityZ * 1.0   -- Written again: the first old value is the one kept
            world.KillZ = world.KillZ - 1000
        end)
        world.StallZ = world.StallZ + 1                         -- Outside: not recorded, never put back
        local list = mirage.changes()
        mirage.share("id", id)
        mirage.share("transactions", #list)
        mirage.share("values", #list > 0 and #list[#list].values or 0)
    elseif not m:enabled() and applied then
        applied = false
        mirage.share("reverted", mirage.revert())
    end
end)
