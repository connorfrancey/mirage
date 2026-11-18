-- Grappling Hook: a Mirage showcase mod, all Lua, no build. Copy this folder into Binaries\mirage\mods\grapple.
--
-- Hold the fire key (middle mouse) while aiming at a surface in range: the hook catches, a rope is drawn in the world,
-- and you swing on it (Swing mode) or are reeled in (Pull mode). Z and X shorten and lengthen the rope; letting go of
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
--   mirage.draw            the HUD: a crosshair that shows whether the hook would catch, and the rope's length
--   mirage.on(...)         level loads, deaths and checkpoint restarts let go of the rope
--   mirage.share           what it is doing, for other mods and the self-test
--   globals                grapple_at({x, y, z}), grapple_release() and grapple_status() for the console (Misc >
--                          Console, mod "grapple") and tests
-- Its cost per tick is in the mod manager (the profiler), like any mod's.
local vec = require("vec")
local rope = require("rope")

local MOVE_WALKING = 1

local Mode, Range, ReelSpeed, Swing, Yank, Boost          -- mod.toml settings (found once the module exists)
local Status, Hooks                                        -- made in code below
local move                                                 -- the mod's move (its EMovement number)

local hook = nil          -- While hooked: {anchor = vec, length = number, taut = bool}
local aim = nil           -- This tick's aim: {hit = vec or nil, distance}
local reel = { into = false, out = false }

local function share(k, v) mirage.share(k, tostring(v)) end

local function config()
    return {
        range = Range:get(), reel_speed = ReelSpeed:get(), swing = Swing:get(),
        min_length = 120, drag = 0.15,
    }
end

-- ---- Hooking and letting go ------------------------------------------------------------------------------------------
local function release(why, boost)
    if not hook then return end
    hook = nil
    mirage.draw3d.clear()
    local p = mirage.pawn()
    if p and boost and boost > 0 then
        local v = vec.of(p.Velocity)
        p.Velocity = vec.engine(vec.add(v, vec.new(0, 0, boost)))
    end
    Status:set("free (" .. why .. ")")
    share("state", "free")
    share("released", why)
end

local function attach(anchor)
    local p = mirage.pawn()
    if not p or not move then return false end
    local pos = vec.of(p.Location)
    local toward, dist = vec.norm(vec.sub(anchor, pos))
    if dist > Range:get() or dist < 50 then return false end
    hook = { anchor = anchor, length = dist, taut = false }
    -- The catch: a yank toward the anchor, and off the ground if standing (the falling move takes it from there)
    local v = vec.add(vec.of(p.Velocity), vec.scale(toward, Yank:get()))
    if p.MovementState == MOVE_WALKING then v.z = math.max(v.z, 320) end
    if p.MovementState == MOVE_WALKING then
        p.Location = vec.engine(vec.add(pos, vec.new(0, 0, 12)))  -- Off the floor, or the falling move lands at once
    end
    p.Velocity = vec.engine(v)
    mirage.move_start(move)
    Hooks:set(Hooks:get() + 1)
    Status:set(string.format("hooked at %.0f units", dist))
    share("state", "hooked")
    share("hooks", Hooks:get())
    return true
end

