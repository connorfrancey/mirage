-- Self-test mod (step 5, Scenario C): MirageSeqAct_Ping, a Kismet action whose Activated is Lua, put in the Prologue's
-- Kismet. With the module on: the first touch event of Edge_p with links and a Trigger for its originator loses its
-- links (so walking in does nothing of the game's) and gets one to a new Ping op instead. Off: mirage.revert() puts the
-- graph back. Unloading (a hot reload) puts it back by itself. What it did is shared for the self-test ("Mirage Kismet
-- (edit)").
local m = mirage.module { name = "Kismet edit (Lua)", tab = "Mods", description = "Self-test: edits the level's Kismet" }
local applied, defined = false, false
local pings = 0

local function define()
    if defined then return true end
    local cls, why = mirage.define_class {
        name = "MirageSeqAct_Ping", extends = "SequenceAction",
        vars = { "int Pings" },
        functions = {
            Activated = function(self)
                self.Pings = self.Pings + 1
                pings = pings + 1
                mirage.share("pings", pings)
            end,
        },
    }
    if not cls then
        mirage.share("problem", "define_class: " .. tostring(why))
        return false
    end
    defined = true
    return true
end

-- The touch event to take over: links out, enabled, can fire again, a Trigger fires it
local function pick()
    local seqs = mirage.kismet("Edge_p")
    local ops = {}
    for _, s in ipairs(seqs) do
        for _, op in ipairs(s.ops) do ops[op.path] = op end
    end
    -- The one taken over before a hot reload (shared values outlive it): the same one again
    local before = mirage.shared("kismet-edit", "event")
    if before ~= "" and ops[before] then
        local event = mirage.find("SeqEvent_Touch " .. before)
        if event then return event, ops[before], ops end
    end
    for _, s in ipairs(seqs) do
        for _, op in ipairs(s.ops) do
            if op.class == "SeqEvent_Touch" and op.originator ~= "" and #op.outputs > 0 and #op.outputs[1].links > 0 then
                local event = mirage.find("SeqEvent_Touch " .. op.path)
                local origin = event and event.Originator
                if origin and mirage.is_a(origin, "Trigger") and event.bEnabled
                    and (event.MaxTriggerCount == 0 or event.TriggerCount < event.MaxTriggerCount) then
                    return event, op, ops
                end
            end
        end
    end
end

local function apply()
    if not define() then return end
    local event, op, ops = pick()
    if not event then
        mirage.share("problem", "no touch event to take over in Edge_p")
        return
    end
    local unlinked = 0
    mirage.transaction("ping instead", function()
        for i, out in ipairs(op.outputs) do
            for _, link in ipairs(out.links) do
                local target = mirage.find(ops[link.op].class .. " " .. link.op)
                if mirage.kismet_unlink(event, i - 1, target, link.input) then unlinked = unlinked + 1 end
            end
        end
        local ping = mirage.kismet_create(event.ParentSequence, "MirageSeqAct_Ping")
        if not ping then
            mirage.share("problem", "kismet_create failed")
            return
        end
        if not mirage.kismet_link(event, "Touched", ping, "In") then mirage.share("problem", "kismet_link failed") end
    end)
    mirage.share("event", op.path)
    mirage.share("unlinked", unlinked)
    mirage.share("applied", (tonumber(mirage.shared("kismet-edit", "applied")) or 0) + 1)
end

mirage.on("tick", function()
    if m:enabled() and not applied then
        applied = true
        apply()
    elseif not m:enabled() and applied then
        applied = false
        mirage.share("reverted", mirage.revert())
    end
end)
