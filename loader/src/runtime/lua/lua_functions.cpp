// Registration lists shared by the Lua runtime and the generated binding tests.
#include "runtime/lua/lua_functions.h"

namespace mirage::mods
{
	LuaFunctions::LuaFunctions(const char* t, std::initializer_list<luaL_Reg> list) : table(t), fns(list)
	{
		fns.push_back({nullptr, nullptr});
		All().push_back(this);
	}

	std::vector<const LuaFunctions*>& LuaFunctions::All()
	{
		static std::vector<const LuaFunctions*> all;
		return all;
	}
}
