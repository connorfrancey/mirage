-- Grappling Hook: a first-party Mirage mod, all Lua, no build. It ships inside the menu's DLL and is written to
-- Binaries\mirage\mods\mirage-grapple at startup: to change it, copy the folder under another id.
--
-- Press the fire key (middle mouse) while aiming at a surface in range: the hook catches, a rope is drawn in the world,
-- and you zip straight to that point (Zip mode, the default), or, holding the key, swing on it (Swing mode) or are
-- reeled in (Pull mode). Z and X shorten and lengthen the rope; letting go of
-- fire releases with a small boost. Everything below is the public mod API (docs/AI-GUIDE.md, runtime/mod_lua.h):
--
--   mirage.module          a switch and settings panel in the menu (settings declared in mod.toml)
--   [actions] in mod.toml  rebindable keys, reported by mirage.on("action")
--   mirage.camera/trace    aiming: the player's view and a ray into the world
--   mirage.move            a move of the mod's own, running as the game's TdMove_Falling, for the launch: the game still
--                          lands, grabs ledges and wall runs
--   mirage.on("tick")      the rope's physics every engine tick while hooked (rope.lua), on the ground or in the air
--   pawn.Location/Velocity the player, read and written by property name
--   mirage.draw3d          the rope and the anchor as lines in the world, inside the game's frame
--   mirage.draw            the HUD: a reticle that shows the available target, mode, and cable tension
--   mirage.on(...)         level loads, deaths and checkpoint restarts let go of the rope
--   mirage.share           what it is doing, for other mods and the self-test
--   globals                grapple_at({x, y, z}), grapple_release() and grapple_status() for the console (Misc >
--                          Console, mod "mirage-grapple") and tests
-- Its cost per tick is in the mod manager (the profiler), like any mod's.
local vec = require("vec")
local rope = require("rope")
local view = require("view")

local MOVE_WALKING = 1

local Mode, Range, ReelSpeed, Swing, Yank, Boost, ZipSpeed, Volume  -- mod.toml settings
local Status, Hooks                                        -- made in code below
local move                                                 -- the mod's move (its EMovement number)

local hook = nil          -- While hooked: {anchor = vec, length = number, taut = bool}
local aim = nil           -- This tick's aim: {hit = vec or nil, distance}
local reel = { into = false, out = false }
local m, feedback, sound
local disabled = false
local blocked_damage = 0

local function share(k, v) mirage.share(k, tostring(v)) end

local function play(cue)
    if sound then mirage.sound_stop(sound) end
    local volume = Volume:get()
    sound = volume > 0 and mirage.sound_play("sounds/" .. cue .. ".wav", { volume = volume }) or nil
end

local function config()
    return {
        range = Range:get(), reel_speed = ReelSpeed:get(), swing = Swing:get(),
        min_length = 120, drag = 0.08,
    }
end

-- ---- Hooking and letting go ------------------------------------------------------------------------------------------
local function release(why, boost)
    if not hook then return end
    local pawn = hook.pawn
    hook = nil
    mirage.draw3d.clear()
    local p = mirage.pawn()
    if p and p == pawn and boost and boost > 0 then
        local v = vec.of(p.Velocity)
        p.Velocity = vec.engine(vec.add(v, vec.new(0, 0, boost)))
    end
    if why == "let go" or why == "arrived" then play("release") end
    Status:set("free (" .. why .. ")")
    share("state", "free")
    share("released", why)
end

