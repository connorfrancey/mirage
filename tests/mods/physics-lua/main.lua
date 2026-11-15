-- Self-test mod (step 6, Scenario E). With its module on and a mesh shared by the self-test (shared "selftest" "mesh":
-- a loaded StaticMesh with collision), it:
--   1. finds a clear spot in front of the player with traces and spawns a prop above it (mirage.spawn_prop);
--   2. lets it fall: it must drop more than a body standing still would, then land and settle;
--   3. pushes it sideways with an impulse, then again with 4x the mass (mass_scale): the speed it gets is a quarter;
--   4. turns its gravity off and gives it an upward speed: it keeps rising (or the engine ignores the factor: noted);
--   5. changes its collision and reads it back; finds it with an overlap and with a downward trace that hits actors.
-- Every measurement is shared ("physics-lua" ...) for the self-test step "Mirage physics". Off: mirage.revert() takes
-- the prop away (and puts its collision back).
local m = mirage.module { name = "Physics (Lua)", tab = "Mods", description = "Self-test: a physics prop driven from Lua" }

local prop, phase, clock, mark = nil, "idle", 0, {}

-- In the air the push is small: a light prop stays slow enough not to reach a wall before its speed is read
local AIR_PUSH = { x = 5000, y = 0, z = 0 }

local function len(v) return math.sqrt(v.x * v.x + v.y * v.y + v.z * v.z) end
local function minus(a, b) return { x = a.x - b.x, y = a.y - b.y, z = a.z - b.z } end
local function share(k, v) mirage.share(k, tostring(v)) end
local function go(next) phase, clock = next, 0 end

