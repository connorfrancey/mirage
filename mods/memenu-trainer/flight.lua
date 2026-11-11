-- Flight and No Clip: the same controls and movement, a different mode. No Clip moves the player straight through
-- everything (no collision test at all); Flight moves with the engine's collision, sliding along surfaces.
--
-- While one is flying, the trainer tools disarm and Save State keeps the player hanging (mirage.share "flight" = "1",
-- read by the menu as mirage.shared("memenu-trainer", "flight")).

local player = require("player")
local later = require("later")

local M = {}

local PHYS_None, PHYS_Falling = 0, 2

local state = {
    active = false,
    mode = nil,             -- "No Clip" or "Flight"
    vx = 0, vy = 0, vz = 0, -- Units per second
    last = nil,             -- The world's real time at the previous tick
    blocking = false,       -- The controller's bIgnoreButtonInput is ours ("Block jump/actions")
    prev_ignore = 0,        -- ... and what it was
}

local function set_active(on)
    state.active = on
    mirage.share("flight", on and "1" or "0")
end
set_active(false)

function M.active() return state.active end

-- Owned apart from `active`: the mode can drop out without a restore (no player for a moment, death) while the
-- controller still has our block set, and a restart must not take that 1 for the player's own value.
local function block_buttons(pc, block)
    if block and not state.blocking then
        state.prev_ignore = pc.bIgnoreButtonInput
        state.blocking = true
    elseif not block and state.blocking then
        pc.bIgnoreButtonInput = state.prev_ignore
        state.blocking = false
    end
    if block then pc.bIgnoreButtonInput = true end
end

-- Same cleanup the trainer does before flying: no slides or animations, a plain state
local function start(pawn, mode)
    mirage.call(pawn, "StopSlideEffect")
    mirage.call(pawn, "StopAllCustomAnimations", { BlendOutTime = 0 })
    mirage.call(pawn, "SetMove", { NewMove = player.MOVE_Walking })
    set_active(true)
    state.mode = mode
    state.vx, state.vy, state.vz = 0, 0, 0
    state.last = nil
    mirage.info(string.format("%s: started at (%.0f, %.0f, %.0f)", mode, pawn.Location.X, pawn.Location.Y, pawn.Location.Z))
end

-- Back to normal physics, keeping the speed: leaving flight feels continuous
local function restore(pawn, pc)
    if pc then block_buttons(pc, false) end
    pawn.Velocity = { X = state.vx, Y = state.vy, Z = state.vz }
    pawn.bCollideWorld = true
    pawn.EnterFallingHeight = -1e30     -- No fall damage for the distance flown
    pawn.Physics = PHYS_Falling
    mirage.info(string.format("%s: stopped at (%.0f, %.0f, %.0f)", state.mode, pawn.Location.X, pawn.Location.Y,
        pawn.Location.Z))
    set_active(false)
    state.vx, state.vy, state.vz = 0, 0, 0
end

local TWO_PI = 2 * math.pi
local function yaw_radians(u) return (u & 0xFFFF) * (TWO_PI / 65536) end
local function pitch_radians(u)
    local p = u & 0xFFFF
    if p > 32767 then p = p - 65536 end
    return p * (TWO_PI / 65536)
end

-- World direction from the input (forward/right: -1..1 axes, vertical -1..1), at most length 1. Unreal: X forward,
-- Y right, Z up. With follow_view forward goes where the camera looks, pitch included.
local function direction(yaw_u, pitch_u, forward, right, vertical, follow_view)
    local yaw = yaw_radians(yaw_u)
    local pitch = follow_view and pitch_radians(pitch_u) or 0
    local cy, sy, cp, sp = math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)
    local x = forward * cp * cy - right * sy
    local y = forward * cp * sy + right * cy
    local z = forward * sp + vertical
    local len = math.sqrt(x * x + y * y + z * z)
    if len > 1 then x, y, z = x / len, y / len, z / len end
    return x, y, z
end

-- Frame-rate independent approach of `current` to `target`
local function approach(current, target, responsiveness, dt)
    return current + (target - current) * (1 - math.exp(-responsiveness * dt))
end

