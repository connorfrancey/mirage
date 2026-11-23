-- Visual: the menu's simple visual features, as a mod using only the public API.
--
--   FOV         the player camera's locked field of view (the engine's own: zoom included), put back when off
--   Crosshair   a cross, dot or circle in the middle of the screen
--   Show FPS    the frame rate in a corner
--   Watermark   the menu's name in the bottom-left corner (Misc tab)
--   Frame Rate Limit   the game's 62 fps cap (GameEngine.MaxSmoothedFrameRate) set to another value, or off; switched
--                      off, the engine gets back exactly what it had. Movement depends on the frame rate: runs at
--                      another cap play differently, speedruns are timed at 62. A TAS session ignores it.
--   Clear Frame        the console's "set Engine bEnableColorClear true": the frame is cleared before the world is
--                      drawn, so nothing smears out of bounds ("hall of mirrors"). On the engine and its class default.

local later = require("later")     -- Game-thread work from on_disable

-- ---- FOV ------------------------------------------------------------------------------------------------------------
local camera, saved = nil, nil      -- The camera it changed, and that camera's own values
local fov
fov = mirage.module {
    name = "FOV",
    tab = "Visual",
    description = "Override the field of view",
    order = 10,
    on_tick = function()
        if not fov:enabled() then return end
        local pc = mirage.controller()
        local cam = pc and pc.PlayerCamera
        if not cam then return end
        if camera == nil or camera ~= cam then     -- First tick, or a new camera (level change, respawn)
            camera = cam
            saved = { locked = cam.bLockedFOV, locked_fov = cam.LockedFOV, default = cam.DefaultFOV }
        end
        local degrees = Degrees:get()
        cam.bLockedFOV = true
        cam.LockedFOV = degrees
        cam.DefaultFOV = degrees
    end,
    on_disable = function()
        later.run(function()
            local pc = mirage.controller()
            local cam = pc and pc.PlayerCamera
            if cam and camera and cam == camera and saved then
                cam.bLockedFOV = saved.locked
                cam.LockedFOV = saved.locked_fov
                cam.DefaultFOV = saved.default
            end
            camera, saved = nil, nil
        end)
    end,
}
Degrees = mirage.setting("fov")

-- ---- Crosshair --------------------------------------------------------------------------------------------------------
mirage.module {
    name = "Crosshair",
    tab = "Visual",
    description = "Draw a crosshair in the screen center",
    order = 30,
    on_render = function()
        local w, h = mirage.draw.screen_size()
        local cx, cy = w * 0.5, h * 0.5
        local s, g, th, col = Size:get(), Gap:get(), Thickness:get(), Color:get()
        local style = Style:get()
        if style == "Cross" then
            mirage.draw.line(cx - g - s, cy, cx - g, cy, col, th)
            mirage.draw.line(cx + g, cy, cx + g + s, cy, col, th)
            mirage.draw.line(cx, cy - g - s, cx, cy - g, col, th)
            mirage.draw.line(cx, cy + g, cx, cy + g + s, col, th)
        elseif style == "Dot" then
            mirage.draw.circle_filled(cx, cy, s * 0.25, col)
        else
            mirage.draw.circle(cx, cy, s, col, th)
        end
    end,
}
Style = mirage.setting("crosshair_style")
Size = mirage.setting("crosshair_size")
Gap = mirage.setting("crosshair_gap")
Thickness = mirage.setting("crosshair_thickness")
Color = mirage.setting("crosshair_color")

-- ---- Show FPS ---------------------------------------------------------------------------------------------------------
mirage.module {
    name = "Show FPS",
    tab = "Visual",
    description = "FPS counter",
    order = 20,
    enabled_by_default = true,
    on_render = function()
        local fps = mirage.frame_rate()
        if fps <= 0 then return end
        local text = Frametime:get() and string.format("%.0f FPS (%.2f ms)", fps, 1000 / fps) or string.format("%.0f FPS", fps)
        local tw, th = mirage.draw.text_size(text)
        local w, h = mirage.draw.screen_size()
        local corner = Corner:get()
        local x = corner:find("right") and w - tw - 10 or 10
        local y = corner:find("Bottom") and h - th - 10 or 10
        mirage.draw.text(x + 1, y + 1, mirage.rgba(0, 0, 0, 200), text)
        mirage.draw.text(x, y, TextColor:get(), text)
    end,
}
Corner = mirage.setting("fps_corner")
TextColor = mirage.setting("fps_color")
Frametime = mirage.setting("fps_frametime")

