-- Self-test mod ("Mirage post-processing"): mirage.postfx and friends, driven by the self-test.
local tint, broken
function tint_on(r, g, b, a)
    tint = tint or mirage.postfx("shaders/tint.hlsl")
    mirage.postfx_set(tint, 1, r, g, b, a)
    return mirage.postfx_enable(tint, true)
end
function tint_off() if tint then mirage.postfx_enable(tint, false) end end
function broken_error()
    broken = broken or mirage.postfx("shaders/broken.hlsl")
    mirage.postfx_enable(broken, true)
    return mirage.postfx_error(broken)
end
function broken_off() if broken then mirage.postfx_enable(broken, false) end end

-- Step 31's milestone: "Tint strength" on the game's own pause menu (Pause > Options > Mod settings) blends the frame
-- toward green by the slider's value, 0 (off) to 1
local strength = mirage.menu_slider { label = "Tint strength", min = 0, max = 1, value = 0, step = 0.1 }
local menu_tint
mirage.on("tick", function()
    local v = mirage.menu_slider_value(strength) or 0
    if v > 0 then
        menu_tint = menu_tint or mirage.postfx("shaders/tint.hlsl")
        mirage.postfx_set(menu_tint, 1, 0, 1, 0, v)
        mirage.postfx_enable(menu_tint, true)
    elseif menu_tint then
        mirage.postfx_enable(menu_tint, false)
    end
end)
function menu_strength() return mirage.menu_slider_value(strength) end
