-- Self-test mod (step 14): while its module is on, it watches actors come and go and the checkpoint restarts, and keeps
-- the actor the self-test spawns (its Tag is "WorldGravityZ": a name the game's name table already has, and no class's,
-- so no other actor has it by default). Then it asks the world about it: by tag, and around the
-- player (nearest first). When the probe is removed, the event's object must equal the one it kept, and the kept one
-- must no longer be valid. What it saw is shared for the self-test (mirage.share).
local m = mirage.module { name = "World access (Lua)", tab = "Mods", description = "Self-test: world access (step 14)" }

local added, removed, restarts, streamed = 0, 0, 0, 0
local probe = nil               -- The probe, kept from its actor_added event
local queried = false

local function share()
    mirage.share("added", added)
    mirage.share("removed", removed)
    mirage.share("restarts", restarts)
    mirage.share("streamed", streamed)
end

-- Subscribed while the module is on (actor events cost a comparison of the levels' lists each tick while anyone listens)
local subs = {}
local function subscribe()
    subs[#subs + 1] = mirage.on("actor_added", function(actor, class)
        added = added + 1
        if actor.Tag == "WorldGravityZ" then
            probe = actor
            queried = false
            mirage.share("probe_class", class)
            mirage.share("probe_valid", mirage.valid(probe) and 1 or 0)
        end
        share()
    end)

    subs[#subs + 1] = mirage.on("actor_removed", function(actor)
        removed = removed + 1
        if probe and actor == probe then
            -- The event's object is the one kept (==), and neither can be used any more
            mirage.share("probe_removed", 1)
            mirage.share("kept_valid_after", mirage.valid(probe) and 1 or 0)
            mirage.share("kept_z_after", tostring(probe.Location))     -- A gone object reads as nothing (nil), never another's
        end
        share()
    end)

    subs[#subs + 1] = mirage.on("restart", function()
        restarts = restarts + 1
        share()
    end)

    subs[#subs + 1] = mirage.on("level_streamed", function(visible, level)
        streamed = streamed + 1
        mirage.share("last_streamed", (visible and "in " or "out ") .. level)
        share()
    end)
end
local function unsubscribe()
    for _, sub in ipairs(subs) do sub:remove() end
    subs = {}
end

mirage.on("tick", function()
    if m:enabled() and #subs == 0 then subscribe() elseif not m:enabled() and #subs > 0 then unsubscribe() end
    if not m:enabled() or not probe or queried or not mirage.valid(probe) then return end
    queried = true
    -- By tag: just the probe
    local tagged = mirage.actors { tag = "WorldGravityZ" }
    mirage.share("query_tag", #tagged)
    mirage.share("query_tag_is_probe", (#tagged == 1 and tagged[1] == probe) and 1 or 0)
    -- Around the player, nearest first: the player itself (distance 0) first, the probe among them
    local pawn = mirage.pawn()
    local near = pawn and mirage.actors { near = pawn, radius = 600 } or {}
    local has = 0
    for _, a in ipairs(near) do if a == probe then has = 1 end end
    mirage.share("query_near", #near)
    mirage.share("query_near_first_is_pawn", (#near > 0 and near[1] == pawn) and 1 or 0)
    mirage.share("query_near_has_probe", has)
    -- By class: actors of an engine class, and a class that doesn't exist
    mirage.share("query_class", #mirage.actors { class = "Engine.PlayerStart" })
    mirage.share("query_missing", #mirage.actors { class = "NoSuchClassAnywhere" })
    mirage.share("queried", 1)      -- Last: the self-test reads the others once it sees this
end)