local function attach(anchor, normal)
    local p = mirage.pawn()
    if not p or not move or not m:enabled() then return false end
    local pos = vec.of(p.Location)
    local toward, dist = vec.norm(vec.sub(anchor, pos))
    if dist > Range:get() or dist < 50 then return false end
    local cylinder = p.CylinderComponent
    local radius = cylinder and cylinder.CollisionRadius or 34
    local height = cylinder and cylinder.CollisionHeight or 90
    hook = {
        anchor = anchor, normal = normal, pawn = p, length = dist, distance = dist,
        age = 0, reel = 0, taut = false, blocked = 0,
        extent = vec.new(math.max(1, radius - 2), math.max(1, radius - 2), math.max(1, height - 3)),
    }
    -- The catch: a yank toward the anchor, and off the ground if standing (the falling move takes it from there)
    local v = vec.add(vec.of(p.Velocity), vec.scale(toward, Yank:get()))
    if p.MovementState == MOVE_WALKING then
        v.z = math.max(v.z, 320)
        local rise = vec.add(pos, vec.new(0, 0, 12))
        if not mirage.trace(pos, rise, { extent = hook.extent, ignore = p }) then
            p.Location = vec.engine(rise)
        end
    end
    p.Velocity = vec.engine(v)
    mirage.move_start(move)
    feedback = { time = 0.2 }
    play("catch")
    Hooks:set(Hooks:get() + 1)
    Status:set(string.format("hooked at %.0f units", dist))
    share("state", "hooked")
    share("hooks", Hooks:get())
    return true
end

-- Zip aims velocity toward the anchor and eases down on approach. Arrival keeps a little forward speed and a hop.
-- A quarter second without progress releases the cable.
local ZIP_ARRIVE, ZIP_EXIT_SPEED = 120, 350
local function zip_tick(p, cam, seconds)
    local pos = vec.of(p.Location)
    local dir, dist = vec.norm(vec.sub(hook.anchor, pos))
    hook.length, hook.distance, hook.taut, hook.reel = dist, dist, true, 0
    share("length", string.format("%.0f", dist))
    share("distance", string.format("%.0f", dist))
    if dist < ZIP_ARRIVE then
        p.Velocity = vec.engine(vec.scale(dir, ZIP_EXIT_SPEED))
        return release("arrived", Boost:get())
    end
    if dist < (hook.best or math.huge) - 1 then
        hook.best, hook.stalled = dist, 0
    else
        hook.stalled = (hook.stalled or 0) + seconds
        if hook.stalled > 0.25 then return release("blocked") end
    end
    local speed = math.min(ZipSpeed:get(), 620 + 9 * (dist - ZIP_ARRIVE))
    p.Velocity = vec.engine(vec.scale(dir, speed))
    view.rope(hook, pos, cam)
end

-- ---- The rope's physics, each engine tick while hooked -----------------------------------------------------------------
local function rope_tick(seconds)
    if not hook then return end
    local p = mirage.pawn()
    if p ~= hook.pawn then return release("player changed") end
    hook.age = hook.age + seconds
    local mode = Mode:get()
    local cam = mirage.camera()
    if mode == "Zip" then return zip_tick(p, cam, seconds) end
    hook.reel = reel.out and -1 or (mode == "Pull" or reel.into) and 1 or 0
    local input = {
        dt = seconds,
        reel = hook.reel,
        steer = cam and cam.forward or nil,
    }
    local from = vec.of(p.Location)
    local pos, vel, taut = rope.step(hook, from, vec.of(p.Velocity), input, config())
    hook.taut = taut
    if vec.dist(from, pos) > 0.5 then
        if mirage.trace(from, pos, { extent = hook.extent, ignore = p }) then
            pos = from
            hook.blocked = hook.blocked + seconds
            if hook.blocked > 0.18 then return release("rope blocked") end
        else
            hook.blocked = 0
            p.Location = vec.engine(pos)
        end
    else
        hook.blocked = 0
    end
    p.Velocity = vec.engine(vel)
    hook.distance = vec.dist(pos, hook.anchor)
    view.rope(hook, pos, cam)
    share("length", string.format("%.0f", hook.length))
    share("distance", string.format("%.0f", hook.distance))
    -- Pull mode: there
    if mode == "Pull" and hook.distance < 160 then release("arrived", Boost:get()) end
end

