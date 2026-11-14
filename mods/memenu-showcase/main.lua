-- The Mirage Showcase: a tour of what the loader lets a mod do, in one window (shown with the menu open while the module
-- is on). Every section is live and names the API it uses, so this file doubles as an example to copy:
--
--   Live objects    the player read by property path every tick (mirage.pawn, proxies)
--   Writing         the level's gravity changed and put back (WorldInfo.WorldGravityZ)
--   Events          level loads, deaths and move changes as they happen (mirage.on)
--   Hooks           a script function of the game watched (mirage.hook on TdPlayerPawn.Landed)
--   Injection       the game's jump code changed from inside its bytecode (mirage.inject)
--   Classes         a new actor class described in Lua, spawned in the level (mirage.define_class)
--   The world       the actors around the player, nearest first (mirage.actors); actors coming and going
--                   (actor_added / actor_removed), restarts, streamed levels; objects that stay safe to keep (mirage.valid)
--   Settings, UI    mod.toml settings and the UI layer (mirage.setting, mirage.ui)
--
-- UObjects are only touched on the game thread: the window asks (flags) and the tick does it. For the self-test the
-- mod shares what it saw (mirage.share).
local ui = mirage.ui

local window   -- The window (below): drawn by the module's on_render

local module = mirage.module {
    name = "Mirage Showcase",
    tab = "Mods",
    description = "A tour of the mod loader: live objects, the world around you, events, hooks, script injection, a class made in Lua",
    order = 0,
    on_render = function() if mirage.menu_open() then window() end end,
}
local LowGravity = mirage.setting("low_gravity")
local GravityScale = mirage.setting("gravity_scale")
local WeightedJumps = mirage.setting("weighted_jumps")
local PadStrength = mirage.setting("pad_strength")
local SpawnPad = mirage.setting("spawn_pad")
local WatchActors = mirage.setting("watch_actors")

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
local nearby = {}                    -- The actors around the player: { name, class, distance }, nearest first
local nearby_total = 0
local actor_log = {}                 -- Actors that came and went, newest first (while Watch actors is on)
local actors_added, actors_removed = 0, 0
local pads_alive = 0                 -- Dropped pads still in the level (counted on the game thread)

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
mirage.on("restart", function() note("checkpoint restart: a new player") end)
mirage.on("level_streamed", function(visible, level) note((visible and "streamed in: " or "streamed out: ") .. level) end)

-- ---- The world: actors coming and going. Only subscribed while Watch actors is on: while anyone listens, the runtime
-- compares the levels' actor lists every tick. ----------------------------------------------------------------------------
local actor_subs = {}
local function log_actor(text)
    table.insert(actor_log, 1, string.format("%7.2f  %s", clock, text))
    if #actor_log > 8 then actor_log[#actor_log] = nil end
end
local function update_watch()
    local want = module:enabled() and WatchActors:get()
    if want and #actor_subs == 0 then
        actor_subs[1] = mirage.on("actor_added", function(actor, class)
            actors_added = actors_added + 1
            log_actor("+ " .. class .. " " .. mirage.name(actor))
        end)
        actor_subs[2] = mirage.on("actor_removed", function(actor)
            actors_removed = actors_removed + 1
            -- The actor is gone: it can't be read any more, but it is == to one kept (a pad of ours, here)
            local ours = false
            for _, pad in ipairs(dropped) do if pad == actor then ours = true end end
            log_actor(ours and "- one of our pads" or "- an actor")
        end)
    elseif not want and #actor_subs > 0 then
        for _, sub in ipairs(actor_subs) do sub:remove() end
        actor_subs = {}
    end
end

local function update_nearby(pawn)
    local found = mirage.actors { near = pawn, radius = 3000 }     -- Nearest first; the player itself is the first
    nearby_total = #found - 1
    nearby = {}
    for i = 2, math.min(#found, 9) do
        local a = found[i]
        local d = a.Location
        local dx, dy, dz = d.X - pawn.Location.X, d.Y - pawn.Location.Y, d.Z - pawn.Location.Z
        nearby[#nearby + 1] = { name = mirage.name(a), class = mirage.class(a), distance = math.sqrt(dx * dx + dy * dy + dz * dz) }
    end
end

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
    update_watch()
    local pawn, world = mirage.pawn(), mirage.world()
    if world then update_gravity(world) end
    if not module:enabled() then
        live.ok = false
        -- Switched off: the pads go too (a pad left under the player would keep launching them). One a level load took
        -- is no longer valid: kept objects are handles, never another object at the same address.
        for _, pad in ipairs(dropped) do
            if mirage.valid(pad) then mirage.call(pad, "Destroy") end
        end
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
        if math.floor(clock * 4) ~= math.floor((clock - seconds) * 4) then update_nearby(pawn) end   -- 4 times a second
        pads_alive = 0
        for _, pad in ipairs(dropped) do if mirage.valid(pad) then pads_alive = pads_alive + 1 end end
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
    mirage.share("nearby", nearby_total)
    mirage.share("actors_added", actors_added)
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
            ui.text_disabled(string.format("%d still in the level (mirage.valid: a level load takes them)", pads_alive))
        end

        if section("The world", "mirage.actors{ near = pawn, radius = 3000 }; mirage.on(\"actor_added\" / \"actor_removed\")") then
            ui.text(string.format("%d actors within 30 m; the nearest:", nearby_total))
            if ui.table_begin("nearby", 3, ui.TABLE_BORDERS + ui.TABLE_ROW_BG) then
                ui.table_column("Class", ui.COLUMN_STRETCH)
                ui.table_column("Name", ui.COLUMN_STRETCH)
                ui.table_column("m", ui.COLUMN_FIXED, 50)
                ui.table_headers()
                for _, a in ipairs(nearby) do
                    ui.table_next_row()
                    ui.table_next_column()
                    ui.text(a.class)
                    ui.table_next_column()
                    ui.text(a.name)
                    ui.table_next_column()
                    ui.text(string.format("%.1f", a.distance / 100))
                end
                ui.table_end()
            end
            local changed, on = ui.checkbox("Watch actors come and go", WatchActors:get())
            if changed then WatchActors:set(on) end
            if WatchActors:get() then
                ui.text(string.format("%d added, %d removed", actors_added, actors_removed))
                for _, line in ipairs(actor_log) do ui.text(line) end
            end
        end

        if section("Events", "mirage.on(\"level_load\" / \"death\" / \"player_state\" / \"restart\" / \"level_streamed\", ...)") then
            if #events == 0 then ui.text_disabled("nothing yet: jump, or load a level") end
            for _, e in ipairs(events) do ui.text(e) end
        end
    end
    ui.window_end()
    frames = frames + 1
    mirage.share("frames", frames)
end
