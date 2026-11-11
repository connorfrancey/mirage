-- The trainer's one-shot tools and Auto Kickglitch.
--
--   Kickglitch        armed: during a wallrun kicks off the wall, and jumps the moment the player lands
--   Beamer            armed while wall climbing: turns around (or sidesteps) at full climb speed
--   Strang            armed: jumps on the exact frame the next move starts (in the engine's move state handler)
--   Auto Kickglitch   always on: every wallrun kick gets its jump on the landing frame; with the kick buffer, a kick
--                     pressed just before the wall waits for it
--
-- A tool switches itself off once it has done its move, and is disarmed when the player dies, the level changes or
-- Flight / No Clip is on. Auto Kickglitch shares what its buffer did (mirage.share "kick_buffer.*") for the tests.

local flight = require("flight")
local later = require("later")

local PHYS_Walking, PHYS_Falling, PHYS_WallRunning = 1, 2, 12
local MOVE = {
    Falling = 2, Grabbing = 3, WallRunningRight = 4, WallRunningLeft = 5, WallClimbing = 6, Jump = 11,
    WallRunJump = 12, IntoGrab = 14, ZipLine = 28, MeleeWallrun = 62, FallingUncontrolled = 72,
}

local function jump(pc) mirage.call(pc.PlayerInput, "Jump") end
local function kick(pc)
    mirage.call(pc, "AttackPress")
    mirage.call(pc, "AttackRelease")
end

-- A tool: `step(pawn, pc)` runs every tick while armed and returns true when done
local function tool(def)
    local m
    def.tab = "Trainer"
    def.every_tick = true           -- Fires on an exact frame: never deferred
    local step = def.step
    def.step = nil
    def.on_tick = function()
        if not m:enabled() then return end
        local pawn, pc = mirage.pawn(), mirage.controller()
        if not pawn or not pc or not pc.PlayerInput or pawn.Health <= 0 or flight.active() then
            m:set_enabled(false)
            return
        end
        if step and step(pawn, pc) then m:set_enabled(false) end
    end
    m = mirage.module(def)
    return m
end

-- ---- Kickglitch -----------------------------------------------------------------------------------------------------
tool {
    name = "Kickglitch",
    description = "Arm, then wallrun: kicks off the wall and jumps the moment you land",
    order = 10,
    step = function(pawn, pc)
        if pawn.Physics == PHYS_Walking then
            jump(pc)
            return true
        end
        if pawn.Physics == PHYS_WallRunning then kick(pc) end
        return false
    end,
}

-- ---- Beamer ---------------------------------------------------------------------------------------------------------
-- The automatic sidestep holds a strafe key through the climb, like the trainer (a real key event: the game reads it
-- like the player's own).
local beamer_started, beamer_key = false, nil
local function beamer_release()
    if beamer_key then
        mirage.input_key(beamer_key, false)
        beamer_key = nil
    end
end
tool {
    name = "Beamer",
    description = "Arm while wall climbing: turns around (or sidesteps) at full climb speed",
    order = 20,
    on_enable = function() beamer_started = false end,
    on_panel = function()       -- Only what the chosen beamer uses
        local sidestep = BeamerSidestep:get()
        BeamerAuto:set_visible(sidestep)
        BeamerDirection:set_visible(sidestep and BeamerAuto:get())
    end,
    on_disable = beamer_release,        -- A key event is fine from any thread
    step = function(pawn, pc)
        if pawn.MovementState ~= MOVE.WallClimbing then
            if not beamer_started then mirage.notify("Beamer: not wall climbing") end
            return true
        end
        local sidestep = BeamerSidestep:get()
        if not beamer_started then
            beamer_started = true
            if sidestep and BeamerAuto:get() then
                local vk = BeamerDirection:get() == "Left" and BeamerLeft:get() or BeamerRight:get()
                if vk ~= 0 then
                    mirage.input_key(vk, true)
                    beamer_key = vk
                end
            end
        end
        local vx, vy = pawn.Velocity.X, pawn.Velocity.Y
        if math.sqrt(vx * vx + vy * vy) <= 1000 then return false end
        if sidestep then jump(pc) else mirage.call(pc, "LookBehind") end
        return true
    end,
}
BeamerSidestep = mirage.setting("beamer_sidestep")
BeamerAuto = mirage.setting("beamer_auto")
BeamerDirection = mirage.setting("beamer_direction")
BeamerLeft = mirage.setting("beamer_left_key")
BeamerRight = mirage.setting("beamer_right_key")

-- ---- Strang ---------------------------------------------------------------------------------------------------------
-- Waits for the engine's move state handler: jumps instead of letting it handle the new state
local strang = tool {
    name = "Strang",
    description = "Arm: jumps on the exact frame your next move starts",
    order = 30,
}
mirage.on("player_state", function()
    if not strang:enabled() then return false end
    local pc = mirage.controller()
    if pc and pc.PlayerInput then jump(pc) end
    strang:set_enabled(false)
    return true
end)

-- ---- Auto Kickglitch ------------------------------------------------------------------------------------------------
-- The kick buffer: a kick pressed in the air is held back (AttackPress never reaches the game, nor its release). When
-- the player is wallrunning within the window it is pressed for them on the first tick they can kick; otherwise it is
-- let go late, or dropped.
local WALL_KICK_TRIES = 8           -- Ticks of wallrunning to get the kick in (the wallrun may not take it at once)
local buffer = { on = false, injecting = false, held = false, since = 0, wall_ticks = 0 }
local stats = { held = 0, on_wall = 0, late = 0, dropped = 0 }
local function count(what)
    stats[what] = stats[what] + 1
    mirage.share("kick_buffer." .. what, stats[what])
end
for k, v in pairs(stats) do mirage.share("kick_buffer." .. k, v) end

local function game_time()
    local world = mirage.world()
    return world and world.TimeSeconds or 0     -- A TAS's clock: replays the same
end

local hooked = pcall(function()
    mirage.hook("TdGame.TdPlayerController.AttackPress", function(call)
        if not buffer.on or buffer.injecting or call.object ~= mirage.controller() then return end
        local pawn = mirage.pawn()
        if not pawn or pawn.Health <= 0 or pawn.Weapon then return end     -- With a gun the press shoots
        if pawn.Physics ~= PHYS_Falling then return end                     -- On the ground or a wall: kick now
        call:skip()
        if buffer.held then return end          -- Pressed again while waiting: still the one kick
        buffer.held, buffer.since, buffer.wall_ticks = true, game_time(), 0
        count("held")
    end)
    mirage.hook("TdGame.TdPlayerController.AttackRelease", function(call)
        -- The press was held back: so is its release (it goes with the kick)
        if buffer.on and not buffer.injecting and buffer.held and call.object == mirage.controller() then call:skip() end
    end)
end)
mirage.share("kick_buffer.hooked", hooked and "1" or "0")
if not hooked then mirage.warn("Auto Kickglitch: the kick can't be held back (no kick buffer)") end

local function buffered_kick(pc)
    buffer.injecting = true
    kick(pc)
    buffer.injecting = false
end
local function drop()
    buffer.held = false
    count("dropped")
end

local function tick_buffer(pc, move, physics)
    if not buffer.held then return end
    if move == MOVE.MeleeWallrun then           -- Kicking off the wall: done
        buffer.held = false
        count("on_wall")
        return
    end
    if physics == PHYS_WallRunning then
        if buffer.wall_ticks < WALL_KICK_TRIES then
            buffer.wall_ticks = buffer.wall_ticks + 1
            buffered_kick(pc)                   -- Until the wallrun takes it
        else
            drop()
        end
        return
    end
    local expired = game_time() - buffer.since > AkgWindow:get() / 1000
    if physics == PHYS_Falling and not expired then return end      -- Still on its way to the wall
    if physics ~= PHYS_Falling or AkgOnMiss:get() == "Drop it" then
        drop()                                  -- Landed, grabbed a ledge, ...: not wanted any more
        return
    end
    buffer.held = false                         -- No wall came: the air kick it would have been, late
    buffered_kick(pc)
    count("late")
end

local OTHER_MOVES = {
    [MOVE.Grabbing] = true, [MOVE.WallRunningLeft] = true, [MOVE.WallRunningRight] = true, [MOVE.WallClimbing] = true,
    [MOVE.IntoGrab] = true, [MOVE.ZipLine] = true, [MOVE.FallingUncontrolled] = true, [MOVE.Jump] = true,
    [MOVE.WallRunJump] = true,
}

local kicked, kick_ticks = false, 0
local akg
akg = mirage.module {
    name = "Auto Kickglitch",
    tab = "Trainer",
    description = "Jumps on the landing frame after every wallrun kick you do",
    order = 12,
    every_tick = true,          -- The landing frame is one tick
    on_panel = function()
        AkgWindow:set_visible(AkgBuffer:get())
        AkgOnMiss:set_visible(AkgBuffer:get())
    end,
    on_tick = function()
        if not akg:enabled() then return end
        buffer.on = AkgBuffer:get() and hooked
        local pawn, pc = mirage.pawn(), mirage.controller()
        if not pawn or not pc or not pc.PlayerInput or pawn.Health <= 0 or flight.active() then
            kicked, buffer.held = false, false
            return
        end
        local move, physics = pawn.MovementState, pawn.Physics
        if physics == PHYS_WallRunning and AkgKickKey:get() ~= 0 and AkgKickKey:down() and not kicked then
            buffered_kick(pc)
        end
        tick_buffer(pc, move, physics)
        if move == MOVE.MeleeWallrun and not kicked then
            kicked, kick_ticks = true, 0
        end
        if not kicked then return end
        if physics == PHYS_Walking then
            jump(pc)
            kicked = false
        else
            kick_ticks = kick_ticks + 1
            if kick_ticks > 240 or OTHER_MOVES[move] then kicked = false end  -- No landing to jump from
        end
    end,
    on_disable = function()
        later.run(function()
            kicked = false
            buffer.on, buffer.held = false, false
        end)
    end,
}
AkgKickKey = mirage.setting("akg_kick_key")
AkgBuffer = mirage.setting("akg_buffer")
AkgWindow = mirage.setting("akg_window")
AkgOnMiss = mirage.setting("akg_on_miss")

return {}