-- s: the module's settings, read this tick
local function tick(mode, s)
    local pawn, pc = mirage.pawn(), mirage.controller()
    if pc and (not pawn or pawn.Health <= 0) then block_buttons(pc, false) end   -- Don't strand the block
    if not pawn or not pc then
        if state.active then set_active(false) end      -- Level change: restarts when a player is there
        return
    end
    if pawn.Health <= 0 then
        if state.active then
            mirage.info(state.mode .. ": player died, stopping")
            set_active(false)
        end
        return
    end
    if state.active and state.mode ~= mode then restore(pawn, pc) end      -- Switched between the two
    if not state.active then start(pawn, mode) end

    -- Real time between ticks (not the world's: game speed doesn't slow the flight), clamped against hitches
    local world = mirage.world()
    local now = world and world.RealTimeSeconds or 0
    local dt = state.last and math.max(0, math.min(now - state.last, 0.1)) or 0
    state.last = now

    local forward, right, vertical = 0, 0, 0
    if not mirage.menu_open() then          -- Menu open: hover, don't fly off
        local input = pc.PlayerInput
        if input then forward, right = input.RawJoyUp, input.RawJoyRight end
        vertical = (s.up and 1 or 0) - (s.down and 1 or 0)
    end
    local dx, dy, dz = direction(pc.Rotation.Yaw, pc.Rotation.Pitch, forward, right, vertical, s.follow_view)
    local speed = s.speed * (s.boost_held and s.boost or 1)
    state.vx = approach(state.vx, dx * speed, s.responsiveness, dt)
    state.vy = approach(state.vy, dy * speed, s.responsiveness, dt)
    state.vz = approach(state.vz, dz * speed, s.responsiveness, dt)

    -- The game's own physics must not fight it: no gravity, no leftover velocity
    pawn.Physics = PHYS_None
    pawn.Velocity = { X = 0, Y = 0, Z = 0 }
    block_buttons(pc, s.block_actions)
    if s.invulnerable then
        pawn.Health = pawn.MaxHealth
        pawn.EnterFallingHeight = -1e30
    end
    -- Hands snap to nearby surfaces: IK off so they don't stick to what we move through
    local left, right_ik = pawn.LeftHandWorldIKController, pawn.RightHandWorldIKController
    if left then left.StrengthTarget = 0 end
    if right_ik then right_ik.StrengthTarget = 0 end

    local mx, my, mz = state.vx * dt, state.vy * dt, state.vz * dt
    if mode == "No Clip" then
        pawn.bCollideWorld = false          -- Teleport-style: never tests collision
        local at = pawn.Location
        pawn.Location = { X = at.X + mx, Y = at.Y + my, Z = at.Z + mz }
    elseif mx ~= 0 or my ~= 0 or mz ~= 0 then
        pawn.bCollideWorld = true           -- The engine's move: collides and slides along surfaces
        mirage.call(pawn, "MoveSmooth", { Delta = { X = mx, Y = my, Z = mz } })
    end
end

local function stop(mode)
    if state.mode ~= mode then return end
    local pawn, pc = mirage.pawn(), mirage.controller()
    if state.active and pawn then restore(pawn, pc)
    else
        if pc then block_buttons(pc, false) end
        set_active(false)
    end
end

-- The two modules: `prefix` names their settings in mod.toml (no_clip_speed, flight_speed, ...)
local function fly_module(name, prefix, description, order, key)
    local s = {}
    for _, k in ipairs({ "speed", "boost", "responsiveness", "follow_view", "invulnerable", "block_actions", "up_key",
                         "down_key", "boost_key" }) do
        s[k] = prefix .. "_" .. k
    end
    local m
    m = mirage.module {
        name = name,
        tab = "Player",
        description = description,
        order = order,
        key = key,
        every_tick = true,
        on_enable = function()
            -- One at a time: the other one stops
            for _, other in ipairs(M.modules) do
                if other ~= m and other:enabled() then other:set_enabled(false) end
            end
        end,
        on_tick = function()
            if not m:enabled() then return end
            local set = s.settings
            tick(name, {
                speed = set.speed:get(), boost = set.boost:get(), responsiveness = set.responsiveness:get(),
                follow_view = set.follow_view:get(), invulnerable = set.invulnerable:get(),
                block_actions = set.block_actions:get(),
                up = set.up_key:down(), down = set.down_key:down(), boost_held = set.boost_key:down(),
            })
        end,
        on_disable = function() later.run(function() stop(name) end) end,
    }
    s.settings = {}
    for k, key_name in pairs(s) do
        if k ~= "settings" then s.settings[k] = mirage.setting(key_name) end
    end
    return m
end

M.modules = {}
M.modules[1] = fly_module("No Clip", "no_clip", "Fly through walls, floors and everything else", 20, 0x74)   -- F5
M.modules[2] = fly_module("Flight", "flight", "Fly freely but still collide with the world", 30, 0)

return M
