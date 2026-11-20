// mirage.vec for Lua mods (lua_vec.h)
#include "runtime/lua_vec.h"

#include <lauxlib.h>
#include <lua.h>

#include <string_view>

namespace mirage::lua
{
	namespace
	{
		// Run with the mirage table and the metatable as its arguments
		constexpr std::string_view kSource = R"lua(
local mirage, V = ...
V.__index = V
V.__name = "mirage.vec"
local setmetatable, getmetatable, type, sqrt = setmetatable, getmetatable, type, math.sqrt

local function new(x, y, z) return setmetatable({x = x, y = y, z = z}, V) end
-- Anything vector-like as three numbers: a vector, {x =, y =, z =}, {1, 2, 3} or a number (all three)
local function parts(v)
    if type(v) == "number" then return v, v, v end
    if type(v) ~= "table" then error("vector expected, got " .. type(v), 3) end
    return v.x or v[1] or 0, v.y or v[2] or 0, v.z or v[3] or 0
end

local vec = setmetatable({}, {
    __call = function(_, x, y, z)
        if type(x) == "table" or (y == nil and type(x) == "number") then return new(parts(x)) end
        return new(x or 0, y or 0, z or 0)
    end,
    __index = function(_, key)
        if key == "zero" then return new(0, 0, 0)
        elseif key == "up" then return new(0, 0, 1)
        elseif key == "forward" then return new(1, 0, 0)
        elseif key == "right" then return new(0, 1, 0) end
    end,
})
function vec.is(x) return getmetatable(x) == V end
mirage.vec = vec

function V.__add(a, b) local ax, ay, az = parts(a); local bx, by, bz = parts(b); return new(ax + bx, ay + by, az + bz) end
function V.__sub(a, b) local ax, ay, az = parts(a); local bx, by, bz = parts(b); return new(ax - bx, ay - by, az - bz) end
function V.__mul(a, b) local ax, ay, az = parts(a); local bx, by, bz = parts(b); return new(ax * bx, ay * by, az * bz) end
function V.__div(a, b) local ax, ay, az = parts(a); local bx, by, bz = parts(b); return new(ax / bx, ay / by, az / bz) end
function V.__unm(a) return new(-a.x, -a.y, -a.z) end
function V.__eq(a, b) return a.x == b.x and a.y == b.y and a.z == b.z end
function V.__tostring(a) return ("vec(%g, %g, %g)"):format(a.x, a.y, a.z) end

function V:len2() return self.x * self.x + self.y * self.y + self.z * self.z end
function V:len() return sqrt(self:len2()) end
function V:dot(b) local x, y, z = parts(b); return self.x * x + self.y * y + self.z * z end
function V:cross(b)
    local x, y, z = parts(b)
    return new(self.y * z - self.z * y, self.z * x - self.x * z, self.x * y - self.y * x)
end
function V:dist(b) local x, y, z = parts(b); return new(self.x - x, self.y - y, self.z - z):len() end
-- Length 1 in the same direction; the zero vector stays zero
function V:normalized()
    local l = self:len()
    if l == 0 then return new(0, 0, 0) end
    return new(self.x / l, self.y / l, self.z / l)
end
function V:lerp(b, t) local x, y, z = parts(b); return new(self.x + (x - self.x) * t, self.y + (y - self.y) * t, self.z + (z - self.z) * t) end
function V:copy() return new(self.x, self.y, self.z) end
function V:unpack() return self.x, self.y, self.z end
)lua";
	}

	bool InstallVec(lua_State* L, int mirageIndex)
	{
		mirageIndex = lua_absindex(L, mirageIndex);
		if (luaL_loadbufferx(L, kSource.data(), kSource.size(), "=mirage.vec", "t") != LUA_OK) {
			lua_pop(L, 1);
			return false;
		}
		lua_pushvalue(L, mirageIndex);
		luaL_newmetatable(L, kVecMeta);
		if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
			lua_pop(L, 1);
			return false;
		}
		return true;
	}

	void PushVec(lua_State* L, float x, float y, float z)
	{
		lua_createtable(L, 0, 3);
		lua_pushnumber(L, x);
		lua_setfield(L, -2, "x");
		lua_pushnumber(L, y);
		lua_setfield(L, -2, "y");
		lua_pushnumber(L, z);
		lua_setfield(L, -2, "z");
		luaL_setmetatable(L, kVecMeta);
	}
}
