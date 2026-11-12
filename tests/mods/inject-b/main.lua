-- Self-test mod (step 11, script injection): injects at the same place as inject-a (before the GetMobilityMultiplier
-- statement of TdGame.TdMove_Jump.StartMove) at priority 0, so it runs second, and records the local
-- CurrentWeaponType it sees there: 1 when inject-a ran first, the real weapon type otherwise.
local FUNCTION = "TdGame.TdMove_Jump.StartMove"
local injection

local m = mirage.module {
    name = "Inject B (Lua)", tab = "Mods", description = "Self-test: injects into StartMove after Inject A, records the local",
}
Seen = m:slider_int("Seen", -1, -1, 255)

mirage.on("tick", function()
    local want = m:enabled()
    if want and not injection then
        injection = mirage.inject(FUNCTION, "before:GetMobilityMultiplier", function(call)
            Seen:set(call.params.CurrentWeaponType)
        end, { priority = 0 })
    elseif not want and injection then
        injection:remove()
        injection = nil
    end
end)
