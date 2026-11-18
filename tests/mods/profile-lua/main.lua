-- Self-test mod (step 27): every tick, as many loop rounds as the self-test shares as "selftest" "busy" (0: none). The
-- self-test reads the profiler (runtime/mod_profile.h) for this mod's "tick event".
local sum = 0
mirage.on("tick", function()
    local n = tonumber(mirage.shared("selftest", "busy")) or 0
    for i = 1, n do sum = (sum + i) % 7919 end
    mirage.share("rounds", n)
end)
