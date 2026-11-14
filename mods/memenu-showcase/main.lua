-- The Mirage Showcase: a tour of what the loader lets a mod do, in one window (shown with the menu open while the module
-- is on). Every section is live and names the API it uses, so this file doubles as an example to copy:
--
--   Live objects    the player read by property path every tick (mirage.pawn, proxies)
--   Writing         the level's gravity changed and put back (WorldInfo.WorldGravityZ)
--   Events          level loads, deaths and move changes as they happen (mirage.on)
--   Hooks           a script function of the game watched (mirage.hook on TdPlayerPawn.Landed)
--   Injection       the game's jump code changed from inside its bytecode (mirage.inject)
--   Classes         a new actor class described in Lua, spawned in the level (mirage.define_class)
--   Settings, UI    mod.toml settings and the UI layer (mirage.setting, mirage.ui)
--
-- UObjects are only touched on the game thread: the window asks (flags) and the tick does it. For the self-test the
-- mod shares what it saw (mirage.share).
local ui = mirage.ui

local window   -- The window (below): drawn by the module's on_render

local module = mirage.module {
    name = "Mirage Showcase",
    tab = "Mods",
    description = "A tour of the mod loader: live objects, events, hooks, script injection, a class made in Lua",
    order = 0,
    on_render = function() if mirage.menu_open() then window() end end,
}
local LowGravity = mirage.setting("low_gravity")
local GravityScale = mirage.setting("gravity_scale")
local WeightedJumps = mirage.setting("weighted_jumps")
local PadStrength = mirage.setting("pad_strength")
local SpawnPad = mirage.setting("spawn_pad")

