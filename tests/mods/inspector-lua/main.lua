-- Self-test mod (step 16): with its module on, a transaction doubles the player's JumpZ; off, it puts it back. The
-- Object Inspector shows the change and the revert like any other.
local m = mirage.module { name = "Inspector (Lua)", tab = "Mods", description = "Self-test: a change the Object Inspector shows" }
local id = nil

mirage.on("tick", function()
    local pawn = mirage.pawn()
    if not pawn then return end
    if m:enabled() and not id then
        id = mirage.transaction("double JumpZ", function() pawn.JumpZ = pawn.JumpZ * 2 end)
        mirage.share("applied", id)
    elseif not m:enabled() and id then
        mirage.revert(id)
        id = nil
        mirage.share("reverted", 1)
    end
end)
