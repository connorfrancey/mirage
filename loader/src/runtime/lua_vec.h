#pragma once
// Vectors for Lua mods: mirage.vec, so a mod doesn't write its own vector math. A vector is a table {x, y, z} with a
// metatable, so anything that takes {x = .., y = .., z = ..} or {1, 2, 3} takes one, and the vectors the API returns
// (locations, velocities, hits) are vectors too.
//
//     local v = mirage.vec(1, 2, 3)          -- or mirage.vec{x = 1, y = 2, z = 3}, mirage.vec{1, 2, 3}, mirage.vec(5)
//     local w = (v + pawn_location) * 2 - mirage.vec.up * 50
//     w:len(), w:len2(), w:dist(v), w:dot(v), w:cross(v), w:normalized(), w:lerp(v, 0.5), w:copy(), w:unpack()
//     v == mirage.vec(1, 2, 3), tostring(v) == "vec(1, 2, 3)", -v, v / 2, 2 * v, v * v (each component)
//     mirage.vec.zero, mirage.vec.up (0, 0, 1: the game's Z is up), mirage.vec.forward (1, 0, 0), mirage.vec.right
//     mirage.vec.is(x): whether x is one
//
// The constants are fresh copies on each read, so changing one changes nothing else.

struct lua_State;

namespace mirage::lua
{
	// The metatable's registry name (luaL_getmetatable)
	inline constexpr const char* kVecMeta = "mirage.vec";

	// Defines mirage.vec in the table at `mirageIndex` and the metatable kVecMeta. False if its chunk failed (a bug).
	bool InstallVec(lua_State* L, int mirageIndex);
	// A new vector on the stack (the metatable set when InstallVec ran)
	void PushVec(lua_State* L, float x, float y, float z);
}
