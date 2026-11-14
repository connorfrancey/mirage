-- Levels (the level loader) and Sublevels.
--
--   Levels      the levels in story order with their checkpoints: click a level to open it, then a checkpoint to load
--               it (A is the level start; a level without checkpoints has "Start"). The level you are in is marked
--               and open. Maps and checkpoints come from the game's own level-select data (UIDataProvider_TdMaps);
--               loads use the same StartGame its menu does. Run: read the lists again.
--   Sublevels   the level's streaming sublevels: whether each is loaded and shown. Ticking Loaded loads and shows it
--               (unticking unloads it); Visible only shows or hides it.
--
-- The lists are read on the game thread (now and then, and on Run) and drawn from the copy on the render thread.

local later = require("later")
local ui = mirage.ui

local M = {}

-- Story levels, until the game's own list is there (names from mmultiplayer's util.h)
local BUILTIN = {
    { "tdmainmenu", "Main menu" }, { "edge_p", "Prologue" }, { "escape_p", "Flight" }, { "stromdrain_p", "Jacknife" },
    { "cranes_p", "Heat" }, { "subway_p", "Ropeburn" }, { "mall_p", "New Eden" }, { "factory_p", "Factory" },
    { "boat_p", "The Boat" }, { "convoy_p", "Kate" }, { "scraper_p", "The Shard" },
}

local lists = { maps = {}, have_game_maps = false, current = "", sublevels = {} }

