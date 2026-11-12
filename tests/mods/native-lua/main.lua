-- Self-test mod (step 11, mod natives): registers a Lua function as a native the game's bytecode can call, and while
-- its module is on patches Core.Object.FPctByRange so that its `return (Value - InMin) / (InMax - InMin)` becomes
-- `return halve((Value - InMin) / (InMax - InMin))`: the game's VM calls back into this Lua state with the value.
local FUNCTION = "Core.Object.FPctByRange"
local HALVE = mirage.native("halve", "float(float)", function(x) return x / 2 end)

local m = mirage.module {
    name = "Native (Lua)", tab = "Mods", description = "Self-test: FPctByRange's result goes through a Lua native",
}
Index = m:slider_int("Index", 0, 0, 4096)
Index:set(HALVE)
local patched = false

local function patch()
    local s = mirage.script.open(FUNCTION)
    if not s then return false end
    for i = 1, s:count() do
        local t = s:get(i)
        if t.kind == "Return" and t.children and t.children[1] then
            local value = s:get(t.children[1])
            if value.kind ~= "Nothing" then
                local call = s:add { kind = "NativeFunction", native = HALVE, children = { t.children[1] } }
                s:set_children(i, { call })
                local ok = s:apply()
                s:close()
                return ok
            end
        end
    end
    s:close()
    mirage.warn("no return with a value in " .. FUNCTION)
    return false
end

mirage.on("tick", function()
    local want = m:enabled()
    if want and not patched then
        patched = patch()
    elseif not want and patched then
        mirage.script.restore(FUNCTION)
        patched = false
    end
end)
