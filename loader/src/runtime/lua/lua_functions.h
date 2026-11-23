#pragma once
// Lua function lists installed by LuaMod::Bind, including MirageAPIGen registrations.

#include <initializer_list>
#include <vector>
#include <lauxlib.h>
#include <mirage/api.h>

namespace mirage::mods
{
	// Each area supplies a list. Bind installs it in mirage or the named subtable and sets the mod as its upvalue.
	struct LuaFunctions
	{
		LuaFunctions(const char* table, std::initializer_list<luaL_Reg> fns);
		const char* table;
		std::vector<luaL_Reg> fns;	// Sentinel terminated for luaL_setfuncs.
		static std::vector<const LuaFunctions*>& All();
	};

	namespace sdk_binding
	{
		int Read(lua_State* L, const char* owner, const char* property, int32_t type);
		int Write(lua_State* L, const char* owner, const char* property, int32_t type);
	}
}
