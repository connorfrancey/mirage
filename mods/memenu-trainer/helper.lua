-- Kickglitch Helper: timing practice for the kickglitch. Kick during a wallrun, jump on the frame you land; the helper
-- sees each attempt by itself and says how many frames from perfect the jump came:
--
--   kick (MOVE_MeleeWallrun) -> lands (physics walking) -> jump (MOVE_Jump), counted in engine ticks (= frames)
--
-- "Perfect" is the frame the Kickglitch tool's own jump lands on: calibrated by one attempt with the tool armed, 1 to
-- start with. Jump presses before the landing (which the game ignores during the kick) are seen through key events on
-- the keys bound to jump (keyboard keys: a jump on a mouse button isn't seen early).
--
-- Shares what it measured for the tests: kg_helper.attempts, .jump_frame (-1: no jump), .calibrated.

local flight = require("flight")
local ui = mirage.ui

local PHYS_Walking, PHYS_Falling = 1, 2
local MOVE = { Jump = 11, WallRunJump = 12, Grabbing = 3, WallRunningLeft = 5, WallRunningRight = 4, WallClimbing = 6,
               IntoGrab = 14, ZipLine = 28, FallingUncontrolled = 72, MeleeWallrun = 62 }
local OTHER = { [MOVE.Jump] = true, [MOVE.WallRunJump] = true, [MOVE.Grabbing] = true, [MOVE.WallRunningLeft] = true,
                [MOVE.WallRunningRight] = true, [MOVE.WallClimbing] = true, [MOVE.IntoGrab] = true, [MOVE.ZipLine] = true,
                [MOVE.FallingUncontrolled] = true }
local LATE_LIMIT, BAR_FRAMES = 20, 4

local M = {}
local tick, keys_at = 0, -1000000
local attempt = { phase = "idle" }
local last_speed, tool_was_armed = 0, false
local jump_vks = {}                 -- Virtual-key codes bound to jump
local presses = 0                   -- Key presses of them (key events)
local seen_presses = 0
local shown = { attempts = 0, perfect = 0, streak = 0, best = 0, last = nil, text = "", detail = "", history = {} }
local flash_pending, flash_at = false, -1
local last = { attempts = 0, jump_frame = -1, calibrated = false }

-- Engine key names to virtual-key codes (keyboard keys)
local NAMED = { spacebar = 0x20, leftshift = 0xA0, rightshift = 0xA1, leftcontrol = 0xA2, rightcontrol = 0xA3,
                leftalt = 0xA4, rightalt = 0xA5, enter = 0x0D, tab = 0x09, capslock = 0x14, backspace = 0x08 }
local function vk_of(name)
    name = name:lower()
    if NAMED[name] then return NAMED[name] end
    if #name == 1 and name:match("[a-z0-9]") then return string.byte(name:upper()) end
    return nil
end

function M.tool_armed() return M.kickglitch and M.kickglitch:enabled() end

local function find_jump_keys(pc)
    keys_at = tick
    jump_vks = {}
    local input = pc.PlayerInput
    if not input then return end
    local binds = input.Bindings
    for i = 1, #binds do
        local b = binds[i]
        local name, command = b.Name or "", b.Command or ""
        if not name:find("^GBA_") and command:find("^GBA_Jump") then
            local vk = vk_of(name)
            if vk then jump_vks[vk] = true end
        end
    end
end

mirage.on("key", function(vk)
    if jump_vks[vk] then presses = presses + 1 end
end)

local function watch_presses()
    if attempt.first_press then return end
    if presses > seen_presses then attempt.first_press = tick end
    seen_presses = presses
end

local function speed_of(pawn)
    local v = pawn.Velocity
    return math.sqrt(v.X * v.X + v.Y * v.Y)
end

local function finish(pawn)
    local a = attempt
    attempt = { phase = "idle" }
    local speed_after = speed_of(pawn)
    if a.calibrating and a.jump_tick then
        local frame = a.jump_tick - a.land_tick
        PerfectFrame:set(math.max(0, math.min(5, frame)))
        mirage.notify("Kickglitch Helper: perfect is the jump " .. frame .. " frame(s) after landing")
    end
    last.attempts = last.attempts + 1
    last.jump_frame = a.jump_tick and (a.jump_tick - a.land_tick) or -1
    last.calibrated = a.calibrating and a.jump_tick ~= nil
    mirage.share("kg_helper.attempts", last.attempts)
    mirage.share("kg_helper.jump_frame", last.jump_frame)
    mirage.share("kg_helper.calibrated", last.calibrated and 1 or 0)

    local s = shown
    s.attempts = s.attempts + 1
    if not a.jump_tick then
        s.last, s.text = "nojump", "No jump"
        if a.first_press and a.first_press <= a.land_tick then
            local early = a.land_tick - a.first_press
            s.detail = string.format("pressed %d frame%s before landing", early, early == 1 and "" or "s")
        else
            s.detail = "within " .. LATE_LIMIT .. " frames of landing"
        end
        s.streak = 0
        return
    end
    local offset = (a.jump_tick - a.land_tick) - PerfectFrame:get()
    s.history[#s.history + 1] = offset
    while #s.history > 12 do table.remove(s.history, 1) end
    if offset == 0 then
        s.last, s.text = "perfect", "PERFECT"
        s.perfect = s.perfect + 1
        s.streak = s.streak + 1
        s.best = math.max(s.best, s.streak)
    else
        local n = math.abs(offset)
        s.last = n == 1 and "close" or "off"
        s.text = n .. (n == 1 and " frame " or " frames ") .. (offset < 0 and "early" or "late")
        s.streak = 0
    end
    s.detail = string.format("speed %.0f -> %.0f u/s", a.speed_before, speed_after)
end

local function landed()
    attempt.land_tick = tick
    attempt.phase = "landed"
    flash_pending = true
end

local helper
helper = mirage.module {
    name = "Kickglitch Helper",
    tab = "Trainer",
    description = "Times your kickglitches: how many frames from the perfect jump you were",
    order = 11,
    every_tick = true,          -- Must see every tick: the window is one frame
    on_enable = function()
        last = { attempts = 0, jump_frame = -1, calibrated = false }
        mirage.share("kg_helper.attempts", 0)
        attempt = { phase = "idle" }
        keys_at = -1000000
    end,
    on_tick = function()
        if not helper:enabled() then return end
        tick = tick + 1
        local pawn, pc = mirage.pawn(), mirage.controller()
        if not pawn or not pc or pawn.Health <= 0 or flight.active() then
            attempt = { phase = "idle" }
            return
        end
        if tick - keys_at > 300 then find_jump_keys(pc) end     -- Bindings can change in the options
        local move, physics, speed = pawn.MovementState, pawn.Physics, speed_of(pawn)
        local armed = M.tool_armed()

        if attempt.phase == "idle" then
            if move == MOVE.MeleeWallrun then
                attempt = { phase = "kicking", kick_tick = tick, speed_before = last_speed }
                if physics == PHYS_Walking then landed() end        -- Kicked right above the ground
                attempt.calibrating = armed or tool_was_armed
                seen_presses = presses
            end
        elseif attempt.phase == "kicking" then
            watch_presses()
            attempt.calibrating = attempt.calibrating or armed or tool_was_armed
            if physics == PHYS_Walking then landed()
            elseif tick - attempt.kick_tick > 240 or OTHER[move] then attempt = { phase = "idle" } end
        elseif attempt.phase == "landed" then
            watch_presses()
            attempt.calibrating = attempt.calibrating or armed or tool_was_armed
            -- The jump move, or back in the air going up (the jump impulse)
            if move == MOVE.Jump or (physics == PHYS_Falling and pawn.Velocity.Z > 100) then
                attempt.jump_tick = tick
                attempt.phase = "measuring"
            elseif tick - attempt.land_tick > LATE_LIMIT then
                finish(pawn)
            end
        elseif attempt.phase == "measuring" then        -- The speed the jump carries, a few frames in
            if tick - attempt.jump_tick >= 3 then finish(pawn) end
        end
        last_speed = speed
        tool_was_armed = armed      -- The tool switches itself off in the tick it jumps
    end,
    on_render = function()
        local s = shown
        local flags = ui.WINDOW_AUTO_SIZE + ui.WINDOW_NO_COLLAPSE + ui.WINDOW_NO_TITLE
        if not mirage.menu_open() then flags = flags + ui.WINDOW_NO_INPUTS end
        ui.window_next_pos(60, 320)
        ui.window_next_bg_alpha(Opacity:get())
        if ui.window_begin("Kickglitch Helper##trainer", flags) then
            ui.text_disabled("Kickglitch")
            if s.attempts == 0 then
                ui.text("Wallrun, kick, jump as you land")
            else
                local color = s.last == "perfect" and mirage.rgba(90, 230, 102) or s.last == "close"
                    and mirage.rgba(255, 191, 64) or mirage.rgba(255, 102, 89)
                ui.text_colored(color, s.text)
                if s.detail ~= "" then ui.text_disabled(s.detail) end
            end
            -- A row of cells, one per frame around perfect; older results fade, the newest is outlined
            local cell, gap, h = 18, 3, 14
            local x, y = ui.cursor_screen_pos()
            for i = 0, BAR_FRAMES * 2 do
                local offset = i - BAR_FRAMES
                local ax = x + i * (cell + gap)
                ui.draw_rect(ax, y, ax + cell, y + h, offset == 0 and mirage.rgba(60, 140, 70) or mirage.rgba(70, 70, 76), 2)
                local hits = 0
                for _, o in ipairs(s.history) do
                    if math.max(-BAR_FRAMES, math.min(BAR_FRAMES, o)) == offset then hits = hits + 1 end
                end
                if hits > 0 then
                    local alpha = math.min(255, 70 + hits * 50)
                    ui.draw_rect(ax, y, ax + cell, y + h,
                        offset == 0 and mirage.rgba(90, 230, 110, alpha) or mirage.rgba(240, 170, 60, alpha), 2)
                end
                local newest = s.history[#s.history]
                if newest and math.max(-BAR_FRAMES, math.min(BAR_FRAMES, newest)) == offset then
                    ui.draw_rect(ax - 1, y - 1, ax + cell + 1, y + h + 1, mirage.rgba(255, 255, 255), 2, 2)
                end
            end
            ui.dummy((BAR_FRAMES * 2 + 1) * (cell + gap) - gap, h)
            ui.text_disabled("early          perfect          late")
            if s.attempts > 0 then
                ui.text(string.format("%d / %d perfect (%.0f%%)  streak %d, best %d", s.perfect, s.attempts,
                    100 * s.perfect / s.attempts, s.streak, s.best))
            end
            -- Flash the window's border on the landing frame: when the jump should have come
            if flash_pending then
                flash_pending = false
                flash_at = ui.time()
            end
            if Flash:get() and flash_at >= 0 and ui.time() - flash_at < 0.12 then
                local x1, y1, x2, y2 = ui.window_rect()
                ui.overlay_rect(x1, y1, x2, y2, mirage.rgba(90, 200, 255), 4, 3)
            end
        end
        ui.window_end()
    end,
    on_panel = function()
        if ui.button("Reset stats", ui.content_width(), 0) then
            shown = { attempts = 0, perfect = 0, streak = 0, best = 0, last = nil, text = "", detail = "", history = {} }
        end
    end,
}
Flash = mirage.setting("kg_helper_flash")
PerfectFrame = mirage.setting("kg_helper_perfect")
Opacity = mirage.setting("kg_helper_opacity")

return M
