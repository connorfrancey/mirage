-- Self-test mod (step 12, the milestone): MirageBouncePadLua, the bounce pad of the self-test's .uc class described
-- from Lua instead of written in UnrealScript: the same variables and defaults, its Touch event and a new function
-- written in Lua. The runtime writes the UnrealScript (mirage\classes\generated\class-lua\), compiles and loads it, and
-- runs these functions from inside the class's bytecode (script injection). Defined on the first tick with the module on.
local m = mirage.module {
    name = "Class (Lua)", tab = "Mods", description = "Self-test: a bounce pad class defined from Lua",
}
Touches = m:slider_int("Touches", 0, 0, 1 << 30)
local defined = false

-- A native of this mod's, declared on the class in UnrealScript: native("double") is its slot in the engine's table
mirage.native("double", "float(float)", function(x) return x * 2 end)

mirage.on("tick", function()
    if defined or not m:enabled() then return end
    defined = true
    mirage.define_class {
        name = "MirageBouncePadLua", extends = "Trigger", placeable = true,
        vars = { "var() float Strength", "int Bounces" },
        defaults = { Strength = 1500.0, bNoDelete = false },
        functions = {
            Touch = function(self, params)
                local other = params.Other
                if not other or not mirage.is_a(other, "Pawn") then return end
                self.Bounces = self.Bounces + 1
                other.Velocity.Z = self.Strength
                mirage.call(other, "SetPhysics", { newPhysics = 2 })   -- PHYS_Falling
                Touches:set(Touches:get() + 1)
            end,
            TimesBounced = { "function int TimesBounced()", function(self) return self.Bounces end },
        },
        extra = 'native("double") function float Twice(float X);',
    }
end)
