-- Self-test mod (step 11, script injection): while its module is on, it injects into the middle of
-- TdGame.TdMove_Jump.StartMove, before the statement
--
--     WantedJumpVelocity.Z = (CurrentWeaponType == 1 ? BaseJumpZHeavy : BaseJumpZ) * PawnOwner.GetMobilityMultiplier();
--
-- and sets the local CurrentWeaponType to 1 (heavy): the branch takes BaseJumpZHeavy. Priority 10: inject-b's injection
-- at the same place runs after it and sees the 1. Injections are made and removed on the game thread (a tick).
local FUNCTION = "TdGame.TdMove_Jump.StartMove"
local injection

local m = mirage.module {
    name = "Inject A (Lua)", tab = "Mods", description = "Self-test: injects into StartMove, makes the jump 'heavy'",
}
Runs = m:slider_int("Runs", 0, 0, 1 << 30)

mirage.on("tick", function()
    local want = m:enabled()
    if want and not injection then
        injection = mirage.inject(FUNCTION, "before:GetMobilityMultiplier", function(call)
            call.params.CurrentWeaponType = 1
            Runs:set(Runs:get() + 1)
        end, { priority = 10 })
    elseif not want and injection then
        injection:remove()
        injection = nil
    end
end)
