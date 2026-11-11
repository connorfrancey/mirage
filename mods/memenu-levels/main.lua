-- Levels: the menu's level actions, as a mod using only the public API.
--
--   Restart Checkpoint   the game's own restart from the last checkpoint (no progress saved, no achievements)
--   Main Menu            quit to the main menu, as the pause menu does
--
-- Both go through the game's data store (UIDataStore_TdGameData), the object its own menus call.

local later = require("later")     -- Game-thread work from on_trigger
require("loader")                  -- Levels, Sublevels (loader.lua)

-- The game's data store: the game info's in a level; found among all objects elsewhere (the main menu)
local function game_data()
    local world = mirage.world()
    local game = world and world.Game
    if game and mirage.is_a(game, "TdGameInfo") and game.TdGameData then return game.TdGameData end
    return mirage.all("UIDataStore_TdGameData")[1]
end

local function with_game_data(what, fn)
    later.run(function()
        local data = game_data()
        if not data then
            mirage.notify(what .. ": the game's data store was not found")
            return
        end
        fn(data)
        mirage.info(what)
    end)
end

mirage.module {
    name = "Restart Checkpoint",
    tab = "Levels",
    description = "Reload from the last checkpoint",
    order = 20,
    action = true,
    on_trigger = function()
        with_game_data("Restarting from last checkpoint", function(data)
            mirage.call(data, "RestartFromLastCheckpoint",
                        { bShouldSaveCheckpointProgress = false, bAllowSPLevelAchievements = false })
        end)
    end,
}

mirage.module {
    name = "Main Menu",
    tab = "Levels",
    description = "Quit to the main menu",
    order = 30,
    action = true,
    on_trigger = function()
        with_game_data("Quitting to main menu", function(data) mirage.call(data, "QuitToMainMenu") end)
    end,
}
