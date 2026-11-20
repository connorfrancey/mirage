// Lua mods: mirage.script: patching a function's bytecode (runtime/lua/lua_mod.h)
#include "runtime/lua/lua_mod.h"

namespace mirage::mods
{
	// ---- mirage.script: script patching (step 8) -------------------------------------------------------------------
	//
	//   local s = mirage.script.open("TdGame.TdMove_Jump.StartMove")
	//   print(s:dump())                         -- every token: index, offset, kind, operands
	//   local t = s:get(12)                     -- {kind = "FloatConst", float = 10.0, children = {...}, ...}
	//   t.float = 20.0; s:set(12, t)
	//   local k = s:add{kind = "FloatConst", float = 1.5}
	//   s:set_children(9, {s:children(9)[1], k})
	//   s:apply()                               -- in place until mirage.script.restore(path) or the mod's unload
	//
	// Indices are 1-based; children(0) / set_children(0, ...) are the top-level statements.

	int LuaMod::MenuOpen(lua_State* L)
	{
		lua_pushboolean(L, Self(L).api_->menu_open());
		return 1;
	}
	// mirage.input_key(vk, down): a real key press (true) or release (false)
	int LuaMod::InputKey(lua_State* L)
	{
		Self(L).api_->input_key(static_cast<int32_t>(luaL_checkinteger(L, 1)), lua_toboolean(L, 2));
		return 0;
	}
	// mirage.share(key, value): a value other mods read with mirage.shared(this mod's id, key); any Lua value as text
	int LuaMod::Share(lua_State* L)
	{
		LuaMod& self = Self(L);
		const char* key = luaL_checkstring(L, 1);
		const char* value = luaL_tolstring(L, 2, nullptr);
		self.api_->share_set(self.mod_, key, value);
		lua_pop(L, 1);
		return 0;
	}
	int LuaMod::Shared(lua_State* L)
	{
		const char* mod = luaL_checkstring(L, 1);
		const char* key = luaL_checkstring(L, 2);
		const MirageApi& api = *Self(L).api_;
		std::string v(api.share_get(mod, key, nullptr, 0), '_');
		api.share_get(mod, key, v.data(), static_cast<uint32_t>(v.size() + 1));
		lua_pushlstring(L, v.data(), v.size());
		return 1;
	}

	int LuaMod::Notify(lua_State* L)
	{
		LuaMod& self = Self(L);
		const std::string text = Concat(L);
		self.api_->notify(self.mod_, text.c_str());
		return 0;
	}

	MirageScript* LuaMod::CheckScript(lua_State* L)
	{
		auto** box = static_cast<MirageScript**>(luaL_checkudata(L, 1, kScriptMeta));
		if (!*box) luaL_error(L, "the script was closed");
		return *box;
	}

	int LuaMod::ScriptOpen(lua_State* L)
	{
		LuaMod& self = Self(L);
		MirageScript* s = self.api_->script_open(self.mod_, luaL_checkstring(L, 1));
		if (!s) {
			lua_pushnil(L);
			return 1;
		}
		auto** box = static_cast<MirageScript**>(lua_newuserdatauv(L, sizeof(MirageScript*), 0));
		*box = s;
		luaL_setmetatable(L, kScriptMeta);
		return 1;
	}

	int LuaMod::ScriptRestore(lua_State* L)
	{
		LuaMod& self = Self(L);
		lua_pushboolean(L, self.api_->script_restore(self.mod_, luaL_checkstring(L, 1)));
		return 1;
	}

	int LuaMod::ScriptCount(lua_State* L)
	{
		lua_pushinteger(L, Self(L).api_->script_count(CheckScript(L)));
		return 1;
	}

