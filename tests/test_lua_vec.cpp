// mirage.vec (runtime/lua_vec.h): the vector class Lua mods get, run in a plain Lua state

#include "runtime/lua_vec.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <gtest/gtest.h>

#include <string>

namespace
{
	// A state with mirage.vec, running `code`: "" if it ran, else Lua's message
	std::string RunLua(const std::string& code)
	{
		lua_State* L = luaL_newstate();
		luaL_openlibs(L);
		lua_newtable(L);
		if (!mirage::lua::InstallVec(L, -1)) {
			lua_close(L);
			return "InstallVec failed";
		}
		lua_setglobal(L, "mirage");
		std::string error;
		if (luaL_dostring(L, code.c_str()) != LUA_OK) error = lua_tostring(L, -1);
		lua_close(L);
		return error;
	}
}

TEST(LuaVec, Arithmetic)
{
	const char* code = R"lua(
		local v = mirage.vec(1, 2, 3)
		assert(mirage.vec.is(v) and not mirage.vec.is({x = 1, y = 2, z = 3}))
		assert(v + {x = 1, y = 1, z = 1} == mirage.vec(2, 3, 4))
		assert({1, 1, 1} + v == mirage.vec(2, 3, 4))		-- either side, any vector-like table
		assert(v - v == mirage.vec.zero)
		assert(v * 2 == mirage.vec(2, 4, 6) and 2 * v == v * 2)
		assert(v * v == mirage.vec(1, 4, 9))
		assert(v / 2 == mirage.vec(0.5, 1, 1.5))
		assert(-v == mirage.vec(-1, -2, -3))
		assert(tostring(v) == "vec(1, 2, 3)", tostring(v))
		assert(mirage.vec(5) == mirage.vec(5, 5, 5))
		assert(mirage.vec{4, 5, 6} == mirage.vec(4, 5, 6) and mirage.vec{x = 7} == mirage.vec(7, 0, 0))
		assert(mirage.vec() == mirage.vec.zero)
	)lua";
	EXPECT_EQ(RunLua(code), "");
}

TEST(LuaVec, Methods)
{
	const char* code = R"lua(
		local v = mirage.vec(3, 4, 0)
		assert(v:len() == 5 and v:len2() == 25)
		assert(v:normalized() == mirage.vec(0.6, 0.8, 0))
		assert(mirage.vec.zero:normalized() == mirage.vec.zero)
		assert(v:dot({1, 1, 1}) == 7)
		assert(mirage.vec.forward:cross(mirage.vec.right) == mirage.vec.up)
		assert(v:dist(mirage.vec.zero) == 5)
		assert(v:lerp(mirage.vec.zero, 0.5) == mirage.vec(1.5, 2, 0))
		local x, y, z = v:unpack()
		assert(x == 3 and y == 4 and z == 0)
		local c = v:copy(); c.x = 9
		assert(v.x == 3)
		local up = mirage.vec.up; up.z = 7
		assert(mirage.vec.up.z == 1)						-- the constants are fresh copies
	)lua";
	EXPECT_EQ(RunLua(code), "");
}

TEST(LuaVec, ErrorsNameTheProblem)
{
	EXPECT_NE(RunLua("return mirage.vec(1, 2, 3) + 'a'").find("vector expected, got string"), std::string::npos);
}

TEST(LuaVec, PushVecIsAVector)
{
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);
	lua_newtable(L);
	ASSERT_TRUE(mirage::lua::InstallVec(L, -1));
	lua_setglobal(L, "mirage");
	mirage::lua::PushVec(L, 1, 2, 3);
	lua_setglobal(L, "pushed");
	EXPECT_EQ(luaL_dostring(L, "assert(mirage.vec.is(pushed) and pushed:len2() == 14)"), LUA_OK) << lua_tostring(L, -1);
	lua_close(L);
}
