local vec = require("vec")
local draw = mirage.draw
local view = {}

local white = mirage.rgba(225, 242, 248)
local cyan = mirage.rgba(104, 224, 255)
local amber = mirage.rgba(255, 191, 94)
local idle = mirage.rgba(165, 185, 198, 155)
local dark = mirage.rgba(10, 18, 25, 210)
local shadow = mirage.rgba(22, 42, 54, 225)

function view.rope(hook, pos, camera)
    local hand = vec.add(pos, vec.new(0, 0, 45))
    local side = vec.new(1, 0, 0)
    if camera then
        local forward = vec.of(camera.forward)
        local yaw = camera.rotation.yaw * math.pi / 32768
        side = vec.new(-math.sin(yaw), math.cos(yaw), 0)
        local up = vec.cross(forward, side)
        hand = vec.add(vec.of(camera.location), vec.add(vec.scale(forward, 52),
                       vec.add(vec.scale(side, 21), vec.scale(up, -18))))
    end
    local progress = math.min(1, hook.age / 0.14)
    local tip = vec.lerp(hand, hook.anchor, 1 - (1 - progress) ^ 2)
    local sag = hook.taut and 0 or math.min(180, math.max(0, hook.length - hook.distance) * 0.45)
    local color = hook.taut and amber or cyan
    local offset = vec.scale(side, 1.8)
    local lines, previous = {}, hand
    for i = 1, 16 do
        local t = i / 16
        local point = vec.lerp(hand, tip, t)
        point.z = point.z - sag * progress * 4 * t * (1 - t)
        local bright = i == 1 + math.floor(hook.age * 48) % 16
        lines[#lines + 1] = { previous, point, bright and white or color }
        lines[#lines + 1] = { vec.add(previous, offset), vec.add(point, offset), shadow }
        previous = point
    end
    if progress == 1 then
        local normal = hook.normal or vec.norm(vec.sub(hand, hook.anchor))
        local axis = math.abs(normal.z) < 0.9 and vec.new(0, 0, 1) or vec.new(0, 1, 0)
        local right = vec.norm(vec.cross(axis, normal))
        local up = vec.cross(normal, right)
        local anchor = vec.add(hook.anchor, vec.scale(normal, 4))
        local radius = 11 + 17 * math.max(0, 1 - (hook.age - 0.14) / 0.28)
        local function edge(angle)
            return vec.add(anchor, vec.add(vec.scale(right, radius * math.cos(angle)),
                                           vec.scale(up, radius * math.sin(angle))))
        end
        for i = 0, 7 do
            lines[#lines + 1] = { edge(i * math.pi / 4), edge((i + 1) * math.pi / 4), color }
        end
        lines[#lines + 1] = { anchor, vec.add(anchor, vec.scale(normal, 16)), white }
    end
    mirage.draw3d.lines(lines)
end

function view.hud(aim, hook, mode, feedback)
    local w, h = draw.screen_size()
    local x, y = w * 0.5, h * 0.5
    local color = hook and (hook.taut and amber or cyan) or aim and cyan or idle
    local gap = hook and 12 or aim and 8 or 15
    if feedback then
        gap = gap + 8 * feedback.time
        if feedback.miss then color = amber end
    end
    for _, side in ipairs({ -1, 1 }) do
        local edge = x + side * gap
        draw.line(edge, y - 6, edge, y + 6, dark, 4)
        draw.line(edge, y - 6, edge, y + 6, color, 2)
        draw.line(edge, y, edge + side * 6, y, color, 2)
    end
    draw.circle_filled(x, y, 1.5, color)
    if aim and aim.assisted and not hook then
        local sx, sy = mirage.world_to_screen(aim.hit)
        if sx then draw.circle(sx, sy, 4, color, 1.5) end
    end
    if hook then
        draw.rect_filled(x + 21, y + 16, x + 151, y + 54, dark)
        draw.rect_filled(x + 21, y + 16, x + 23, y + 54, color)
        local state = hook.reel > 0 and "REEL IN" or hook.reel < 0 and "REEL OUT" or
            hook.taut and "TAUT" or "SLACK"
        draw.text(x + 29, y + 20, color, mode:upper() .. " / " .. state)
        draw.text(x + 29, y + 36, white, string.format("%.1f m cable", hook.length * 0.02))
    elseif aim or feedback then
        local text = aim and string.format("%s  %.1f m", mode:upper(), aim.distance * 0.02) or "NO ANCHOR"
        draw.rect_filled(x + 21, y + 16, x + 133, y + 38, dark)
        draw.text(x + 28, y + 19, color, text)
    end
end

return view
