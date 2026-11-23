-- Misc: Keep Key Bindings.
--
-- The game keeps one key per action when it starts: it puts back the keys of its profile (primary and alternate packed
-- in one value per action) and only one survives, so an alternate key set in the game's controls is gone the next
-- time the game runs or a level loads. This keeps every key the player bound to one of the game's actions (the GBA_
-- commands) and puts back the missing ones whenever the game resets them.
--
-- A reset comes with a new PlayerInput (game start, level load): for 15 seconds after one, keys that went missing are
-- put back. A change after that is the player's own (the controls menu) and becomes what is kept. Kept in this mod's
-- config.toml ("bindings": key=command;...). The self-test hands it a list of its own: mirage.shared("mirage",
-- "keep_bindings.test"), used instead of the config and never written.

local GRACE = 15                    -- Seconds after a reset
local CHECK_EVERY = 60              -- Ticks between looks at the list while the game may be resetting it (reading
                                    -- ~280 bindings costs milliseconds); after that only when SetBind ran
local dirty = true                  -- SetBind ran (the controls menu): look now

local input_seen, grace_until = nil, 0
local loaded, has_stored = false, false
local stored, last = {}, nil
local ticks = 0
local test_list = ""

local function encode(set)
    local parts = {}
    for b in pairs(set) do parts[#parts + 1] = b end
    table.sort(parts)
    return table.concat(parts, ";")
end
local function decode(text)
    local set = {}
    for b in text:gmatch("[^;]+") do
        if b:find("=", 1, true) then set[b] = true end
    end
    return set
end
local function same(a, b)
    for k in pairs(a) do if not b[k] then return false end end
    for k in pairs(b) do if not a[k] then return false end end
    return true
end

-- Keyboard and mouse keys bound to one of the game's actions, as "Key=Command"; and every key bound to anything
local function current(input)
    local set, used = {}, {}
    local binds = input.Bindings
    for i = 1, #binds do
        local b = binds[i]
        local cmd = b.Command or ""
        if cmd ~= "" then
            local key = b.Name or ""
            if not (b.Control or b.Shift or b.Alt) then
                used[key] = true
                if cmd:find("^GBA_") and not key:find("^GBA_") and not key:find("^XboxTypeS_") then
                    set[key .. "=" .. cmd] = true
                end
            end
        end
    end
    return set, used
end

local function read_stored()
    test_list = mirage.shared("mirage", "keep_bindings.test")
    if test_list ~= "" then return decode(test_list), true end
    local text = mirage.config.get("bindings")
    if text == nil then return {}, false end
    local set = decode(text)
    local n = 0
    for _ in pairs(set) do n = n + 1 end
    mirage.info(n .. " kept bindings")
    return set, true
end

local function write_stored(set)
    if test_list ~= "" then return true end
    mirage.config.set("bindings", encode(set))
    return true
end

-- Puts back the kept bindings the game dropped: never onto a key the player uses for something else
local function restore(input, cur, used)
    local n = 0
    for b in pairs(stored) do
        local key, cmd = b:match("^(.-)=(.*)$")
        if key and not cur[b] and not used[key] then
            mirage.call(input, "SetBind", { BindName = key, Command = cmd })     -- Saves TdInput.ini, like the menu
            mirage.info("put back " .. key .. " = " .. cmd)
            n = n + 1
        end
    end
    return n > 0
end

local keep
keep = mirage.module {
    name = "Keep Key Bindings",
    tab = "Misc",
    description = "Keeps the alternate keys you set in the game's controls between sessions",
    order = 30,
    enabled_by_default = true,
    on_enable = function()
        input_seen, loaded = nil, false        -- The next look is a reset moment: what is missing comes back
        ticks = CHECK_EVERY - 1
    end,
    on_tick = function()
        if not keep:enabled() then return end
        ticks = ticks + 1
        local pc, world = mirage.controller(), mirage.world()
        local input = pc and pc.PlayerInput
        if not input or not world then return end
        local resetting = input ~= input_seen or world.RealTimeSeconds < grace_until
        if not dirty and not (resetting and ticks >= CHECK_EVERY) then return end
        ticks, dirty = 0, false
        if not loaded then
            loaded = true
            stored, has_stored = read_stored()
        end
        local now = world.RealTimeSeconds
        if input_seen == nil or input ~= input_seen then   -- A new PlayerInput: the game (re)applied its bindings
            input_seen = input
            grace_until = now + GRACE
        end
        local cur, used = current(input)
        if now < grace_until then
            if has_stored and restore(input, cur, used) then cur = current(input) end
        elseif not has_stored or not last or not same(cur, last) then
            if has_stored and not same(cur, stored) then mirage.info("the player changed the bindings: keeping them") end
            stored = cur
            has_stored = write_stored(stored)
        end
        last = cur
    end,
}

-- The controls menu binds through SetBind: a change is looked at on the next tick (restores call it too: harmless)
mirage.hook("Engine.PlayerInput.SetBind", function() dirty = true end, { when = "after" })
