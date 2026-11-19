-- Blink: a first-party Mirage mod, all Lua, no build. It ships inside the menu's DLL and is written to
-- Binaries\mirage\mods\mirage-blink at startup: to change it, copy the folder under another id.
--
-- Hold the blink key (F) to aim: a marker shows where Faith would arrive. Let go and she crosses to it at 100 m/s,
-- through the air, and keeps the speed she had before; right mouse puts the aim away unused. (100 units are a metre.)
--
--   Range      a squashed sphere: 11 m (tier I) or 16 m (tier II) looking level or down; looking up, the aim is scaled
--              until its height is 5 m, so a blink reaches far across but only 5 m up.
--   Targeting  every tick while the key is held: a 20 cm box is swept along the aim. A wall within 3 m nudges the ray
--              up, then sideways, 30 cm at a time (past corners and lips). From the hit the spot pulls back in half her
--              width at a time until her whole body fits, never behind the camera, so blinking at a wall stops in front
--              of it. Then her body drops to find the ground, and stands on it when it is close below.
--   Ledges     a spot against a wall with a top within "Ledge reach" above it mantles: up the wall, then onto the top.
--   Travel     1 m every 10 ms, in steps of at most 50 cm, without collision (the targeting made the path safe); it stops
--              at the target, or on touching another pawn.
--   Arrival    the velocity from before the blink is given back, so a blink in a jump keeps its momentum.
--   Cooldown   1 s.
--   The view   a lens pass (shaders/blink.hlsl) gathers while aiming, tears toward the aim point in travel and settles
--              through the cooldown, with a short widening of the view; the aim point stays sharp.
--
-- Faith's left arm plays the mod's own animations (content/animations): raised while aiming, driven forward on letting
-- go, dropped back on arriving. Sounds, when sounds/charge.wav and sounds/release.wav are in the installed mod's folder.
--
-- For the console and tests: blink_to({x, y, z}) blinks to a point (as a let-go key does), blink_target() gives what
-- aiming finds now, blink_status() what the mod is doing, blink_count() the blinks so far.
local vec = require("vec")

local Tier, Ledge, Cooldown, FovKick, Volume           -- mod.toml settings
local Status, Blinks
local move                                             -- the mod's move (its EMovement number)

local UNITS = 100           -- A metre
local PROBE = 10            -- Half the swept box: 20 cm
local UP_REACH = 5 * UNITS  -- The most a blink rises
local NUDGE_NEAR = 3 * UNITS
local NUDGE_STEP = 30       -- 30 cm
local NUDGES = 4            -- Each way
local SPEED = 100 * UNITS   -- 1 m every 10 ms
local SUBSTEP = 50          -- 50 cm
local GROUND_DROP = 1.5 * UNITS
local KICK_TIME = 0.4

local aiming = false        -- The key is held
local target = nil          -- This tick's aim while aiming: {stand, via (a ledge's corner) or nil, kind}
local dash = nil            -- While blinking: {points, index, start, velocity}
local cooldown = 0          -- Seconds until the next blink
local kick = nil            -- The view's widening while it runs: {base = degrees, t}
local fx, look = nil, { warm = 0, travel = 0, wobble = 0 }    -- The lens pass and what drives it

local function share(k, v) mirage.share(k, tostring(v)) end

-- ---- Sounds and the arm --------------------------------------------------------------------------------------------------
local missing = {}
local charging = nil        -- The charge sound's handle while it plays
local function sound(name, options)
    if missing[name] then return nil end
    local h = mirage.sound_play("sounds/" .. name .. ".wav", options)
    if not h then missing[name] = true end
    return h
end
local function stop_charge()
    if charging then mirage.sound_stop(charging) end
    charging = nil
end
local function arm(sequence, options)
    local p = mirage.pawn()
    if not p then return end
    options.slot = "upperbody"
    mirage.anim_play(p, sequence, options)
end
local function arm_down(blend)
    local p = mirage.pawn()
    if p then mirage.anim_stop(p, "upperbody", blend or 0.2) end