-- ---- The rope, drawn in the world ------------------------------------------------------------------------------------
-- From the player's hand to the anchor: a straight line when taut, a sagging curve when slack
local function draw_rope(pos)
    local hand = vec.add(pos, vec.new(0, 0, 45))
    local dist = vec.dist(hand, hook.anchor)
    local sag = hook.taut and 0 or math.min(300, (hook.length - dist) * 0.5)
    local color = hook.taut and mirage.rgba(255, 170, 60) or mirage.rgba(230, 230, 230)
    local lines, prev = {}, hand
    local segments = 14
    for i = 1, segments do
        local t = i / segments
        local p = vec.lerp(hand, hook.anchor, t)
        p.z = p.z - sag * 4 * t * (1 - t)                  -- A parabola: the deepest in the middle
        lines[#lines + 1] = { prev, p, color }
        prev = p
    end
    local a, s = hook.anchor, 25                           -- The anchor: a small cross
    local red = mirage.rgba(255, 80, 60)
    lines[#lines + 1] = { vec.add(a, vec.new(-s, 0, 0)), vec.add(a, vec.new(s, 0, 0)), red }
    lines[#lines + 1] = { vec.add(a, vec.new(0, -s, 0)), vec.add(a, vec.new(0, s, 0)), red }
    lines[#lines + 1] = { vec.add(a, vec.new(0, 0, -s)), vec.add(a, vec.new(0, 0, s)), red }
    mirage.draw3d.lines(lines)
end

-- ---- The rope's physics, each engine tick while hooked -----------------------------------------------------------------
local function rope_tick(seconds)
    if not hook then return end
    local p = mirage.pawn()
    if not p then return release("no player") end
    local cam = mirage.camera()
    local input = {
        dt = seconds,
        reel = (Mode:get() == "Pull" or reel.into) and 1 or (reel.out and -1 or 0),
        steer = cam and cam.forward or nil,
    }
    local pos, vel, taut = rope.step(hook, vec.of(p.Location), vec.of(p.Velocity), input, config())
    hook.taut = taut
    if taut then p.Location = vec.engine(pos) end          -- Location, not SetLocation: that one doesn't move the player
    p.Velocity = vec.engine(vel)
    draw_rope(pos)
    share("length", string.format("%.0f", hook.length))
    share("distance", string.format("%.0f", vec.dist(pos, hook.anchor)))
    -- Pull mode: there
    if Mode:get() == "Pull" and vec.dist(pos, hook.anchor) < 160 then release("arrived", Boost:get()) end
end

-- ---- Aiming (every tick while the module is on) -----------------------------------------------------------------------
local function update_aim()
    local cam, p = mirage.camera(), mirage.pawn()
    if not cam or not p then
        aim = nil
        return
    end
    local far = vec.add(cam.location, vec.scale(cam.forward, Range:get()))
    local hit = mirage.trace(cam.location, far, { ignore = p })
    aim = { hit = hit and hit.location or nil, distance = hit and vec.dist(cam.location, hit.location) or nil }
end

-- ---- The HUD ----------------------------------------------------------------------------------------------------------
local function hud()
    local w, h = mirage.draw.screen_size()
    local cx, cy = w * 0.5, h * 0.5
    local ready = aim and aim.hit
    local color = hook and mirage.rgba(255, 170, 60) or ready and mirage.rgba(90, 230, 120) or mirage.rgba(200, 200, 200, 120)
    mirage.draw.circle(cx, cy, 11, color, 2)
    mirage.draw.line(cx - 18, cy, cx - 13, cy, color, 2)
    mirage.draw.line(cx + 13, cy, cx + 18, cy, color, 2)
    if ready and not hook then
        local sx, sy = mirage.world_to_screen(aim.hit)          -- Where it would catch
        if sx then mirage.draw.circle_filled(sx, sy, 4, color) end
        mirage.draw.text(cx + 16, cy + 12, color, string.format("%.0f", aim.distance))
    end
    if hook then
        local text = string.format("rope %.0f%s", hook.length, hook.taut and "" or "  (slack)")
        mirage.draw.text(cx + 16, cy + 12, color, text)
    end
end

-- ---- The module -------------------------------------------------------------------------------------------------------
local m = mirage.module {
    name = "Grappling Hook",
    tab = "Mods",
    description = "Hold middle mouse to fire a rope at a surface; Z / X reel in and out",
    on_tick = function()
        if not move then
            move = mirage.move { name = "Grapple", as = "TdMove_Falling" }
        end
        update_aim()
    end,
    on_render = hud,
    on_disable = function()
        release("switched off")
        mirage.draw3d.clear()
        mirage.revert()                                       -- The move goes with everything else the mod changed
        move = nil
    end,
}
Mode, Range, ReelSpeed = mirage.setting("mode"), mirage.setting("range"), mirage.setting("reel_speed")
Swing, Yank, Boost = mirage.setting("swing"), mirage.setting("yank"), mirage.setting("release_boost")
Status = m:text("Status", "free", "What the hook is doing")
Hooks = m:slider_int("Hooks fired", 0, 0, 1 << 30, "Times the hook caught this session")

mirage.on("action", function(name, pressed)
    if not m:enabled() then return end
    if name == "fire" then
        if pressed and aim and aim.hit then attach(aim.hit)
        elseif not pressed then release("let go", Boost:get()) end
    elseif name == "reel_in" then reel.into = pressed
    elseif name == "reel_out" then reel.out = pressed end
end)
mirage.on("tick", function(seconds)
    if hook and m:enabled() then rope_tick(seconds) end
end)
for _, event in ipairs({ "level_load", "death", "restart" }) do
    mirage.on(event, function() release(event) end)
end

-- ---- For the console and tests -----------------------------------------------------------------------------------------
function grapple_at(point) return attach(vec.of(point)) end
function grapple_release() release("console") end
function grapple_status()
    return hook and string.format("hooked: length %.0f, %s", hook.length, hook.taut and "taut" or "slack") or "free"
end
