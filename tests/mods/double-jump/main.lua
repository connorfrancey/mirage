local m = mirage.module { name = "Double Jump (test)", tab = "Mods", description = "Self-test: a double jump (step 8)" }

local MOVE_WALKING, MOVE_FALLING, MOVE_JUMP, MOVE_LANDING = 1, 2, 11, 20
local move, used = nil, false
local n = { starts = 0, ticks = 0, stops = 0, actions = 0, refused = 0 }

local function count(k)
    n[k] = n[k] + 1
    mirage.share(k, tostring(n[k]))
end

mirage.on("tick", function()
    if m:enabled() and not move then
        move = mirage.move {
            name = "DoubleJump", as = "TdMove_Falling",
            -- Its animation: the springboard's take-off, blended in and out on the full-body slot
            anim = "SpringBoardLeftLeg", anim_slot = "fullbody", blend_in = 0.1, blend_out = 0.25,
            on_start = function()
                local p = mirage.pawn()
                if p then
                    p.Velocity.Z = 650                  -- The move's physics: a second jump's speed
                    local a = mirage.anim_state(p, "fullbody")
                    mirage.share("anim", a and a.playing and a.sequence or "")
                end
                count("starts")
            end,
            on_tick = function() count("ticks") end,
            on_stop = function() count("stops") end,
        }
        mirage.share("move", tostring(move or 0))
    elseif not m:enabled() and move then
        move, used = nil, false
        mirage.share("reverted", tostring(mirage.revert()))
    end
end)

-- On the ground again: the double jump is back
mirage.on("player_state", function(now)
    if now == MOVE_WALKING or now == MOVE_LANDING then used = false end
end)

mirage.on("action", function(name, pressed)
    if name ~= "double_jump" or not pressed then return end
    count("actions")
    if not move or used then return end
    local p = mirage.pawn()
    if not p then return end
    local state = p.MovementState
    if state ~= MOVE_JUMP and state ~= MOVE_FALLING then
        count("refused")                                -- Not in the air: the game's own jump does its work
        return
    end
    used = true
    mirage.share("started", tostring(mirage.move_start(move)))
end)