	int LuaMod::ScriptGet(lua_State* L)
	{
		const MirageApi& api = *Self(L).api_;
		MirageScript* s = CheckScript(L);
		const auto index = static_cast<int32_t>(luaL_checkinteger(L, 2)) - 1;
		MirageToken t{};
		t.size = sizeof t;
		if (!api.script_get(s, index, &t)) {
			lua_pushnil(L);
			return 1;
		}
		lua_newtable(L);
		lua_pushstring(L, api.script_kind_name(t.kind));
		lua_setfield(L, -2, "kind");
		lua_pushinteger(L, t.native_index);
		lua_setfield(L, -2, "native");
		lua_pushinteger(L, t.int_value);
		lua_setfield(L, -2, "int");
		lua_pushnumber(L, t.floats[0]);
		lua_setfield(L, -2, "float");
		lua_createtable(L, 3, 0);
		for (int i = 0; i < 3; ++i) {
			lua_pushnumber(L, t.floats[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "floats");
		lua_createtable(L, 3, 0);
		for (int i = 0; i < 3; ++i) {
			lua_pushinteger(L, t.ints[i]);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "ints");
		PushObject(L, t.object);
		lua_setfield(L, -2, "object");
		PushObject(L, t.object2);
		lua_setfield(L, -2, "object2");
		char objectName[256] = "";
		if (t.object) api.object_name(t.object, objectName, sizeof objectName);
		lua_pushstring(L, objectName);
		lua_setfield(L, -2, "object_name");
		lua_pushstring(L, t.name);
		lua_setfield(L, -2, "name");
		lua_pushinteger(L, t.word);
		lua_setfield(L, -2, "word");
		lua_pushinteger(L, t.word2);
		lua_setfield(L, -2, "word2");
		lua_pushinteger(L, t.byte_value);
		lua_setfield(L, -2, "byte");
		lua_pushinteger(L, t.byte_value2);
		lua_setfield(L, -2, "byte2");
		lua_pushinteger(L, t.memory_offset);
		lua_setfield(L, -2, "offset");
		lua_pushstring(L, t.text);
		lua_setfield(L, -2, "text");
		// Its operands, as indices
		const int32_t n = api.script_children(s, index, nullptr, 0);
		std::vector<int32_t> kids(static_cast<std::size_t>(std::max(n, 0)));
		if (n > 0) api.script_children(s, index, kids.data(), n);
		lua_createtable(L, n, 0);
		for (int32_t i = 0; i < n; ++i) {
			lua_pushinteger(L, kids[static_cast<std::size_t>(i)] + 1);
			lua_rawseti(L, -2, i + 1);
		}
		lua_setfield(L, -2, "children");
		return 1;
	}

	// A token table (as script:get gives, or the fields the mod sets) into a MirageToken; `t` starts as the current one
	bool LuaMod::ToToken(lua_State* L, int index, MirageToken& t)
	{
		const MirageApi& api = *Self(L).api_;
		luaL_checktype(L, index, LUA_TTABLE);
		const auto field = [&](const char* name) { return lua_getfield(L, index, name); };
		if (field("kind") == LUA_TSTRING) {
			const int32_t k = api.script_kind(lua_tostring(L, -1));
			if (k < 0) return luaL_error(L, "no token kind '%s'", lua_tostring(L, -1)), false;
			t.kind = k;
		}
		lua_pop(L, 1);
		const auto integer = [&](const char* name, auto& out) {
			if (field(name) == LUA_TNUMBER) out = static_cast<std::remove_reference_t<decltype(out)>>(lua_tointeger(L, -1));
			lua_pop(L, 1);
		};
		integer("native", t.native_index);
		integer("int", t.int_value);
		integer("word", t.word);
		integer("word2", t.word2);
		integer("byte", t.byte_value);
		integer("byte2", t.byte_value2);
		if (field("float") == LUA_TNUMBER) t.floats[0] = static_cast<float>(lua_tonumber(L, -1));
		lua_pop(L, 1);
		for (const auto& [name, isFloat] : {std::pair{"floats", true}, std::pair{"ints", false}}) {
			if (field(name) == LUA_TTABLE)
				for (int i = 0; i < 3; ++i) {
					if (lua_rawgeti(L, -1, i + 1) == LUA_TNUMBER) {
						if (isFloat) t.floats[i] = static_cast<float>(lua_tonumber(L, -1));
						else t.ints[i] = static_cast<int32_t>(lua_tointeger(L, -1));
					}
					lua_pop(L, 1);
				}
			lua_pop(L, 1);
		}
		for (const auto& [name, out] : {std::pair{"object", &t.object}, std::pair{"object2", &t.object2}}) {
			const int type = field(name);
			if (type == LUA_TNIL) {
			} else if (type == LUA_TSTRING) {		// By full name: "FloatProperty Core.Object.Vector.Z"
				*out = api.object_find(lua_tostring(L, -1));
				if (!*out) return luaL_error(L, "no object '%s'", lua_tostring(L, -1)), false;
			} else if (type == LUA_TBOOLEAN && !lua_toboolean(L, -1)) {
				*out = nullptr;
			} else {
				*out = CheckObject(L, lua_gettop(L));
			}
			lua_pop(L, 1);
		}
		const auto text = [&](const char* name, char* out, std::size_t capacity) {
			if (field(name) == LUA_TSTRING) {
				std::size_t n = 0;
				const char* s = lua_tolstring(L, -1, &n);
				n = std::min(n, capacity - 1);
				std::memcpy(out, s, n);
				out[n] = '\0';
			}
			lua_pop(L, 1);
		};
		text("name", t.name, sizeof t.name);
		text("text", t.text, sizeof t.text);
		if (field("target") == LUA_TNUMBER) t.target = static_cast<int32_t>(lua_tointeger(L, -1)) - 1;
		else if (lua_type(L, -1) == LUA_TSTRING && std::string_view(lua_tostring(L, -1)) == "end") t.target = -2;
		lua_pop(L, 1);
		return true;
	}

	int LuaMod::ScriptSet(lua_State* L)
	{
		const MirageApi& api = *Self(L).api_;
		MirageScript* s = CheckScript(L);
		const auto index = static_cast<int32_t>(luaL_checkinteger(L, 2)) - 1;
		MirageToken t{};
		t.size = sizeof t;
		if (!api.script_get(s, index, &t)) return luaL_error(L, "no token %d", index + 1);
		ToToken(L, 3, t);
		lua_pushboolean(L, api.script_set(s, index, &t));
		return 1;
	}

	int LuaMod::ScriptAdd(lua_State* L)
	{
		const MirageApi& api = *Self(L).api_;
		MirageScript* s = CheckScript(L);
		MirageToken t{};
		t.size = sizeof t;
		t.target = -1;
		ToToken(L, 2, t);
		const int32_t index = api.script_add(s, &t);
		if (index < 0) return luaL_error(L, "the token can't be added (the log says why)");
		// Its operands, if given
		if (lua_getfield(L, 2, "children") == LUA_TTABLE) {
			std::vector<int32_t> kids;
			for (lua_Integer i = 1; lua_rawgeti(L, -1, i) == LUA_TNUMBER; ++i) {
				kids.push_back(static_cast<int32_t>(lua_tointeger(L, -1)) - 1);
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
			if (!api.script_set_children(s, index, kids.data(), static_cast<int32_t>(kids.size())))
				return luaL_error(L, "the token's children are out of range");
		}
		lua_pop(L, 1);
		lua_pushinteger(L, index + 1);
		return 1;
	}

	int LuaMod::ScriptChildren(lua_State* L)
	{
		const MirageApi& api = *Self(L).api_;
		MirageScript* s = CheckScript(L);
		const auto index = static_cast<int32_t>(luaL_checkinteger(L, 2)) - 1;
		const int32_t n = api.script_children(s, index, nullptr, 0);
		if (n < 0) return luaL_error(L, "no token %d", index + 1);
		std::vector<int32_t> kids(static_cast<std::size_t>(n));
		if (n > 0) api.script_children(s, index, kids.data(), n);
		lua_createtable(L, n, 0);
		for (int32_t i = 0; i < n; ++i) {
			lua_pushinteger(L, kids[static_cast<std::size_t>(i)] + 1);
			lua_rawseti(L, -2, i + 1);
		}
		return 1;
	}

	int LuaMod::ScriptSetChildren(lua_State* L)
	{
		const MirageApi& api = *Self(L).api_;
		MirageScript* s = CheckScript(L);
		const auto index = static_cast<int32_t>(luaL_checkinteger(L, 2)) - 1;
		luaL_checktype(L, 3, LUA_TTABLE);
		std::vector<int32_t> kids;
		for (lua_Integer i = 1; lua_rawgeti(L, 3, i) == LUA_TNUMBER; ++i) {
			kids.push_back(static_cast<int32_t>(lua_tointeger(L, -1)) - 1);
			lua_pop(L, 1);
		}
		lua_pop(L, 1);
		lua_pushboolean(L, api.script_set_children(s, index, kids.data(), static_cast<int32_t>(kids.size())));
		return 1;
	}

	int LuaMod::ScriptApply(lua_State* L)
	{
		lua_pushboolean(L, Self(L).api_->script_apply(CheckScript(L)));
		return 1;
	}

	int LuaMod::ScriptDump(lua_State* L)
	{
		const MirageApi& api = *Self(L).api_;
		MirageScript* s = CheckScript(L);
		const uint32_t n = api.script_dump(s, nullptr, 0);
		std::string text(n + 1, '\0');
		api.script_dump(s, text.data(), n + 1);
		text.resize(n);
		lua_pushlstring(L, text.data(), text.size());
		return 1;
	}

	int LuaMod::ScriptClose(lua_State* L)
	{
		auto** box = static_cast<MirageScript**>(luaL_checkudata(L, 1, kScriptMeta));
		if (*box) Self(L).api_->script_close(*box);
		*box = nullptr;
		return 0;
	}

	// mirage: this file's functions (Bind puts every list in)
	const LuaFunctions kFunctions(nullptr, {
		{"notify", LuaMod::Notify}, {"menu_open", LuaMod::MenuOpen}, {"input_key", LuaMod::InputKey},
		{"share", LuaMod::Share}, {"shared", LuaMod::Shared}
	});
	// mirage.script: this file's functions (Bind puts every list in)
	const LuaFunctions kScriptFunctions("script", {
		{"open", LuaMod::ScriptOpen}, {"restore", LuaMod::ScriptRestore}
	});
}
