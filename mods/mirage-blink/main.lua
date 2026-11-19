-- Blink: a first-party Mirage mod, all Lua, no build. It ships inside the menu's DLL and is written to
-- Binaries\mirage\mods\mirage-blink at startup: to change it, copy the folder under another id.
--
-- Hold the blink key (F) to aim: a marker shows where Faith would arrive. Let go and she dashes there in a fraction of a
-- second, untouched by gravity on the way; right mouse puts the aim away unused. Where she arrives:
--
--   a floor              she stands on it
--   a wall below a ledge she goes to the wall, then up and over onto the ledge (within "Ledge reach")
--   a wall, no ledge     she stops against it in the air and falls from there
--   nothing in range     the end of her reach, in the air
--
-- Every spot comes from a box the size of her collision swept through the world (mirage.trace with an extent), so the
-- place the marker shows is one she fits in. The dash runs as the game's falling move (mirage.move, like the grapple's
-- zip): the game still lands her, grabs ledges and wall runs once it ends. A short cooldown follows each blink.
--
-- Faith's left arm plays the mod's own animations (content/animations): raised while aiming, driven forward on letting
-- go, dropped back on arriving.
--
-- For the console and tests: blink_to({x, y, z}) dashes to a point (the same way a let-go key does), blink_target()
-- gives what aiming finds right now, blink_status() what the mod is doing, blink_count() the blinks so far.
local vec = require("vec")

local Range, Speed, Ledge, Cooldown, KeepMomentum, Charges, Recharge, FovKick, Volume   -- mod.toml settings
local Status, Blinks
local move                                               -- the mod's move (its EMovement number)

local aiming = false        -- The key is held
local target = nil          -- This tick's aim while aiming: {stand = vec, via = vec or nil, kind = "floor"|"ledge"|"wall"|"air"}
local dash = nil            -- While blinking: {points = {vec...}, index, best, stalled, start, direction}
local cooldown = 0          -- Seconds until the next blink
local cooling = false       -- A cooldown is running (the status says "ready" again when it ends)
local flash = 0             -- Seconds left of the arrival flash
local charges = nil         -- Blinks available (fractional while refilling), full at the start
local kick = nil            -- The view's kick while it runs: {base = degrees, t = seconds into it}
local KICK_TIME = 0.4

local FLASH_TIME = 0.25
local ARRIVE = 24           -- Close enough to a point to take the next (units)
local EXIT_SPEED = 260      -- Speed kept along the dash when "Keep momentum" is on

local function share(k, v) mirage.share(k, tostring(v)) end

-- Sounds: sounds/charge.wav while aiming, sounds/release.wav on letting go (16-bit PCM WAVs in the installed mod's
-- folder; none ship with the mod). A file that isn't there is tried once: the mod is silent without it.
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

-- The left arm (content/animations): played on the upper body, so the legs keep running under it
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

-- Faith's collision, the box that is swept: half its width and half its height
local function body(p)
    local radius, half = 34, 96
    local ok, cyl = pcall(function() return p.CylinderComponent end)
    if ok and cyl then
        pcall(function() radius, half = cyl.CollisionRadius, cyl.CollisionHeight end)
    end
    return { x = radius, y = radius, z = half }
end

local function horizontal(v)
    local h = vec.norm(vec.new(v.x, v.y, 0))
    return h
end

-- ---- Where a blink along the view would end ---------------------------------------------------------------------------
-- The box goes from the eye along the view; where it stops is a spot she fits. A floor there is her standing spot. A
-- wall is tried for a ledge: from above the hit, a little past the wall's face, the box drops onto what's there.
local function find_target(p, cam)
    local extent = body(p)
    local eye = vec.of(cam.location)
    local far = vec.add(eye, vec.scale(cam.forward, Range:get()))
    local hit = mirage.trace(eye, far, { extent = extent, ignore = p, actors = true })
    if not hit then return { stand = far, kind = "air" } end
    local spot, normal = vec.of(hit.location), vec.of(hit.normal)
    if normal.z > 0.7 then return { stand = spot, kind = "floor" } end
    if normal.z > -0.7 and Ledge:get() > 0 then
        local into = vec.scale(horizontal(vec.scale(normal, -1)), extent.x * 2 + 8)
        local above = vec.add(vec.add(spot, into), vec.new(0, 0, Ledge:get() + 4))
        local below = vec.add(vec.add(spot, into), vec.new(0, 0, -extent.z * 0.5))
        -- The way up must be clear (nothing between the wall spot and the point above it) and the drop must land on a
        -- floor, starting in the open
        local up = vec.add(spot, vec.new(0, 0, Ledge:get() + 4))
        local blocked = mirage.trace(spot, up, { extent = extent, ignore = p, actors = true })
        local over = not blocked and mirage.trace(up, above, { extent = extent, ignore = p, actors = true })
        local land = not blocked and not over and mirage.trace(above, below, { extent = extent, ignore = p, actors = true })
        if land and land.time > 0.001 and vec.of(land.normal).z > 0.7 then
            return { stand = vec.of(land.location), via = up, kind = "ledge" }
        end
    end
    return { stand = spot, kind = "wall" }
