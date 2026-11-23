-- Speedometer: position, speed, angles and more in a small window, and the trainer's checkpoint timer (set a
-- checkpoint, load a state: times how long you take to get back to it; it restarts on every Save State load, which the
-- menu shares as mirage.shared("mirage", "save_state.loads")).

local later = require("later")
local ui = mirage.ui

local values = nil                 -- What the window shows: copied on the game thread
local top_speed = { value = 0, at = -1e9, any = false }
local top_height = { value = 0, at = -1e9, any = false }
local checkpoint = { set = false }

-- Highest value of the last 2.75 s (the trainer's top speed / height, like the game's own meter)
local function peak(p, v, now)
    if now - p.at > 2.75 then p.any = false end
    if not p.any or v > p.value then p.value, p.at, p.any = v, now, true end
    return p.value
end

local function loads() return tonumber(mirage.shared("mirage", "save_state.loads")) or 0 end

local function update_checkpoint(pawn, world)
    local c = checkpoint
    local now = world.TimeSeconds                   -- Game time: slowed with the world, like the trainer
    local dt = now - (c.last or now)
    c.last = now
    if not c.set then return end
    local n = loads()
    if n ~= c.seen_loads then
        c.seen_loads, c.time, c.running = n, 0, true
        return
    end
    if not c.running then return end
    local l = pawn.Location
    local dx, dy, dz = l.X - c.x, l.Y - c.y, l.Z - c.z
    if math.sqrt(dx * dx + dy * dy + dz * dz) <= Radius:get() then c.running = false
    elseif dt > 0 and dt < 1 then c.time = c.time + dt end
end

local function set_checkpoint()
    later.run(function()
        local pawn, world = mirage.pawn(), mirage.world()
        if not pawn then return end
        local l = pawn.Location
        checkpoint = { set = true, x = l.X, y = l.Y, z = l.Z, time = 0, running = false, seen_loads = loads(),
                       last = world and world.TimeSeconds }
    end)
end

local UNITS = { ["u/s"] = 1, ["km/h"] = 0.036, ["m/s"] = 0.01 }    -- 1 unit = 1 cm

local speedo
speedo = mirage.module {
    name = "Speedometer",
    tab = "Trainer",
    description = "Position, speed, angles and more in a small window",
    order = 70,
    on_enable = function() values = nil end,
    on_tick = function()
        if not speedo:enabled() then return end
        local pawn, pc, world = mirage.pawn(), mirage.controller(), mirage.world()
        if not (pawn and pc and world) then
            values = nil
            top_speed.any, top_height.any = false, false
            return
        end
        local now = world.RealTimeSeconds
        local l, v, rot = pawn.Location, pawn.Velocity, pc.Rotation
        local speed = math.sqrt(v.X * v.X + v.Y * v.Y)
        local jump = pawn.LastJumpLocation
        update_checkpoint(pawn, world)
        values = {
            x = l.X, y = l.Y, z = l.Z, speed = speed,
            top_speed = peak(top_speed, speed, now), top_height = peak(top_height, l.Z, now),
            pitch = (rot.Pitch & 0xFFFF) / 65536 * 360, yaw = (rot.Yaw & 0xFFFF) / 65536 * 360,
            state = pawn.MovementState, health = pawn.Health,
            reaction = math.min(pc.ReactionTimeEnergy, 100),
            jump_z = jump.Z, since_jump = l.Z - jump.Z,
            checkpoint = checkpoint.set and checkpoint.time or nil,
        }
    end,
    on_render = function()
        local v = values
        if not v then return end
        local flags = ui.WINDOW_AUTO_SIZE + ui.WINDOW_NO_COLLAPSE
        if not TitleBar:get() then flags = flags + ui.WINDOW_NO_TITLE end
        if not mirage.menu_open() then flags = flags + ui.WINDOW_NO_INPUTS end     -- Movable only with the menu open
        ui.window_next_pos(60, 60)
        ui.window_next_bg_alpha(Opacity:get())
        if ui.window_begin("Speedometer##trainer", flags) then
            local label_color, value_color, offset = LabelColor:get(), ValueColor:get(), ValueColumn:get()
            local function row(label, fmt, ...)
                ui.text_colored(label_color, label)
                ui.same_line(offset)
                ui.text_colored(value_color, string.format(fmt, ...))
            end
            local unit = Unit:get()
            local scale = UNITS[unit] or 1
            if ShowPosition:get() then
                row("X", "%.2f", v.x)
                row("Y", "%.2f", v.y)
                row("Z", "%.2f", v.z)
                row("Top Z", "%.2f", v.top_height)
            end
            if ShowSpeed:get() then
                row("Speed", "%.2f %s", v.speed * scale, unit)
                row("Top speed", "%.2f %s", v.top_speed * scale, unit)
            end
            if ShowAngles:get() then
                row("Pitch", "%.2f", v.pitch)
                row("Yaw", "%.2f", v.yaw)
            end
            if ShowCheckpoint:get() then
                if v.checkpoint then row("Checkpoint", "%.2f s", v.checkpoint) else row("Checkpoint", "%s", "not set") end
            end
            if ShowState:get() then
                row("State", "%d", v.state)
                row("Health", "%d", v.health)
                row("Reaction", "%.0f", v.reaction)
            end
            if ShowJump:get() then
                row("Jump Z", "%.2f", v.jump_z)
                row("Since jump", "%.2f", v.since_jump)
            end
        end
        ui.window_end()
    end,
    on_panel = function()
        local w = ui.content_width()
        Radius:set_visible(ShowCheckpoint:get())
        if ui.button("Set checkpoint here", w, 0) then set_checkpoint() end
        if ui.button("Clear checkpoint", w, 0) then later.run(function() checkpoint = { set = false } end) end
    end,
}
ShowPosition = mirage.setting("speedo_position")
ShowSpeed = mirage.setting("speedo_speed")
ShowAngles = mirage.setting("speedo_angles")
ShowCheckpoint = mirage.setting("speedo_checkpoint")
ShowState = mirage.setting("speedo_state")
ShowJump = mirage.setting("speedo_jump")
Unit = mirage.setting("speedo_unit")
Radius = mirage.setting("speedo_radius")
CheckpointKey = mirage.setting("speedo_checkpoint_key")
TitleBar = mirage.setting("speedo_title_bar")
Opacity = mirage.setting("speedo_opacity")
ValueColumn = mirage.setting("speedo_value_column")
LabelColor = mirage.setting("speedo_label_color")
ValueColor = mirage.setting("speedo_value_color")

-- The trainer's key for "Set checkpoint here" (3)
mirage.on("key", function(vk)
    if speedo:enabled() and vk ~= 0 and vk == CheckpointKey:get() and not mirage.menu_open() then set_checkpoint() end
end)
