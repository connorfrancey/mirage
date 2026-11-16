-- Self-test mod (step 7, Scenario F). With its module on it puts, 300 units in front of the player:
--   * a trigger for pawns (enter, stay and leave counted): entering it pushes a prop up (the mod's own logic acting on
--     an interactive object), the prop sitting beside it;
--   * a trigger at the same spot for KActors only: the player must not set it off;
--   * 450 units further, a moving platform with a trigger over it: stepping on sends it up, carrying the player.
-- The self-test walks the player in and out ("Mirage triggers") and reads what is shared. Off: mirage.revert() takes the
-- triggers and the prop away.
local m = mirage.module { name = "Triggers (Lua)", tab = "Mods", description = "Self-test: triggers from Lua (step 7)" }

local made, counts = false, { enter = 0, stay = 0, leave = 0, filtered = 0, launched = 0 }
local prop, mark_t, platform

local function share(k, v) mirage.share(k, tostring(v)) end

local function make()
    local at = mirage.body(mirage.pawn()).location
    local spot = { x = at.x + 300, y = at.y, z = at.z }
    share("spot", string.format("%.1f %.1f %.1f", spot.x, spot.y, spot.z))
    local mesh = mirage.shared("selftest", "mesh")
    if mesh ~= "" then prop = mirage.spawn_prop(mesh, { x = spot.x, y = spot.y + 150, z = spot.z }) end
    local t = mirage.trigger {
        location = spot, radius = 80, height = 100, class = "Pawn", stay = true,
        on_enter = function(trigger, other)
            counts.enter = counts.enter + 1
            share("enter", counts.enter)
            share("who", mirage.name(other))
            if prop then
                mirage.impulse(prop, { x = 0, y = 0, z = 400 }, nil, true)     -- The mod's logic: the prop jumps
                counts.launched = counts.launched + 1
                share("launched", counts.launched)
            end
        end,
        on_stay = function() counts.stay = counts.stay + 1 share("stay", counts.stay) end,
        on_leave = function() counts.leave = counts.leave + 1 share("leave", counts.leave) end,
    }
    local f = mirage.trigger {
        location = spot, radius = 80, height = 100, class = "KActor",
        on_enter = function(trigger, other)
            if other ~= prop then counts.filtered = counts.filtered + 1 share("filtered", counts.filtered) end
        end,
    }
    -- Scenario F: a platform (the prop's mesh, flattened and widened) further on, with a trigger over it; the player
    -- stepping on sends it up 300 units in 1.5 s, carrying the player
    if mesh ~= "" then
        local p = { x = spot.x + 450, y = spot.y, z = spot.z + 20 }
        platform = mirage.mover { mesh = mesh, location = p, scale = { 4, 4, 0.5 } }
        share("platform", platform and mirage.name(platform) or "")
        if platform then
            mirage.trigger {
                location = { x = p.x, y = p.y, z = p.z + 120 }, radius = 120, height = 100, class = "Pawn",
                on_enter = function()
                    counts.rides = (counts.rides or 0) + 1
                    share("rides", counts.rides)
                    share("sent", mirage.move_to(platform, { x = p.x, y = p.y, z = p.z + 300 }, 1.5))
                end,
            }
        end
    end
    local box = mirage.trigger { shape = "box", location = spot }       -- Refused: only cylinders
    share("made", (t and 1 or 0) + (f and 1 or 0))
    share("box_refused", box == nil)
    share("trigger", t and mirage.name(t) or "")
    mark_t = t
end

mirage.on("tick", function()
    if m:enabled() and not made then
        made = true
        make()
    elseif mark_t then
        share("inside", mirage.trigger_count(mark_t))
    end
    if not m:enabled() and made then
        made = false
        share("reverted", mirage.revert())
        mark_t, prop, platform = nil, nil, nil
        counts.rides = 0
    end
end)
