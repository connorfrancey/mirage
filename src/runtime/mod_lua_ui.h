#pragma once
// mirage.ui for Lua mods (mod_lua_ui.cpp)
#include "mirage/api.h"

struct lua_State;

namespace mirage::mods::lua_ui
{
	void Push(lua_State* L, const MirageUi* ui);	// The mirage.ui table, on the stack
}