-- ---- Watermark --------------------------------------------------------------------------------------------------------
mirage.module {
    name = "Watermark",
    tab = "Misc",
    description = "Small text in the bottom-left corner",
    order = 10,
    on_render = function()
        local text = "Mirror's Edge Menu"
        local _, th = mirage.draw.text_size(text)
        local _, h = mirage.draw.screen_size()
        mirage.draw.text(10, h - th - 10, mirage.rgba(230, 50, 45, math.floor(Opacity:get() * 255)), text)
    end,
}
Opacity = mirage.setting("watermark_opacity")

-- ---- Frame Rate Limit ---------------------------------------------------------------------------------------------------
-- The game engine object lives as long as the game: found once
local engine
local function game_engine()
    if engine == nil then engine = mirage.all("GameEngine")[1] end
    return engine
end

local frame_cap = nil       -- What the engine had when the module took over: put back when it is switched off
local frame_rate
frame_rate = mirage.module {
    name = "Frame Rate Limit",
    tab = "Visual",
    description = "Change the game's 62 fps cap. Off: the game's own 62. Speedruns are at 62: movement depends on it",
    order = 15,
    on_tick = function()
        if not frame_rate:enabled() then return end
        local e = game_engine()
        if not e then return end
        if not frame_cap then
            frame_cap = { smooth = e.bSmoothFrameRate, min = e.MinSmoothedFrameRate, max = e.MaxSmoothedFrameRate }
        end
        local smooth = not Uncapped:get()
        if e.bSmoothFrameRate ~= smooth then e.bSmoothFrameRate = smooth end
        local max = Cap:get()
        if smooth and math.abs(e.MaxSmoothedFrameRate - max) > 0.01 then
            e.MaxSmoothedFrameRate = max
            if e.MinSmoothedFrameRate > max then e.MinSmoothedFrameRate = max end
        end
    end,
    on_panel = function()
        local ui = mirage.ui
        Cap:set_visible(not Uncapped:get())
        if ui.button("Back to 62", ui.content_width(), 0) then
            Cap:set(62)
            Uncapped:set(false)
        end
        if Uncapped:get() or math.abs(Cap:get() - 62) > 0.01 then
            ui.color_push(ui.COLOR_TEXT, mirage.rgba(255, 191, 77))
            ui.text_wrapped("Not 62: the game moves differently at other frame rates. Not for speedruns")
            ui.color_pop(1)
        end
    end,
    on_disable = function()
        later.run(function()
            local e = game_engine()
            if e and frame_cap then
                e.bSmoothFrameRate = frame_cap.smooth
                e.MinSmoothedFrameRate = frame_cap.min
                e.MaxSmoothedFrameRate = frame_cap.max
                mirage.info(string.format("Frame Rate Limit: off, the game's cap is back (%s, %.0f fps)",
                                          frame_cap.smooth and "on" or "off", frame_cap.max))
            end
            frame_cap = nil
        end)
    end,
}
Cap = mirage.setting("frame_rate_cap")
Uncapped = mirage.setting("frame_rate_uncapped")

-- ---- Clear Frame ----------------------------------------------------------------------------------------------------------
local cleared = nil         -- { { engine = ..., was = ... } }: the engine objects (class default too), as they were
local clear_frame
clear_frame = mirage.module {
    name = "Clear Frame",
    tab = "Visual",
    description = "Clear the screen every frame: no repeating draws out of bounds",
    order = 30,
    on_tick = function()
        if not clear_frame:enabled() then return end
        if not cleared then
            cleared = {}
            for _, e in ipairs(mirage.all("Engine", true)) do
                cleared[#cleared + 1] = { engine = e, was = e.bEnableColorClear }
            end
            mirage.info(string.format("Clear Frame: %d engine object(s)", #cleared))
        end
        for _, c in ipairs(cleared) do c.engine.bEnableColorClear = true end
    end,
    on_disable = function()
        later.run(function()
            for _, c in ipairs(cleared or {}) do c.engine.bEnableColorClear = c.was end
            cleared = nil
        end)
    end,
}
