-- Trainer: features the menu had as native code, now a mod using only the public API (and the example to copy).
--
--   God Mode            the controller's bGodMode, full health and no fall deaths every tick
--   Teleport to Spawn   back to the start spot the game itself picks (GameInfo.FindPlayerStart), facing its way
--   Game Speed          the world's TimeDilation and the game's GameSpeed
--   Kill Bots           every enemy (TdBotPawn) in the level, or near the player, dies as if hit
--   Freeze Timer        a time trial's clock stops (its start stamps move on with the world time)
--   No Clip, Flight     flight.lua
--   Kickglitch, Beamer, Strang, Auto Kickglitch   tools.lua

local player = require("player")
local later = require("later")     -- Game-thread work from on_trigger / on_disable
require("flight")                  -- No Clip, Flight (flight.lua)
require("tools")                   -- Kickglitch, Beamer, Strang, Auto Kickglitch (tools.lua)

-- ---- God Mode ------------------------------------------------------------------------------------------------------
local god
god = mirage.module {
    name = "God Mode",
    tab = "Player",
    description = "Take no damage",
    order = 10,

    on_enable = function()
        -- Switched on during a death fall, it catches you (like the trainer's God tool)
        later.run(function()
            local pawn, pc = mirage.pawn(), mirage.controller()
            if pawn and pc and pawn.Health > 0 and pawn.MovementState == player.MOVE_FallingUncontrolled then
                player.reset_movement(pawn, pc)
            end
        end)
    end,

    on_tick = function()
        local pawn, pc = mirage.pawn(), mirage.controller()
        if not pawn or not pc or not god:enabled() then return end   -- A tick still under way when switched off
        pc.bGodMode = true                  -- The engine's own "God" flag
        pawn.Health = pawn.MaxHealth
        pawn.EnterFallingHeight = -1e30     -- No fall deaths
    end,

    on_disable = function()
        later.run(function()
            local pc = mirage.controller()
            if pc then pc.bGodMode = false end
        end)
    end,
}

-- ---- Teleport to Spawn ------------------------------------------------------------------------------------------------
mirage.module {
    name = "Teleport to Spawn",
    tab = "Player",
    description = "Move the player back to the level start",
    order = 40,
    action = true,

    on_trigger = function()
        later.run(function()
            local pawn, pc, world = mirage.pawn(), mirage.controller(), mirage.world()
            local game = world and world.Game
            if not pawn or not pc or not game then
                mirage.notify("Teleport to Spawn: no player")
                return
            end
            local start = mirage.call(game, "FindPlayerStart", { Player = pc, InTeam = 0 })   -- IncomingName stays ""
            if type(start) ~= "userdata" then
                mirage.notify("Teleport to Spawn: this level has no start spot")
                return
            end
            player.reset_movement(pawn, pc)     -- Out of any move (wallrun, grab, ...) first
            pawn.Velocity.X, pawn.Velocity.Y, pawn.Velocity.Z = 0, 0, 0
            pawn.Acceleration.X, pawn.Acceleration.Y, pawn.Acceleration.Z = 0, 0, 0
            pawn.Location.X, pawn.Location.Y, pawn.Location.Z = start.Location.X, start.Location.Y, start.Location.Z
            pc.Rotation.Yaw = start.Rotation.Yaw    -- The view, and the body with it
            pawn.Rotation.Yaw = start.Rotation.Yaw
            mirage.log(string.format("at %s (%.0f, %.0f, %.0f)", mirage.name(start), start.Location.X, start.Location.Y,
                start.Location.Z))
            mirage.notify("Teleported to the level start")
        end)
    end,
}

-- ---- Game Speed -------------------------------------------------------------------------------------------------------
local function set_speed(speed)
    local world = mirage.world()
    if not world then return end
    world.TimeDilation = speed
    if world.Game then world.Game.GameSpeed = speed end
end

local speed
speed = mirage.module {
    name = "Game Speed",
    tab = "World",
    description = "Change the world time dilation",
    order = 10,
    on_tick = function()
        if speed:enabled() then set_speed(Speed:get()) end    -- A tick still under way when switched off: nothing
    end,
    on_disable = function() later.run(function() set_speed(1.0) end) end,
}
Speed = mirage.setting("game_speed")

-- ---- Kill Bots --------------------------------------------------------------------------------------------------------
-- They die as if hit (a lot), so the game's AI and level scripts see a normal death; a bot the level made
-- invulnerable loses that first, and one still alive after the damage is killed outright.
local MELEE = "Class TdGame.TdDmgType_Melee"

local function kill_all(range_m)
    local world, me, pc = mirage.world(), mirage.pawn(), mirage.controller()
    if not world then return 0 end
    local melee = mirage.find(MELEE)
    -- Collected first: a death can take a pawn off the list while it is walked
    local bots = {}
    local p = world.PawnList
    while p do
        if p ~= me and mirage.is_a(p, "TdBotPawn") and (p.Health or 0) > 0 and not p.bDeleteMe then
            local near = true
            if range_m > 0 and me then
                local dx, dy, dz = p.Location.X - me.Location.X, p.Location.Y - me.Location.Y, p.Location.Z - me.Location.Z
                near = math.sqrt(dx * dx + dy * dy + dz * dz) <= range_m * 100     -- 100 units = 1 m
            end
            if near then bots[#bots + 1] = p end
        end
        p = p.NextPawn
    end
    local killed = 0
    for _, bot in ipairs(bots) do
        bot.bAIGodMode = false
        mirage.call(bot, "TakeDamage", { DamageAmount = bot.Health + 10000, EventInstigator = pc, DamageType = melee,
            DamageCauser = me })
        local health = bot.Health      -- nil once it is gone
        local dead = health == nil or health <= 0 or bot.bDeleteMe
        if not dead then dead = mirage.call(bot, "Died", { Killer = pc, DamageType = melee }) == true end
        if dead then killed = killed + 1 end
    end
    mirage.log(string.format("%d of %d killed", killed, #bots))
    return killed
end

mirage.module {
    name = "Kill Bots",
    tab = "World",
    description = "Kill every enemy in the level, or the ones near you",
    order = 30,
    action = true,
    on_trigger = function()
        later.run(function()
            local range = Range:get()
            local n = kill_all(range)
            if n == 0 then mirage.notify("Kill Bots: no enemies" .. (range > 0 and " in range" or ""))
            else mirage.notify("Killed " .. n .. (n == 1 and " enemy" or " enemies")) end
        end)
    end,
}
Range = mirage.setting("kill_range")

-- ---- Freeze Timer -----------------------------------------------------------------------------------------------------
-- The race time is the world time minus the race's start stamp (and the split times count from the last checkpoint's
-- stamp): while frozen both stamps move forward by the world time that passed. Every tick, or the clock creeps on.
local last = -1
local freeze
freeze = mirage.module {
    name = "Freeze Timer",
    tab = "World",
    description = "Stop the time trial clock (only time trials)",
    order = 20,
    on_tick = function()
        if not freeze:enabled() then return end
        local world = mirage.world()
        local game = world and world.Game
        if not game or not mirage.is_a(game, "TdSPTimeTrialGame") then
            last = -1
            return
        end
        local now = world.TimeSeconds
        local running = game.RaceStartTimeStamp > 0 and game.RaceEndTimeStamp <= 0
        if running and last >= 0 and now > last then
            local passed = now - last
            game.RaceStartTimeStamp = game.RaceStartTimeStamp + passed
            if game.LastCheckpointTimeStamp > 0 then game.LastCheckpointTimeStamp = game.LastCheckpointTimeStamp + passed end
        end
        last = now
    end,
    on_disable = function() later.run(function() last = -1 end) end,
}