-- ---- Aiming (every tick while the module is on) -----------------------------------------------------------------------
local function update_aim()
    local cam, p = mirage.camera(), mirage.pawn()
    aim = nil
    if not cam or not p then return end
    local eye, forward = vec.of(cam.location), vec.of(cam.forward)
    local pos, range = vec.of(p.Location), Range:get()
    local function target(dir, assisted)
        local hit = mirage.trace(eye, vec.add(eye, vec.scale(dir, range)), { ignore = p })
        if not hit then return end
        local at = vec.of(hit.location)
        local distance = vec.dist(at, pos)
        if distance < 50 or distance > range then return end
        return { hit = at, normal = vec.of(hit.normal), distance = distance, assisted = assisted }
    end
    aim = target(forward, false)
    if aim then return end
    -- A narrow fallback around the reticle makes ledges easier to catch when the centre ray just misses.
    local yaw = cam.rotation.yaw * math.pi / 32768
    local right = vec.new(-math.sin(yaw), math.cos(yaw), 0)
    for _, axis in ipairs({ right, vec.cross(forward, right) }) do
        for _, side in ipairs({ -1, 1 }) do
            aim = target(vec.norm(vec.add(forward, vec.scale(axis, side * 0.023))), true)
            if aim then return end
        end
    end
end

-- ---- The module -------------------------------------------------------------------------------------------------------
m = mirage.module {
    name = "Grappling Hook",
    tab = "Mods",
    description = "Fire a cable to zip, swing or reel in. Swing protects Faith from fall damage while attached.",
    on_render = function() view.hud(aim, hook, Mode:get(), feedback) end,
    on_disable = function() disabled = true end,
}
Mode, Range, ReelSpeed = mirage.setting("mode"), mirage.setting("range"), mirage.setting("reel_speed")
Swing, Yank, Boost = mirage.setting("swing"), mirage.setting("yank"), mirage.setting("release_boost")
ZipSpeed, Volume = mirage.setting("zip_speed"), mirage.setting("volume")
Status = m:text("Status", "free", "What the hook is doing")
Hooks = m:slider_int("Hooks fired", 0, 0, 1 << 30, "Times the hook caught this session")

-- Skip only the attached pawn's fall-damage calculation, without changing its damage settings.
local function prevent_fall_damage(call)
    if hook and m:enabled() and Mode:get() == "Swing" and call.object == hook.pawn then
        call:skip()
        blocked_damage = blocked_damage + 1
        share("fall_damage_blocked", blocked_damage)
    end
end
mirage.hook("TdGame.TdPlayerPawn.TakeFallingDamage", prevent_fall_damage)
mirage.hook("TdGame.TdPawn.TakeFallingDamage", prevent_fall_damage)

mirage.on("action", function(name, pressed)
    if not m:enabled() then return end
    if name == "fire" then
        if pressed and not hook then
            update_aim()
            if not aim or not attach(aim.hit, aim.normal) then
                feedback = { miss = true, time = 0.28 }
                play("miss")
            end
        elseif not pressed and Mode:get() ~= "Zip" then release("let go", Boost:get()) end  -- A zip runs to the end
    elseif name == "reel_in" then reel.into = pressed
    elseif name == "reel_out" then reel.out = pressed end
end)
local function clear_input()
    reel.into, reel.out, aim, feedback = false, false, nil, nil
end
mirage.on("tick", function(seconds)
    if disabled then
        disabled = false
        release("switched off")
        clear_input()
        if sound then mirage.sound_stop(sound) end
        sound = nil
        mirage.revert()
        move = nil
    end
    if not m:enabled() then return end
    if not move then move = mirage.move { name = "Grapple", as = "TdMove_Falling" } end
    if feedback then
        feedback.time = feedback.time - seconds
        if feedback.time <= 0 then feedback = nil end
    end
    if hook then rope_tick(seconds) else update_aim() end
end)
for _, event in ipairs({ "level_load", "death", "restart", "state_restored" }) do
    mirage.on(event, function()
        release(event)
        clear_input()
    end)
end

-- ---- For the console and tests -----------------------------------------------------------------------------------------
function grapple_at(point) return attach(vec.of(point)) end
function grapple_release() release("console") end
function grapple_status()
    return hook and string.format("hooked: length %.0f, %s", hook.length, hook.taut and "taut" or "slack") or "free"
end
