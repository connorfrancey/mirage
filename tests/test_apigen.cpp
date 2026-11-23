#include "runtime/lua/lua_functions.h"

#include <cstring>

namespace mirage::mods::sdk_binding
{
	// The game-free test checks the actual Lua registry and every generated wrapper's arguments.
	int Read(lua_State* L, const char* owner, const char* property, int32_t type)
	{
		if (std::strcmp(owner, "Class ALAudio.ALAudioDevice") != 0 || type != MIRAGE_VALUE_FLOAT)
			return luaL_error(L, "wrong generated owner or type");
		if (std::strcmp(property, "MinOggVorbisDurationGame") == 0) lua_pushnumber(L, 1.25);
		else if (std::strcmp(property, "MinOggVorbisDurationEditor") == 0) lua_pushnumber(L, 2.5);
		else return luaL_error(L, "wrong generated property");
		return 1;
	}
}

int main()
{
	lua_State* L = luaL_newstate();
	if (!L) return 1;
	lua_newtable(L);
	int functions = 0;
	for (const auto* list : mirage::mods::LuaFunctions::All()) {
		if (!list->table || std::strcmp(list->table, "sdk") != 0) continue;
		luaL_setfuncs(L, list->fns.data(), 0);
		functions += static_cast<int>(list->fns.size()) - 1;
	}
	const auto check = [L](const char* name, lua_Number value) {
		lua_getfield(L, -1, name);
		const bool ok = lua_pcall(L, 0, 1, 0) == LUA_OK && lua_isnumber(L, -1) && lua_tonumber(L, -1) == value;
		lua_pop(L, 1);
		return ok;
	};
	const bool ok = functions == 2 && check("get_vorbis_game_threshold", 1.25) &&
		check("get_vorbis_editor_threshold", 2.5);
	lua_close(L);
	return ok ? 0 : 1;
}
