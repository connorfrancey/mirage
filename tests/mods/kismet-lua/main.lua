-- Self-test mod (step 5): mirage.kismet() gives the loaded levels' sequences as tables. Its test, run by the self-test's
-- "Mod tests" step in the Prologue: a root sequence, touch events wired to something, links that name ops it has.
mirage.module { name = "Kismet (Lua)", tab = "Mods", description = "Self-test: reads the level's Kismet" }

mirage.test("the Prologue's Kismet reads", function()
    local seqs = mirage.kismet("Edge_p")
    if not seqs or #seqs == 0 then return false, "no sequence in Edge_p" end
    local ops, roots, touches, wired = {}, 0, 0, 0
    for _, s in ipairs(seqs) do
        if s.parent == "" then roots = roots + 1 end
        for _, op in ipairs(s.ops) do ops[op.path] = op end
    end
    for _, op in pairs(ops) do
        if op.class == "SeqEvent_Touch" then
            touches = touches + 1
            for _, out in ipairs(op.outputs) do
                for _, link in ipairs(out.links) do
                    if not ops[link.op] then return false, op.path .. " links to " .. link.op .. ", which isn't read" end
                    wired = wired + 1
                end
            end
        end
    end
    if roots ~= 1 then return false, roots .. " root sequences, not 1" end
    if touches == 0 or wired == 0 then return false, touches .. " touch events, " .. wired .. " links from them" end
end)
