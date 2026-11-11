-- Visual: the menu's simple visual features, as a mod using only the public API.
--
--   FOV         the player camera's locked field of view (the engine's own: zoom included), put back when off
--   Crosshair   a cross, dot or circle in the middle of the screen
--   Show FPS    the frame rate in a corner
--   Watermark   the menu's name in the bottom-left corner (Misc tab)

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
