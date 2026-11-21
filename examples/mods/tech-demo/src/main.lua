local vec = mirage.vec
local draw = mirage.draw
local ui = mirage.ui
local cyan = mirage.rgba(106, 229, 255)
local amber = mirage.rgba(255, 197, 114)
local white = mirage.rgba(231, 242, 249)
local dark = mirage.rgba(10, 20, 29, 220)
local radius = 75
local model = "models/Probe.obj"
local instrumentClass = "classes/MirageFieldInstrument.uc"
local launchSpeed, volume, showHud
local request, run, module
local shader, sound, animatedPawn
local phase, flash = "ready", 0
local report = "Deploy on an open stretch of floor."
local lifetime = tonumber(mirage.config.get("pulses", "0")) or 0
local intensity = mirage.menu_slider {
    label = "Field Lab pulse intensity", min = 0, max = 1, value = 0.45, step = 0.05,
}

local function status(state, message)
    phase, report = state, message
    mirage.share("status", state)
    mirage.share("message", message)
end

local function clear()
    if run then
        if run.hook then run.hook:remove() end
        if run.transaction then mirage.revert(run.transaction) end
        mirage.draw3d.clear()
    end
    if animatedPawn and mirage.valid(animatedPawn) then
        local animation = mirage.anim_state(animatedPawn, "upperbody")
        if animation and animation.playing and animation.sequence == "MirageFieldPulse" then
            mirage.anim_stop(animatedPawn, "upperbody", 0.1)
        end
    end
    if sound then mirage.sound_stop(sound) end
    if shader then mirage.postfx_enable(shader, false) end
    run, sound, animatedPawn, flash = nil, nil, nil, 0
    status("ready", "Deploy on an open stretch of floor.")
end

local function floor_at(at)
    local hit = mirage.trace(at + vec.up * 70, at - vec.up * 175)
    if hit and hit.normal.z > 0.85 then return hit.location end
end

local function placement(pawn, camera)
    local state = mirage.body(pawn)
    if not state then return end
    local origin = state.location
    local yaw = camera.rotation.yaw * math.pi / 32768
    local forward = vec(math.cos(yaw), math.sin(yaw), 0)
    local right = vec(-forward.y, forward.x, 0)
    for _, distance in ipairs({ 270, 360, 190 }) do
        local center = floor_at(origin + forward * distance)
        if center and math.abs(center.z - origin.z) < 130 then
            local raised = center + vec.up * 85
            local wall = mirage.trace(origin, raised, { extent = vec(21, 21, 26) })
            if not wall then
                for _, side in ipairs({ right, -right }) do
                    local base = floor_at(raised + side * 136)
                    if base and math.abs(base.z - center.z) < 50 then
                        local probe = base + vec.up * 70
                        local blocked = mirage.trace(raised, probe, { extent = vec(33, 33, 33) })
                        local above = mirage.trace(probe, probe + vec.up * 120, { extent = vec(33, 33, 33) })
                        if not blocked and not above then return center, probe end
                    end
                end
            end
        end
    end
end