-- ---- State, written on the game thread and read by the window -------------------------------------------------------
local live = { ok = false }          -- The player, read every tick
local events = {}                    -- The last events, newest first
local landings, last_impact = 0, 0   -- From the Landed hook
local injected_runs = 0              -- How often the injection ran
local pads, bounces = 0, 0           -- Pads dropped, bounces on them
local dropped = {}                   -- The pads in the level (weak handles: a level load destroys them)
local gravity = { original = nil, applied = nil, world = nil }
local frames = 0
local clock = 0                      -- Game seconds since the mod loaded (the tick event's time step, summed)

local function note(text)
    table.insert(events, 1, string.format("%7.2f  %s", clock, text))
    if #events > 12 then events[#events] = nil end
end

local function kmh(v) return v * 0.036 end   -- Unreal units per second (cm/s) to km/h

-- ---- Events ---------------------------------------------------------------------------------------------------------
mirage.on("level_load", function(finished, map)
    if finished then note("level loaded: " .. tostring(map)) end
    gravity.world = nil     -- A new WorldInfo: the gravity is applied again to it
end)
mirage.on("death", function(after)
    if not after then note("the player died") end
end)
mirage.on("player_state", function(new, old)
    note(string.format("move %d -> %d", old, new))
end)

-- ---- Writing: the level's gravity -----------------------------------------------------------------------------------
local function update_gravity(world)
    local want = module:enabled() and LowGravity:get()
    if want then
        if gravity.world ~= world then      -- First time in this level: remember the level's own value
            gravity.world = world
            gravity.original = world.WorldGravityZ
            gravity.applied = nil
        end
        local value = gravity.original * GravityScale:get()
        if gravity.applied ~= value then
            world.WorldGravityZ = value
            gravity.applied = value
            note(string.format("gravity %.0f (the level's %.0f)", value, gravity.original))
        end
    elseif gravity.applied then             -- Off: the level's own value back
        if gravity.world == world then world.WorldGravityZ = gravity.original end
        note("gravity back to the level's")
        gravity.applied, gravity.world = nil, nil
    end
end

-- ---- Hooks: the game's TdPlayerPawn.Landed, before it runs -----------------------------------------------------------
local landed_hook
local function update_hook()
    if module:enabled() and not landed_hook then
        landed_hook = mirage.hook("TdGame.TdPlayerPawn.Landed", function(call)
            landings = landings + 1
            local pawn = mirage.pawn()
            if pawn then last_impact = -pawn.Velocity.Z end   -- Still the falling speed: Landed runs before it's zeroed
        end, { when = "before" })
    elseif not module:enabled() and landed_hook then
        landed_hook:remove()
        landed_hook = nil
    end
end

-- ---- Injection: inside TdMove_Jump.StartMove, before it asks GetMobilityMultiplier, the local CurrentWeaponType set
-- to 1 (heavy): the game then picks BaseJumpZHeavy. Nothing else in the function changes. -------------------------------
local injection
local function update_injection()
    local want = module:enabled() and WeightedJumps:get()
    if want and not injection then
        injection = mirage.inject("TdGame.TdMove_Jump.StartMove", "before:GetMobilityMultiplier", function(call)
            call.params.CurrentWeaponType = 1
            injected_runs = injected_runs + 1
        end)
        note("weighted jumps injected into TdMove_Jump.StartMove")
    elseif not want and injection then
        injection:remove()
        injection = nil
        note("TdMove_Jump.StartMove back to its own bytecode")
    end
end

-- ---- Classes: a bounce pad described in Lua. The runtime writes its UnrealScript, compiles it in the game and runs
-- this Touch from inside the class's bytecode. -------------------------------------------------------------------------
local Pad
local function pad_class()
    if Pad then return Pad end
    Pad = mirage.define_class {
        name = "MirageShowcasePad", extends = "Trigger", placeable = true,
        vars = { "var() float Strength", "int Bounces" },
        defaults = { Strength = 1500.0, bNoDelete = false },
        functions = {
            Touch = function(self, params)
                local other = params.Other
                if not other or not mirage.is_a(other, "Pawn") then return end
                self.Bounces = self.Bounces + 1
                other.Velocity.Z = self.Strength
                mirage.call(other, "SetPhysics", { newPhysics = 2 })   -- PHYS_Falling
                bounces = bounces + 1
            end,
        },
    }
    return Pad
end

local function drop_pad(pawn)
    local cls = pad_class()
    if not cls then return end
    local at = pawn.Location
    local pad = mirage.call(pawn, "Spawn", {
        SpawnClass = cls, bNoCollisionFail = true,
        ["SpawnLocation.X"] = at.X, ["SpawnLocation.Y"] = at.Y, ["SpawnLocation.Z"] = at.Z,
    })
    if pad then
        dropped[#dropped + 1] = pad
        pad.Strength = PadStrength:get()
        pads = pads + 1
        note(string.format("dropped a pad (strength %.0f)", PadStrength:get()))
        -- A nudge: a move makes the engine see what the pawn now overlaps
        mirage.call(pawn, "SetLocation", { ["NewLocation.X"] = at.X, ["NewLocation.Y"] = at.Y, ["NewLocation.Z"] = at.Z + 1 })
    end
end

-- ---- The tick: game thread ------------------------------------------------------------------------------------------
mirage.on("tick", function(seconds)
    clock = clock + seconds
    update_hook()
    update_injection()
    local pawn, world = mirage.pawn(), mirage.world()
    if world then update_gravity(world) end
    if not module:enabled() then
        live.ok = false
        -- Switched off: the pads go too (a pad left under the player would keep launching them)
        for _, pad in ipairs(dropped) do pcall(mirage.call, pad, "Destroy") end   -- (one a level load took is gone)
        dropped = {}
        return
    end
    if pawn then
        local v = pawn.Velocity
        live = {
            ok = true, class = mirage.class(pawn), x = pawn.Location.X, y = pawn.Location.Y, z = pawn.Location.Z,
            speed = math.sqrt(v.X * v.X + v.Y * v.Y), vz = v.Z, move = pawn.MovementState, health = pawn.Health,
        }
        if SpawnPad:get() then
            SpawnPad:set(false)
            drop_pad(pawn)
        end
    else
        live.ok = false
    end
    -- For the self-test
    mirage.share("pawn_z", live.ok and live.z or 0)
    mirage.share("landings", landings)
    mirage.share("injected_runs", injected_runs)
    mirage.share("pads", pads)
    mirage.share("bounces", bounces)
    mirage.share("gravity", gravity.applied or 0)
    mirage.share("events", #events)
end)

-- ---- The window: render thread, with the menu open --------------------------------------------------------------------
local function section(title, api)
    local open = ui.collapsing_header(title, true)
    if open then ui.text_disabled(api) end
    return open
end

function window()
    ui.window_next_size(430, 560)
    local shown = ui.window_begin("Mirage Showcase")
    if shown then
        ui.text_wrapped("Everything here is a Lua mod (mods/memenu-showcase) using the public API only.")
        ui.separator()

        if section("Live objects", "mirage.pawn(), pawn.Location, pawn.Velocity: properties by name") then
            if live.ok then
                ui.label_text("Class", live.class)
                ui.label_text("Location", string.format("%.0f  %.0f  %.0f", live.x, live.y, live.z))
                ui.label_text("Speed", string.format("%.1f km/h, vertical %.1f", kmh(live.speed), kmh(live.vz)))
                ui.label_text("Move state", tostring(live.move))
                ui.label_text("Health", tostring(live.health))
            else
                ui.text_disabled("no player (loading, or a menu)")
            end
        end

        if section("Writing the game", "world.WorldGravityZ = ...: undone when switched off") then
            local on_changed, on = ui.checkbox("Low gravity", LowGravity:get())
            if on_changed then LowGravity:set(on) end
            local changed, scale = ui.slider_float("Gravity scale", GravityScale:get(), 0.1, 1.0, "%.2f")
            if changed then GravityScale:set(scale) end
            if gravity.applied then ui.text(string.format("gravity %.0f, the level's %.0f", gravity.applied, gravity.original)) end
        end

        if section("Hooks", "mirage.hook(\"TdGame.TdPlayerPawn.Landed\", ...)") then
            ui.label_text("Landings", tostring(landings))
            ui.label_text("Last impact", string.format("%.1f km/h", kmh(last_impact)))
        end

        if section("Script injection", "mirage.inject(\"TdGame.TdMove_Jump.StartMove\", \"before:GetMobilityMultiplier\", ...)") then
            local changed, on = ui.checkbox("Weighted jumps", WeightedJumps:get())
            if changed then WeightedJumps:set(on) end
            ui.label_text("Ran", tostring(injected_runs))
        end

        if section("A class made in Lua", "mirage.define_class{ name = \"MirageShowcasePad\", extends = \"Trigger\", ... }") then
            local changed, s = ui.slider_float("Pad strength", PadStrength:get(), 600, 4000, "%.0f")
            if changed then PadStrength:set(s) end
            if ui.button("Drop a pad where I stand") then SpawnPad:set(true) end
            ui.same_line()
            ui.text(string.format("%d dropped, %d bounces", pads, bounces))
        end

        if section("Events", "mirage.on(\"level_load\" / \"death\" / \"player_state\", ...)") then
            if #events == 0 then ui.text_disabled("nothing yet: jump, or load a level") end
            for _, e in ipairs(events) do ui.text(e) end
        end
    end
    ui.window_end()
    frames = frames + 1
    mirage.share("frames", frames)
end
