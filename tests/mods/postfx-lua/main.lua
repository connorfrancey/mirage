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