end

-- ---- The marker, drawn in the world while aiming -----------------------------------------------------------------------
-- A ring at her feet and one at her head, joined: the space she'll take there; and a line up and over for a ledge
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
        or target.kind == "air" and mirage.rgba(170, 200, 255, 160)
        or mirage.rgba(120, 220, 255, 230)
    local lines = {}
    local feet = vec.add(target.stand, vec.new(0, 0, -extent.z))
    local head = vec.add(target.stand, vec.new(0, 0, extent.z))
    ring(feet, extent.x, color, lines)
    ring(head, extent.x * 0.6, color, lines)
    for i = 0, 3 do
        local a = i / 4 * math.pi * 2
        local off = vec.new(math.cos(a) * extent.x, math.sin(a) * extent.x, 0)
        local offTop = vec.scale(off, 0.6)
        lines[#lines + 1] = { vec.add(feet, off), vec.add(head, offTop), color }
    end
    if target.via then
        local from = vec.add(target.via, vec.new(0, 0, -(Ledge:get() + 4)))
        lines[#lines + 1] = { from, target.via, color }
        lines[#lines + 1] = { target.via, target.stand, color }
    end
    mirage.draw3d.lines(lines)
end

-- ---- Blinking ----------------------------------------------------------------------------------------------------------
local function finish(why)
    if not dash then return end
    dash = nil
    cooldown, cooling = Cooldown:get(), true
    if why == "arrived" or why == "blocked" then arm("BlinkLand", { blend_in = 0.04, blend_out = 0.15 }) end
    Status:set("ready in " .. string.format("%.2f", cooldown) .. " s (" .. why .. ")")
    share("state", "idle")
share("blinks", Blinks:get())
    share("ended", why)
end

-- Starts a dash through `t`'s points: the wall spot and the point above it first for a ledge, then the spot itself
local function start(t)
    local p = mirage.pawn()
    if not p or not move or dash or cooldown > 0 then return false end
    if charges < 1 then
        Status:set("out of charges")
        share("denied", "charges")
        return false
    end
    charges = charges - 1
    local points = {}
    if t.via then
        points[#points + 1] = vec.add(t.via, vec.new(0, 0, -(Ledge:get() + 4)))
        points[#points + 1] = t.via
    end
    points[#points + 1] = t.stand
    local from = vec.of(p.Location)
    dash = { points = points, index = 1, best = math.huge, stalled = 0, start = from, kind = t.kind }
    if p.MovementState == 1 then                         -- Walking: off the floor, or the falling move lands at once
        p.Location = vec.engine(vec.add(from, vec.new(0, 0, 4)))
    end
    mirage.move_start(move)
    arm("BlinkCast", { blend_in = 0.04, blend_out = 0 })
    stop_charge()
    sound("release", { volume = Volume:get() })
    local cam = mirage.camera()
    if FovKick:get() > 0 and cam and cam.fov then kick = { base = kick and kick.base or cam.fov, t = 0 } end
    flash = FLASH_TIME
    Blinks:set(Blinks:get() + 1)
    Status:set("blinking (" .. t.kind .. ")")
    share("state", "blinking")
    share("kind", t.kind)
    share("blinks", Blinks:get())
    share("charges", string.format("%.2f", charges))
    return true
end

-- Each tick of a dash: straight at the next point at the dash speed (so gravity has no time to act). Reaching it, or
-- the tick's step passing it, puts her on it exactly. A dash that stops getting closer (something moved into the way)
-- ends where she is.
local function dash_tick(p, seconds)
    local pos = vec.of(p.Location)
    local goal = dash.points[dash.index]
    local dir, dist = vec.norm(vec.sub(goal, pos))
    local step = Speed:get() * math.max(seconds, 1 / 240)
    if dist <= math.max(ARRIVE, step) then
        p.Location = vec.engine(goal)
        if dash.index < #dash.points then
            dash.index, dash.best, dash.stalled = dash.index + 1, math.huge, 0
            return
        end
        local keep = KeepMomentum:get() and vec.scale(horizontal(vec.sub(goal, dash.start)), EXIT_SPEED) or vec.new(0, 0, 0)
        p.Velocity = vec.engine(keep)
        share("distance", string.format("%.0f", vec.dist(dash.start, goal)))
        return finish("arrived")
    end
    if dist < dash.best - 1 then
        dash.best, dash.stalled = dist, 0
    else
        dash.stalled = dash.stalled + seconds
        if dash.stalled > 0.12 then
            p.Velocity = vec.engine(vec.new(0, 0, 0))
            return finish("blocked")
        end
    end
    p.Velocity = vec.engine(vec.scale(dir, Speed:get()))
end

-- ---- The view's kick: wider for the dash, easing back as she arrives ----------------------------------------------------
-- The camera's own FOV lock (Camera.SetFOV): set each tick while the kick runs, and let go (0) after
local function set_fov(degrees)
    local c = mirage.controller()
    local cam = c and c.PlayerCamera
    if cam then mirage.call(cam, "SetFOV", { NewFOV = degrees }) end
end
local function kick_tick(seconds)
    if not kick then return end
    kick.t = kick.t + seconds
    if kick.t >= KICK_TIME then
        set_fov(0)
        kick = nil
        return
    end
    local x = kick.t / KICK_TIME
    local shape = x < 0.25 and (x / 0.25) or (1 - (x - 0.25) / 0.75) ^ 2      -- Out fast, back slowly
    set_fov(kick.base + FovKick:get() * shape)
    share("fov", string.format("%.1f", kick.base + FovKick:get() * shape))
end

-- ---- The HUD: a vignette while aiming, a flash when blinking, the cooldown ---------------------------------------------
local function frame(w, h, inset, thickness, color)
    mirage.draw.rect_filled(inset, inset, w - inset, inset + thickness, color)
    mirage.draw.rect_filled(inset, h - inset - thickness, w - inset, h - inset, color)
    mirage.draw.rect_filled(inset, inset + thickness, inset + thickness, h - inset - thickness, color)
    mirage.draw.rect_filled(w - inset - thickness, inset + thickness, w - inset, h - inset - thickness, color)
end

local function hud()
    local w, h = mirage.draw.screen_size()
    if aiming and not dash then
        for i = 0, 5 do frame(w, h, i * 14, 14, mirage.rgba(40, 90, 140, 70 - i * 11)) end
        local cx, cy = w * 0.5, h * 0.5
        local color = cooldown > 0 and mirage.rgba(160, 160, 170, 140) or mirage.rgba(140, 225, 255, 230)
        mirage.draw.circle(cx, cy, 7, color, 2)
        if target then
            local sx, sy = mirage.world_to_screen(target.stand)
            if sx then mirage.draw.circle_filled(sx, sy, 4, color) end
        end
        if cooldown > 0 then mirage.draw.text(cx + 12, cy + 10, color, string.format("%.1f", cooldown)) end
    end
    -- The charges: pips under the crosshair while aiming or refilling
    if charges and (aiming or charges < Charges:get()) then
        local n, cx = Charges:get(), w * 0.5
        for i = 1, n do
            local x = cx + (i - (n + 1) / 2) * 16
            local fill = math.max(0, math.min(1, charges - (i - 1)))
            mirage.draw.rect(x - 5, h * 0.5 + 26, x + 5, h * 0.5 + 30, mirage.rgba(140, 225, 255, 120), 1)
            if fill > 0 then mirage.draw.rect_filled(x - 5, h * 0.5 + 26, x - 5 + 10 * fill, h * 0.5 + 30, mirage.rgba(140, 225, 255, fill >= 1 and 230 or 110)) end
        end
    end
    if flash > 0 then
        local a = math.floor(140 * (flash / FLASH_TIME))
        mirage.draw.rect_filled(0, 0, w, h, mirage.rgba(200, 235, 255, a))
    end
end

-- ---- The module ---------------------------------------------------------------------------------------------------------
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
        stop_charge()
        aiming, target, dash = false, nil, nil
        mirage.draw3d.clear()
        mirage.revert()
        move = nil
    end,
}
Range, Speed, Ledge = mirage.setting("range"), mirage.setting("speed"), mirage.setting("ledge")
Cooldown, KeepMomentum = mirage.setting("cooldown"), mirage.setting("keep_momentum")
Charges, Recharge, FovKick = mirage.setting("charges"), mirage.setting("recharge"), mirage.setting("fov_kick")
Volume = mirage.setting("volume")
charges = Charges:get()
Status = m:text("Status", "ready", "What the blink is doing")
Blinks = m:slider_int("Blinks", 0, 0, 1 << 30, "Blinks this session")
share("state", "idle")

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
            if target then start(target) end
        end
    elseif name == "cancel" and pressed and aiming then
        aiming = false
        arm_down(0.2)
        stop_charge()
    end
end)

mirage.on("tick", function(seconds)
    if not m:enabled() then return end
    cooldown = math.max(0, cooldown - seconds)
    flash = math.max(0, flash - seconds)
    charges = math.min(Charges:get(), charges + seconds / Recharge:get())
    kick_tick(seconds)
    if cooling and cooldown == 0 then
        cooling = false
        Status:set("ready")
    end
    if dash then
        local p = mirage.pawn()
        if not p then return finish("no player") end
        dash_tick(p, seconds)
    end
end)

for _, event in ipairs({ "level_load", "death", "restart" }) do
    mirage.on(event, function()
        aiming, target = false, nil
        finish(event)
        arm_down(0)
        stop_charge()
        if kick then set_fov(0); kick = nil end
        cooldown, charges = 0, Charges:get()
    end)
end

-- ---- For the console and tests -----------------------------------------------------------------------------------------
-- A blink straight to `point` (a standing spot), or along the view when nil: the same dash a let-go key starts
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
    return string.format("%s %.0f %.0f %.0f", t.kind, t.stand.x, t.stand.y, t.stand.z)
end
function blink_count() return Blinks:get() end
function blink_status()
    if dash then return "blinking to point " .. dash.index .. " of " .. #dash.points end
    if cooldown > 0 then return "cooldown" end
    return charges >= 1 and "ready" or "out of charges"
end