local function builtin_maps()
    local maps = {}
    for _, b in ipairs(BUILTIN) do
        maps[#maps + 1] = { file = b[1], label = b[2] .. " (" .. b[1] .. ")", mode = "", checkpoints = {} }
    end
    return maps
end
lists.maps = builtin_maps()

-- ---- Reading the lists (game thread) -----------------------------------------------------------------------------------
local function refresh_maps()
    local maps, seen = {}, {}
    for _, p in ipairs(mirage.all("UIDataProvider_TdMaps")) do
        local file, title = p.Filename or "", p.MapName or ""
        if file ~= "" and not seen[file .. "|" .. title] then
            seen[file .. "|" .. title] = true
            local map = { file = file, label = title == "" and file or title .. " (" .. file .. ")", mode = p.GameMode or "",
                          checkpoints = {} }
            local cps = p.Checkpoints
            for i = 1, #cps do
                local cp = cps[i]
                local name, friendly = cp.CheckpointName or "", cp.CheckpointFriendlyName or ""
                if name ~= "" then
                    map.checkpoints[#map.checkpoints + 1] = { name = name,
                        label = friendly == "" and name or friendly .. " (" .. name .. ")" }
                end
            end
            maps[#maps + 1] = map
        end
    end
    lists.have_game_maps = #maps > 0
    lists.maps = lists.have_game_maps and maps or builtin_maps()
end

local function refresh_sublevels()
    local subs, current = {}, ""
    local world = mirage.world()
    if world then
        local o = world                     -- The persistent level's package: Outer up from the WorldInfo
        for _ = 1, 16 do
            local outer = o.Outer
            if not outer then break end
            o = outer
        end
        current = mirage.name(o)
        local levels = world.StreamingLevels
        for i = 1, #levels do
            local ls = levels[i]
            if ls then
                subs[#subs + 1] = { name = ls.PackageName or "", loaded = ls.bShouldBeLoaded, visible = ls.bShouldBeVisible }
            end
        end
    end
    lists.sublevels, lists.current = subs, current
end

function M.refresh()
    later.run(function()
        refresh_maps()
        refresh_sublevels()
    end)
end

local ticks = 0
mirage.on("tick", function()
    ticks = ticks + 1
    if ticks % 30 == 0 then refresh_sublevels() end
    if ticks % 120 == 0 and not lists.have_game_maps then refresh_maps() end   -- The game's list comes a bit later
end)
mirage.on("level_load", function(done) if done then refresh_sublevels() end end)
M.refresh()

-- ---- Loading (game thread) ------------------------------------------------------------------------------------------------
local function game_data()
    local world = mirage.world()
    local game = world and world.Game
    if game and mirage.is_a(game, "TdGameInfo") and game.TdGameData then return game.TdGameData end
    return mirage.all("UIDataStore_TdGameData")[1]
end

local function load(map, checkpoint, console, save)
    later.run(function()
        if console then
            local pc = mirage.controller() or (mirage.all("PlayerController")[1])
            if not pc then return mirage.notify("No player controller") end
            mirage.call(pc, "ConsoleCommand", { Command = "open " .. map.file, bWriteToLog = false })
            mirage.notify("open " .. map.file .. (checkpoint and " (checkpoints need the game's load)" or ""))
            return
        end
        local data = game_data()
        if not data then return mirage.notify("The game's data store was not found: try Console load") end
        -- The same call the game's own level select makes. Achievements stay off for loads made here.
        mirage.call(data, "StartGame", { LevelName = map.file, CheckpointName = checkpoint and checkpoint.name or "",
            GameMode = map.mode, URL = "", bShouldSaveCheckpointProgress = save, bAllowSPLevelAchievements = false })
        mirage.info("StartGame " .. map.file .. (checkpoint and " @ " .. checkpoint.name or ""))
    end)
end

-- ---- Drawing (render thread) ------------------------------------------------------------------------------------------
local function title_case(s)
    return (s:lower():gsub("(%f[%w])(%w)", function(_, c) return c:upper() end))
end

-- "CHAPTER 9 - THE SHARD (Scraper_p)" -> "9", "The Shard"
local function map_title(map)
    local label = map.label:gsub(" %(.-%)$", "")
    local chapter, name = label:match("^[Cc][Hh][Aa][Pp][Tt][Ee][Rr] (.-) %- (.*)$")
    if not chapter then name = label end
    if not name or name == "" then name = map.file end
    return chapter or "", title_case(name)
end

-- "CHECKPOINT A (Scraper_Start)" -> "A"
local function checkpoint_title(label)
    local s = label:gsub(" %(.-%)$", "")
    s = s:gsub("^[Cc][Hh][Ee][Cc][Kk][Pp][Oo][Ii][Nn][Tt] ", "")
    return title_case(s)
end

local function contains(text, lower_part)
    return lower_part == "" or text:lower():find(lower_part, 1, true) ~= nil
end

local DIM, HERE = mirage.rgba(140, 140, 140), mirage.rgba(90, 200, 255)
local find_level, opened_for = "", ""

local function draw_levels()
    local width = ui.content_width()
    local maps, current = lists.maps, lists.current:lower()
    local _
    ui.next_width(width)
    _, find_level = ui.input_text_hint("##find", "find a level or checkpoint", find_level, 0, 48)
    local find = find_level:lower()
    local console, save = ConsoleLoad:get(), SaveProgress:get()

    for i = #maps, 1, -1 do           -- The game lists them last to first
        local map = maps[i]
        if map.file:lower() ~= "tdmainmenu" then        -- The Main Menu module does that
            local chapter, name = map_title(map)
            local here = current ~= "" and map.file:lower() == current
            local level_match = contains(name, find) or contains(map.file, find)
            local any_cp = false
            if find ~= "" then
                for _, cp in ipairs(map.checkpoints) do any_cp = any_cp or contains(cp.label, find) end
            end
            if level_match or any_cp then
                ui.id_push(i)
                if find ~= "" then ui.next_open(true, true)
                elseif here and opened_for ~= current then
                    ui.next_open(true, true)
                    opened_for = current
                end
                if here then ui.color_push(ui.COLOR_TEXT, HERE) end
                local open = ui.tree_node(name .. "##map")
                if here then ui.color_pop(1) end
                if ui.item_hovered() then
                    ui.tooltip(map.label .. (here and "\nYou are here" or "") .. "\n" .. #map.checkpoints .. " checkpoints")
                end
                local tag = here and "here" or chapter
                if tag ~= "" then
                    local tw = ui.text_size(tag)
                    ui.same_line(width - tw - 4)
                    ui.text_colored(here and HERE or DIM, tag)
                end
                if open then
                    if #map.checkpoints == 0 and (level_match or find == "") and ui.selectable("Start", false) then
                        load(map, nil, console, save)
                    end
                    for j, cp in ipairs(map.checkpoints) do
                        if level_match or contains(cp.label, find) then
                            local short = checkpoint_title(cp.label)
                            local place = cp.name:gsub("_", " ")
                            local text = short
                            if place:lower() ~= short:lower() then text = short .. "   " .. place end
                            if ui.selectable(text .. "##cp" .. j, false) then load(map, cp, console, save) end
                            if ui.item_hovered() then ui.tooltip(cp.label) end
                        end
                    end
                    ui.tree_pop()
                end
                ui.id_pop()
            end
        end
    end
end

M.levels = mirage.module {
    name = "Levels",
    tab = "Levels",
    description = "Run: read the level list from the game again",
    order = 10,
    action = true,
    panel_open = true,
    on_trigger = M.refresh,
    on_panel = function()
        draw_levels()
        local options = LoadOptions:get()
        ConsoleLoad:set_visible(options)
        SaveProgress:set_visible(options)
    end,
}
LoadOptions = mirage.setting("load_options")
ConsoleLoad = mirage.setting("console_load")
SaveProgress = mirage.setting("save_progress")

-- ---- Sublevels ------------------------------------------------------------------------------------------------------------
local function with_sublevel(name, change)
    later.run(function()
        local world = mirage.world()
        if not world then return mirage.notify("No level loaded") end
        local levels = world.StreamingLevels
        for i = 1, #levels do
            local ls = levels[i]
            if ls and ls.PackageName == name then
                change(ls)
                refresh_sublevels()
                return
            end
        end
        mirage.notify("Sublevel " .. name .. " not found: Run Sublevels to read them again")
    end)
end

local find_sub = ""
local function draw_sublevels()
    local width = ui.content_width()
    local subs = lists.sublevels
    if #subs == 0 then return ui.text_disabled("No sublevels (open a level)") end
    local loaded, visible = 0, 0
    for _, s in ipairs(subs) do
        if s.loaded then loaded = loaded + 1 end
        if s.visible then visible = visible + 1 end
    end
    ui.text_disabled(string.format("%d sublevels, %d loaded, %d visible", #subs, loaded, visible))
    if ui.item_hovered() then
        ui.tooltip("Streaming volumes in the level load and show them as you move: they can change these back")
    end
    local _
    ui.next_width(width)
    _, find_sub = ui.input_text_hint("##find", "find", find_sub, 0, 48)
    local find = find_sub:lower()
    local height = math.min(#subs + 1, 12) * ui.frame_height() + 6
    if not ui.table_begin("##subs", 3, ui.TABLE_ROW_BG + ui.TABLE_SCROLL_Y, width, height) then return end
    ui.table_column("Sublevel", ui.COLUMN_STRETCH)
    ui.table_column("Loaded", ui.COLUMN_FIXED)
    ui.table_column("Visible", ui.COLUMN_FIXED)
    ui.table_headers()
    for i, s in ipairs(subs) do
        if contains(s.name, find) then
            ui.id_push(i)
            ui.table_next_row()
            ui.table_next_column()
            ui.align_text_to_frame()
            if s.visible then ui.text(s.name) else ui.text_disabled(s.name) end
            if ui.item_hovered() then ui.tooltip(s.name) end
            ui.table_next_column()
            if ui.checkbox("##loaded", s.loaded) then
                with_sublevel(s.name, function(ls)
                    if ls.bShouldBeLoaded then
                        ls.bShouldBeLoaded, ls.bShouldBeVisible = false, false
                    else
                        ls.bShouldBeLoaded, ls.bShouldBeVisible, ls.bShouldBlockOnLoad = true, true, true
                    end
                end)
            end
            if ui.item_hovered() then ui.tooltip(s.loaded and "Unload it" or "Load and show it") end
            ui.table_next_column()
            if ui.checkbox("##visible", s.visible) then
                with_sublevel(s.name, function(ls)
                    if ls.bShouldBeVisible then ls.bShouldBeVisible = false
                    else ls.bShouldBeLoaded, ls.bShouldBeVisible = true, true end
                end)
            end
            if ui.item_hovered() then ui.tooltip(s.visible and "Hide it (stays loaded)" or "Show it (loads it if needed)") end
            ui.id_pop()
        end
    end
    ui.table_end()
end

M.sublevels = mirage.module {
    name = "Sublevels",
    tab = "Levels",
    description = "Streaming sublevels of this level. Run: read the list again",
    order = 40,
    action = true,
    panel_open = true,
    on_trigger = M.refresh,
    on_panel = draw_sublevels,
}

-- Shared for the tests: how many maps and sublevels it read
mirage.on("tick", function()
    if ticks % 60 == 0 then
        mirage.share("maps", #lists.maps)
        mirage.share("game_maps", lists.have_game_maps and 1 or 0)
        mirage.share("sublevels", #lists.sublevels)
        mirage.share("current", lists.current)
    end
end)

return M