-- A spot on the floor in front of the player with room above it: a trace down from 300 units up finds the floor, and
-- a trace from the player's eyes to above it is clear
local function spot()
    local pawn = mirage.pawn()
    local at = mirage.body(pawn).location
    local why = {}
    for _, dist in ipairs({ 250, 180, 400 }) do
        for _, up in ipairs({ 250, 150 }) do
            for i = 0, 7 do
                local a = i * math.pi / 4
                local p = { x = at.x + math.cos(a) * dist, y = at.y + math.sin(a) * dist, z = at.z + up }
                local block = mirage.trace({ x = at.x, y = at.y, z = at.z + 40 }, p, { actors = true, ignore = pawn })
                local floor = not block and mirage.trace(p, { x = p.x, y = p.y, z = p.z - 900 })
                if floor and floor.location.z < at.z + 60 and floor.location.z > at.z - 400 then return p end
                why[#why + 1] = block and ("blocked " .. math.floor(block.time * 100) .. "%")
                    or (floor and ("floor at " .. math.floor(floor.location.z - at.z)) or "no floor")
            end
        end
    end
    share("spot_why", table.concat(why, ", ", 1, math.min(#why, 8)))
end

local function step(dt)
    clock = clock + dt
    local b = prop and mirage.body(prop)
    if phase == "spawn" then
        local mesh = mirage.shared("selftest", "mesh")
        if mesh == "" then return end
        local at = spot()
        if not at then share("problem", "no clear spot in front of the player") return go("done") end
        prop = mirage.spawn_prop(mesh, at)
        if not prop then share("problem", "spawn_prop failed (the log says why)") return go("done") end
        b = mirage.body(prop)
        share("spawned", mesh)
        share("simulated", b.simulated)
        mark.z0 = b.location.z
        go("fall")
    elseif phase == "fall" then
        -- Settled: slow, after at least a second
        if clock > 1.0 and len(b.velocity) < 5 or clock > 4 then
            share("dropped", mark.z0 - b.location.z)
            share("fall_time", clock)
            go("push")
        end
    elseif phase == "push" then
        -- On the floor (friction): only that it moves
        if not mark.pushed then
            mark.pushed = true
            mirage.impulse(prop, { x = 20000, y = 0, z = 0 })
        elseif not mark.vfloor then
            mark.vfloor = len(b.velocity)
            share("speed_floor", mark.vfloor)
        elseif clock > 1.5 then
            go("float")
        end
    elseif phase == "float" then
        -- Gravity off, lifted 150 and still: an impulse's speed is exactly impulse / mass here (no floor, no friction)
        if not mark.floated then
            mark.floated = true
            mark.zf = b.location.z + 150
            mirage.set_body(prop, { gravity_scale = 0, location = { x = b.location.x, y = b.location.y, z = mark.zf },
                                    velocity = { x = 0, y = 0, z = 0 }, angular = { x = 0, y = 0, z = 0 }, awake = true })
        elseif clock > 0.5 and not mark.risen then
            mark.risen = true
            mark.spot = { x = b.location.x, y = b.location.y, z = b.location.z }   -- Where the others are measured too
            share("drift", b.location.z - mark.zf)       -- Near 0 with gravity off (it would fall about 120 in 0.5 s)
            mark.before1 = b.velocity
            mirage.impulse(prop, AIR_PUSH)
        elseif mark.risen and not mark.v1 then
            mark.v1 = len(minus(b.velocity, mark.before1))      -- The change the push made
            share("speed_1x", mark.v1)
        elseif mark.v1 then
            -- The same mesh at other masses, one after another at the same spot in the air (mass is given at spawn); each
            -- launched prop has moved off along x before the next comes
            local scales = { 0.25, 2, 4 }
            mark.i = mark.i or 1
            local scale = scales[mark.i]
            if not scale then
                share("gravity_kept", mirage.body(prop).gravity_scale)
                -- A live body's mass can't be changed: refused, the body untouched
                share("live_mass_refused", not mirage.set_body(prop, { mass_scale = 2 }) and mirage.body(prop).simulated)
                mirage.set_body(prop, { gravity_scale = 1, velocity = { x = 0, y = 0, z = 0 } })
                for _, o in ipairs(mark.others or {}) do mirage.set_body(o, { gravity_scale = 1, velocity = { x = 0, y = 0, z = 0 } }) end
                return go("query")
            end
            mark.others = mark.others or {}
            if mark.o == nil then
                -- Each a little higher (clear of the one launched before it), or lower when something is above
                local at = { x = mark.spot.x, y = mark.spot.y, z = mark.spot.z + 90 * mark.i }
                if mirage.trace(mark.spot, at, { actors = true, ignore = prop }) then
                    at = { x = mark.spot.x - 120 * mark.i, y = mark.spot.y, z = mark.spot.z }
                end
                mark.o = mirage.spawn_prop(mirage.shared("selftest", "mesh"), at, nil, scale) or false
                share("mass_" .. scale, mark.o and mirage.body(mark.o).mass_scale or "not spawned")
                if mark.o then
                    mark.others[#mark.others + 1] = mark.o
                    mirage.set_body(mark.o, { gravity_scale = 0, velocity = { x = 0, y = 0, z = 0 }, angular = { x = 0, y = 0, z = 0 }, awake = true })
                end
                mark.at = clock
            elseif not mark.pushedO and clock > mark.at + 0.3 then
                mark.pushedO = true
                if mark.o then
                    mark.beforeO = mirage.body(mark.o).velocity
                    mirage.impulse(mark.o, AIR_PUSH)
                end
            elseif mark.pushedO and not mark.vO then
                mark.vO = mark.o and len(minus(mirage.body(mark.o).velocity, mark.beforeO)) or 0
                share("speed_" .. scale, mark.vO)
                mark.at = clock
            elseif mark.vO and clock > mark.at + 0.6 then
                mark.i, mark.o, mark.pushedO, mark.vO = mark.i + 1, nil, nil, nil
            end
        end
    elseif phase == "query" then
        -- From just above it: a trace with actors also hits trigger actors (the Prologue has one over this floor)
        local above = { x = b.location.x, y = b.location.y, z = b.location.z + 120 }
        local hit = mirage.trace(above, b.location, { actors = true })
        share("trace_hit_prop", hit ~= nil and hit.actor == prop)
        share("trace_hit_what", hit and (mirage.name(hit.actor) or "?") or "nothing")
        share("trace_world", mirage.trace(above, b.location) ~= nil)
        local found = false
        for _, a in ipairs(mirage.overlap(b.location, 50)) do
            if a == prop then found = true end
        end
        share("overlap", found)
        local was = mirage.collision(prop)
        mirage.set_collision(prop, { collide_actors = was.collide_actors, block_actors = false, block_rigid_body = was.block_rigid_body })
        local now = mirage.collision(prop)
        share("block_before", was.block_actors)
        share("block_after", now.block_actors)
        go("done")
        share("done", 1)
    end
end

mirage.on("tick", function(dt)
    if m:enabled() then
        if phase == "idle" then go("spawn") end
        if phase ~= "done" then step(dt) end
    elseif phase ~= "idle" then
        share("reverted", mirage.revert())
        prop, phase, mark = nil, "idle", {}
    end
end)
