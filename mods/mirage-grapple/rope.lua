-- The rope's physics, one engine tick at a time. Pure Lua over vectors: no game calls here, so it is easy to read and to
-- try in the console (Misc > Console, mod "mirage-grapple": `require("rope").step(...)`).
--
-- The rope is a constraint, not a spring: the player may be closer to the anchor than the rope is long (it goes slack),
-- never farther. Each tick: the rope reels in or out, then a player beyond its length is put back on the sphere around
-- the anchor and loses the part of the velocity that points away from it (what a taut rope does to a pendulum), keeping
-- the rest: swinging is gravity (the game's falling physics) plus this.
local vec = require("vec")
local rope = {}

-- state: {anchor, length}; pos, vel: the player's; input: {reel = -1 | 0 | 1, steer = vector, dt}; config: settings.
-- Returns the new position, velocity and whether the rope is taut.
function rope.step(state, pos, vel, input, config)
    local dt = input.dt
    if input.reel ~= 0 then
        state.length = math.max(config.min_length, math.min(config.range, state.length - input.reel * config.reel_speed * dt))
    end
    local out, dist = vec.norm(vec.sub(pos, state.anchor))
    local taut = dist >= state.length - 1
    if input.steer then
        local steer = vec.norm(vec.new(input.steer.x, input.steer.y, 0))
        if taut then steer = vec.sub(steer, vec.scale(out, vec.dot(steer, out))) end
        vel = vec.add(vel, vec.scale(steer, config.swing * dt))
    end
    if dist > state.length then
        pos = vec.add(state.anchor, vec.scale(out, state.length))
    end
    if taut then
        local away = vec.dot(vel, out)
        if away > 0 then vel = vec.sub(vel, vec.scale(out, away)) end
    end
    -- A little drag, so a swing settles instead of gaining energy from the position corrections
    vel = vec.scale(vel, 1 - math.min(1, config.drag * dt))
    return pos, vel, taut
end

return rope