end

-- ---- Her body ------------------------------------------------------------------------------------------------------------
local function body(p)
    local radius, half = 34, 96
    local ok, cyl = pcall(function() return p.CylinderComponent end)
    if ok and cyl then pcall(function() radius, half = cyl.CollisionRadius, cyl.CollisionHeight end) end
    return { x = radius, y = radius, z = half }
end
local function sweep(from, to, extent, p) return mirage.trace(from, to, { extent = extent, ignore = p, actors = true }) end
-- Her whole body fits at `at`: a tiny sweep there starts clear
local function fits(at, extent, p)
    local hit = sweep(at, vec.add(at, vec.new(0, 0, 1)), extent, p)
    return not hit or hit.time > 0
end
local function horizontal(v) return (vec.norm(vec.new(v.x, v.y, 0))) end

-- ---- Where a blink along the view ends ------------------------------------------------------------------------------------
local function reach(forward)
    local far = vec.scale(forward, Tier:get() == "II" and 16 * UNITS or 11 * UNITS)
    if far.z > UP_REACH then far = vec.scale(far, UP_REACH / far.z) end      -- The squashed sphere: only 5 m up
    return far
end

-- The probe along the aim; a wall close in front nudges it up, then sideways, for a way past
local function probe(eye, forward, p)
    local probeBox = { x = PROBE, y = PROBE, z = PROBE }
    local far = reach(forward)
    local hit = sweep(eye, vec.add(eye, far), probeBox, p)
    if hit and hit.time * vec.len(far) < NUDGE_NEAR and math.abs(vec.of(hit.normal).z) < 0.7 then
        local side = vec.norm(vec.new(-forward.y, forward.x, 0))
        local tries = {}
        for i = 1, NUDGES do tries[#tries + 1] = vec.new(0, 0, i * NUDGE_STEP) end
        for i = 1, NUDGES do
            tries[#tries + 1] = vec.scale(side, i * NUDGE_STEP)
            tries[#tries + 1] = vec.scale(side, -i * NUDGE_STEP)
        end
        for _, offset in ipairs(tries) do
            local from = vec.add(eye, offset)
            if fits(from, probeBox, p) then
                local h = sweep(from, vec.add(from, far), probeBox, p)
                if not h or h.time * vec.len(far) >= NUDGE_NEAR then return from, far, h end
            end
        end
    end
    return eye, far, hit
end

local function find_target(p, cam)
    local extent = body(p)
    local eye, forward = vec.of(cam.location), vec.norm(vec.of(cam.forward))
    local from, far, hit = probe(eye, forward, p)
    local dir, length = vec.norm(far)
    local spot = hit and vec.of(hit.location) or vec.add(from, far)
    -- Back along the aim until her body fits, never behind the camera. Where the ground is all that's in the way, the
    -- spot rises out of it instead (by half her height, or all of it)
    local function lifted(at)
        for _, up in ipairs({ 0, extent.z * 0.5, extent.z }) do
            local q = vec.add(at, vec.new(0, 0, up))
            if fits(q, extent, p) then return q end
        end
    end
    local back = 0
    local travelled = hit and hit.time * length or length
    local fitted = lifted(spot)
    while not fitted do
        back = back + extent.x * 0.5
        if back >= travelled then
            fitted = from
            break
        end
        spot = vec.sub(spot, vec.scale(dir, extent.x * 0.5))
        fitted = lifted(spot)
    end
    spot = fitted
    local wall = hit and math.abs(vec.of(hit.normal).z) < 0.7 and vec.of(hit.normal) or nil
    local why = nil         -- Why a wall gave no ledge (blink_target says)

    -- A ledge: against a wall whose top is within reach, up and over onto it
    if wall and Ledge:get() > 0 then
        local up = vec.add(spot, vec.new(0, 0, Ledge:get() + 4))
        local into = vec.scale(horizontal(vec.scale(wall, -1)), extent.x * 2 + 8)
        local above = vec.add(up, into)
        local below = vec.add(vec.add(spot, into), vec.new(0, 0, -extent.z * 0.5))
        local blocked = sweep(spot, up, extent, p)
        local over = not blocked and sweep(up, above, extent, p)
        local land = not blocked and not over and sweep(above, below, extent, p)
        if land and land.time > 0.001 and vec.of(land.normal).z > 0.7 then
            return { stand = vec.of(land.location), via = up, kind = "ledge", from = spot }
        end
        why = blocked and "no way up" or over and "no way over" or not land and "no top" or "the top isn't a floor"
    end

    -- The ground under it: she stands on it when it is close below, else the spot is in the air
    local ground = sweep(spot, vec.add(spot, vec.new(0, 0, -GROUND_DROP)), extent, p)
    if ground and vec.of(ground.normal).z > 0.7 then return { stand = vec.of(ground.location), kind = "floor", why = why } end
    return { stand = spot, kind = wall and "wall" or "air", why = why }
end

-- ---- The marker ----------------------------------------------------------------------------------------------------------
local function ring(center, radius, color, lines)
    local n, prev = 16, nil
    for i = 0, n do
        local a = i / n * math.pi * 2
        local pt = vec.add(center, vec.new(math.cos(a) * radius, math.sin(a) * radius, 0))
        if prev then lines[#lines + 1] = { prev, pt, color } end
        prev = pt
    end
end
local function draw_marker(p)
    if not target then return mirage.draw3d.clear() end
    local extent = body(p)
    local color = cooldown > 0 and mirage.rgba(150, 150, 170, 120)
        or target.kind == "air" and mirage.rgba(170, 200, 255, 160) or mirage.rgba(120, 220, 255, 230)
    local lines = {}
    local feet = vec.add(target.stand, vec.new(0, 0, -extent.z))
    local head = vec.add(target.stand, vec.new(0, 0, extent.z))
    ring(feet, extent.x, color, lines)
    ring(head, extent.x * 0.6, color, lines)
    for i = 0, 3 do
        local a = i / 4 * math.pi * 2
        local off = vec.new(math.cos(a) * extent.x, math.sin(a) * extent.x, 0)
        lines[#lines + 1] = { vec.add(feet, off), vec.add(head, vec.scale(off, 0.6)), color }
    end
    if target.via then
        lines[#lines + 1] = { target.from, target.via, color }
        lines[#lines + 1] = { target.via, target.stand, color }
    end
    mirage.draw3d.lines(lines)
end

-- ---- The view: the lens pass and the widening ------------------------------------------------------------------------------
local function set_fov(degrees)
    local c = mirage.controller()
    local cam = c and c.PlayerCamera
    if cam then mirage.call(cam, "SetFOV", { NewFOV = degrees }) end
end
local function view_tick(seconds)
    if kick then
        kick.t = kick.t + seconds
        if kick.t >= KICK_TIME then
            set_fov(0)
            kick = nil
        else
            local x = kick.t / KICK_TIME
            local shape = x < 0.25 and (x / 0.25) or (1 - (x - 0.25) / 0.75) ^ 2      -- Out fast, back slowly
            set_fov(kick.base + FovKick:get() * shape)
            share("fov", string.format("%.1f", kick.base + FovKick:get() * shape))
        end
    end
    -- The lens: gathering while aiming, torn in travel, a wobble dying away after it, fading with the cooldown
    look.warm = aiming and not dash and math.min(1, look.warm + seconds / 0.35) or math.max(0, look.warm - seconds / 0.15)
    look.travel = dash and 1 or math.max(0, look.travel - seconds / 0.25)
    look.wobble = look.wobble * math.exp(-seconds / 0.3)
    local cool = Cooldown:get() > 0 and cooldown / Cooldown:get() or 0
    if fx then
        local on = look.warm + look.travel + look.wobble + cool > 0.002
        if on then mirage.postfx_set(fx, 1, look.warm, look.travel, cool, look.wobble) end
        mirage.postfx_enable(fx, on)
        share("lens", on and string.format("%.2f %.2f %.2f %.2f", look.warm, look.travel, cool, look.wobble) or "off")
    end
end

-- ---- Blinking -------------------------------------------------------------------------------------------------------------
local function finish(why)
    if not dash then return end
    local p = mirage.pawn()
    if p then p.Velocity = vec.engine(dash.velocity) end        -- The speed she had before: blink-jumps keep it
    dash = nil
    cooldown = Cooldown:get()
    look.wobble = 1
    if why == "arrived" or why == "touched" then arm("BlinkLand", { blend_in = 0.04, blend_out = 0.15 }) end
    Status:set("cooldown (" .. why .. ")")
    share("state", "idle")
    share("ended", why)
end

local function start(t)
    local p = mirage.pawn()
    if not p or not move or dash or cooldown > 0 then
        share("denied", cooldown > 0 and "cooldown" or "busy")
        return false
    end
    local points = {}
    if t.via then
        points[#points + 1] = t.from
        points[#points + 1] = t.via
    end
    points[#points + 1] = t.stand
    dash = { points = points, index = 1, start = vec.of(p.Location), velocity = vec.of(p.Velocity) }
    mirage.move_start(move)
    p.Velocity = vec.engine(vec.new(0, 0, 0))
    arm("BlinkCast", { blend_in = 0.04, blend_out = 0 })
    stop_charge()
    sound("release", { volume = Volume:get() })
    local cam = mirage.camera()
    if FovKick:get() > 0 and cam and cam.fov then kick = { base = kick and kick.base or cam.fov, t = 0 } end
    Blinks:set(Blinks:get() + 1)
    Status:set("blinking (" .. t.kind .. ")")
    share("state", "blinking")
    share("kind", t.kind)
    share("blinks", Blinks:get())
    return true
end

-- Each tick: along the path at 100 m/s in steps of at most 50 cm, with no collision; the target ends it, and so does
-- touching another pawn on the way
local function dash_tick(p, seconds)
    local budget = SPEED * math.max(seconds, 1 / 240)
    local probeBox = { x = PROBE, y = PROBE, z = PROBE }
    while budget > 0 and dash do
        local pos = vec.of(p.Location)
        local goal = dash.points[dash.index]
        local dir, dist = vec.norm(vec.sub(goal, pos))
        local step = math.min(budget, SUBSTEP, dist)
        local nextPos = vec.add(pos, vec.scale(dir, step))
        local hit = mirage.trace(pos, nextPos, { extent = probeBox, ignore = p, actors = true })
        if hit and hit.actor and hit.actor ~= p and mirage.is_a(hit.actor, "Pawn") then
            p.Location = vec.engine(vec.of(hit.location))
            return finish("touched")
        end
        p.Location = vec.engine(nextPos)
        budget = budget - step
        if step >= dist - 0.01 then
            if dash.index < #dash.points then
                dash.index = dash.index + 1
            else
                share("distance", string.format("%.0f", vec.dist(dash.start, goal)))
                return finish("arrived")
            end
        end
    end
    p.Velocity = vec.engine(vec.new(0, 0, 0))       -- No gravity on the way
end

-- ---- The HUD: the crosshair while aiming, a flash without the lens pass ----------------------------------------------------
local flash = 0
local function hud()
    local w, h = mirage.draw.screen_size()
    if aiming and not dash then
        local cx, cy = w * 0.5, h * 0.5
        local color = cooldown > 0 and mirage.rgba(160, 160, 170, 140) or mirage.rgba(140, 225, 255, 230)
        mirage.draw.circle(cx, cy, 7, color, 2)
        if target then
            local sx, sy = mirage.world_to_screen(target.stand)
            if sx then mirage.draw.circle_filled(sx, sy, 4, color) end
        end
        if cooldown > 0 then mirage.draw.text(cx + 12, cy + 10, color, string.format("%.1f", cooldown)) end
    end
    if not fx and flash > 0 then
        mirage.draw.rect_filled(0, 0, w, h, mirage.rgba(200, 235, 255, math.floor(140 * flash / 0.25)))
    end
end

-- ---- The module -----------------------------------------------------------------------------------------------------------
local m = mirage.module {
    name = "Blink",
    tab = "Mods",
    description = "Hold F to aim a short teleport, let go to blink there (right mouse cancels)",
    on_tick = function()
        if not move then move = mirage.move { name = "Blink", as = "TdMove_Falling" } end
        local p, cam = mirage.pawn(), mirage.camera()
        target = (aiming and not dash and p and cam) and find_target(p, cam) or nil
        if p then draw_marker(p) end
    end,
    on_render = hud,
    on_disable = function()
        if aiming or dash then arm_down(0) end
        if kick then set_fov(0); kick = nil end
        if fx then mirage.postfx_enable(fx, false) end
        stop_charge()
        aiming, target, dash = false, nil, nil
        look = { warm = 0, travel = 0, wobble = 0 }
        mirage.draw3d.clear()
        mirage.revert()
        move = nil
    end,
}
Tier, Ledge, Cooldown = mirage.setting("tier"), mirage.setting("ledge"), mirage.setting("cooldown")
FovKick, Volume = mirage.setting("fov_kick"), mirage.setting("volume")
Status = m:text("Status", "ready", "What the blink is doing")
Blinks = m:slider_int("Blinks", 0, 0, 1 << 30, "Blinks so far")
fx = mirage.postfx("shaders/blink.hlsl")
share("state", "idle")
share("blinks", Blinks:get())

mirage.on("action", function(name, pressed)
    if not m:enabled() then return end
    if name == "blink" then
        if pressed then
            aiming = true
            if not dash then
                arm("BlinkAim", { blend_in = 0.15, blend_out = 0.2, loop = true })
                stop_charge()
                charging = sound("charge", { volume = Volume:get() })
            end
        elseif aiming then
            aiming = false
            stop_charge()
            if target and start(target) then flash = 0.25 else arm_down(0.2) end
        end
    elseif name == "cancel" and pressed and aiming then
        aiming = false
        arm_down(0.2)
        stop_charge()
    end
end)

mirage.on("tick", function(seconds)
    if not m:enabled() then return end
    local was = cooldown
    cooldown = math.max(0, cooldown - seconds)
    flash = math.max(0, flash - seconds)
    if was > 0 and cooldown == 0 then Status:set("ready") end
    if dash then
        local p = mirage.pawn()
        if not p then
            dash = nil
            share("state", "idle")
        else
            dash_tick(p, seconds)
        end
    end
    view_tick(seconds)
end)

for _, event in ipairs({ "level_load", "death", "restart" }) do
    mirage.on(event, function()
        aiming, target = false, nil
        if dash then
            dash = nil
            share("state", "idle")
            share("ended", event)
        end
        arm_down(0)
        stop_charge()
        if kick then set_fov(0); kick = nil end
        look = { warm = 0, travel = 0, wobble = 0 }
        cooldown = 0
    end)
end

-- ---- For the console and tests ----------------------------------------------------------------------------------------------
function blink_to(point)
    if point then return start({ stand = vec.of(point), kind = "console" }) end
    local p, cam = mirage.pawn(), mirage.camera()
    if not p or not cam then return false end
    return start(find_target(p, cam))
end
function blink_target()
    local p, cam = mirage.pawn(), mirage.camera()
    if not p or not cam then return "none" end
    local t = find_target(p, cam)
    return string.format("%s %.0f %.0f %.0f%s", t.kind, t.stand.x, t.stand.y, t.stand.z, t.why and (" (" .. t.why .. ")") or "")
end
function blink_count() return Blinks:get() end
function blink_status()
    if dash then return "blinking to point " .. dash.index .. " of " .. #dash.points end
    return cooldown > 0 and "cooldown" or "ready"
end
function blink_fx_error() return fx and mirage.postfx_error(fx) or "no effect" end