local function marker(center, color, length)
    local lines = {}
    local function point(angle)
        return center + vec(math.cos(angle) * radius, math.sin(angle) * radius, 5)
    end
    for i = 0, 23 do
        local a, b = point(i * math.pi / 12), point((i + 1) * math.pi / 12)
        lines[#lines + 1] = { a, b, color }
        if i % 6 == 0 then lines[#lines + 1] = { a, a + vec.up * length, color } end
    end
    mirage.draw3d.lines(lines)
end

local function wire_kismet(session, sequence)
    if not sequence then return end
    local event = mirage.kismet_create(sequence, "Engine.SeqEvent_RemoteEvent")
    local delay = mirage.kismet_create(sequence, "Engine.SeqAct_Delay")
    if not event or not delay then return end
    event.Originator = session.instrument
    event.MaxTriggerCount = 0
    event.ReTriggerDelay = 0
    delay.Duration = 0.15
    if mirage.kismet_link(event, nil, delay, nil) then
        session.event, session.delay = event, delay
        session.kismet = "Wired: event to delay"
    end
end

local function pulse(session, pawn)
    if session ~= run or pawn ~= session.pawn or flash > 0 or phase ~= "deployed" then return end
    if not mirage.valid(session.instrument) or not mirage.valid(session.probe) then return end
    session.pulses = mirage.call(session.instrument, "RecordPulse")
    session.impulse = mirage.impulse(session.probe, vec.up * launchSpeed:get(), nil, true)
    session.delay_count = session.delay and session.delay.ActivateCount
    session.kismet_fired = session.event and mirage.kismet_fire(session.event, nil, pawn) or false
    if session.kismet_fired then session.kismet = "Event sent" end
    if sound then mirage.sound_stop(sound) end
    sound = mirage.sound_play("sounds/pulse.wav", { volume = volume:get() })
    session.audio = sound ~= nil
    session.animation = mirage.anim_play(pawn, "MirageFieldPulse", { slot = "upperbody", blend_in = 0.05 })
    if session.animation then animatedPawn = pawn end
    shader = shader or mirage.postfx("shaders/pulse.hlsl")
    lifetime = lifetime + 1
    mirage.config.set("pulses", lifetime)
    mirage.share("pulses", session.pulses)
    flash = 0.65
    marker(session.center, amber, 44)
    status("deployed", "Pulse sent. Step out of the ring and back in to repeat.")
end

local function deploy(session, mesh)
    local cls, why = mirage.load_class(instrumentClass)
    if not cls then
        clear()
        status("error", "UnrealScript: " .. (why or "class unavailable"))
        return
    end
    local world = mirage.world()
    local sequence = world and mirage.call(world, "GetGameSequence")
    session.transaction = mirage.transaction("Deploy Field Lab", function()
        session.instrument = mirage.spawn(instrumentClass, session.center + vec.up * 20)
        session.probe = mirage.spawn_prop(mirage.path(mesh), session.probeAt)
        if session.instrument and session.probe then
            wire_kismet(session, sequence)
            session.trigger = mirage.trigger {
                location = session.center + vec.up * 55, radius = radius, height = 85, class = "Pawn",
                on_enter = function(_, pawn) if run then pulse(run, pawn) end end,
            }
        end
    end)
    if not session.trigger then
        clear()
        status("error", "Could not place the probe and trigger here. Try an open area.")
        return
    end
    session.hook = mirage.hook(mirage.path(cls) .. ".RecordPulse", function()
        if run then run.hits = run.hits + 1 end
    end, { when = "after" })
    mirage.share("pulses", 0)
    marker(session.center, cyan, 26)
    status("deployed", "Step into the cyan scanner ring.")
end

local function start(pawn)
    clear()
    local camera = pawn and mirage.camera()
    if not camera then return status("blocked", "Enter a playable level to deploy.") end
    local center, probeAt = placement(pawn, camera)
    if not center then return status("blocked", "No clear floor ahead. Move to an open spot and try again.") end
    run = { pawn = pawn, center = center, probeAt = probeAt, pulses = 0, hits = 0, kismet = "No sequence available" }
    status("building", "Preparing the probe model and UnrealScript instrument...")
end

local function tick(dt)
    local command = request
    request = nil
    if command == "clear" then clear() end
    if not module:enabled() or (not run and command ~= "deploy") then return end
    local pawn = mirage.pawn()
    if run and pawn ~= run.pawn then clear() end
    if command == "deploy" then start(pawn) end
    if not run then return end
    if phase == "building" then
        local mesh, state = mirage.model(model)
        if mesh then
            deploy(run, mesh)
        elseif state ~= "pending" then
            clear()
            status("error", "Could not load the probe model. Check the Mirage log.")
        end
        return
    end
    if run.kismet_fired and mirage.valid(run.delay) and run.delay.ActivateCount > run.delay_count then
        run.kismet = "Delay activated"
        run.kismet_fired = false
    end
    if flash > 0 then
        flash = math.max(0, flash - dt)
        if flash == 0 then marker(run.center, cyan, 26) end
        if shader then
            local strength = intensity and mirage.menu_slider_value(intensity) or 0
            mirage.postfx_set(shader, 1, strength * flash / 0.65, 1 - flash / 0.65, 0, 0)
            mirage.postfx_enable(shader, flash > 0 and strength > 0)
        end
    end
end

local function render()
    if not showHud:get() then return end
    local _, height = draw.screen_size()
    local y = height - 112
    draw.rect_filled(22, y, 482, y + 86, dark)
    draw.rect_filled(22, y, 25, y + 86, phase == "error" and amber or cyan)
    draw.text(37, y + 10, cyan, "MIRAGE FIELD LAB")
    draw.text(37, y + 33, white, report)
    draw.text(37, y + 57, white, string.format("Session pulses  %d   |   Lifetime pulses  %d", run and run.pulses or 0, lifetime))
    if run and phase == "deployed" then
        local x, sy = mirage.world_to_screen(run.center + vec.up * 24)
        if x then
            draw.circle(x, sy, 5, flash > 0 and amber or cyan, 2)
        end
    end
end

module = mirage.module {
    name = "Field Lab", tab = "Mods",
    description = "A portable showcase for the Mirage mod loader",
    on_render = render,
    on_disable = function() request = "clear" end,
    on_panel = function()
        if not module:enabled() then
            ui.text_wrapped("Enable Field Lab to deploy it.")
            return
        end
        ui.text_wrapped("Deploy on open floor, then walk into the cyan scanner ring. Use the mod's Actions to view or rebind its keys.")
        if ui.button("Deploy here") then request = "deploy" end
        ui.same_line()
        if ui.button("Pack up") then request = "clear" end
        ui.text_wrapped(report)
        if run and phase == "deployed" then
            ui.separator()
            ui.label_text("UnrealScript pulses", tostring(run.pulses))
            ui.label_text("Hook calls", tostring(run.hits))
            ui.label_text("Kismet", run.kismet)
            ui.label_text("Physics impulse", run.impulse and "Applied" or "Ready")
            ui.label_text("Custom animation", run.animation and "Played" or "Ready")
            ui.label_text("Mod audio", run.audio and "Played" or "Ready")
        end
    end,
}
launchSpeed = mirage.setting("launch_speed")
volume = mirage.setting("volume")
showHud = mirage.setting("hud")
mirage.on("tick", tick)
mirage.on("action", function(name, down)
    if down and module:enabled() then
        if name == "deploy" then request = "deploy" end
        if name == "clear" then request = "clear" end
    end
end)
for _, event in ipairs({ "level_load", "death", "restart", "state_restored" }) do
    mirage.on(event, function() request = "clear" end)
end

-- The game's self-test reads the same state the panel presents. No action is taken from this function.
function field_lab_status()
    if not run then return { phase = phase, message = report } end
    return {
        phase = phase, message = report, pulses = run.pulses, hooks = run.hits, center = run.center,
        model = run.probe and mirage.valid(run.probe) or false, kismet = run.kismet,
        impulse = run.impulse, animation = run.animation, audio = run.audio,
        shader_error = shader and mirage.postfx_error(shader) or "",
    }
end
