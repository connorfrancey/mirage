// Lua mods: the `mirage` table, bound to MirageApi only (runtime/mod_lua.h).
//
// One state per mod. Callbacks come from the game thread (on_tick) and the render thread (on_render), so every entry
// into the state holds the mod's lock. Each call gets the state's instruction budget; a callback that errors is
// logged with the mod's name and its module switched off (it would otherwise log the same error every frame).

#include "runtime/lua/lua_mod.h"

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

	void LuaModDeleter::operator()(LuaMod* m) const { delete m; }

	std::size_t LuaMemory(const LuaMod& m) { return m.Memory(); }
	lua::Result EvalLua(LuaMod& m, const std::string& code, std::string& printed) { return m.Eval(code, printed); }

	// require("name"): name.lua (dots for folders) in the mod's folder, run once, its result kept
	int LuaMod::Require(lua_State* L)
	{
		LuaMod& self = Self(L);
		std::string name = luaL_checkstring(L, 1);
		for (char c : name)
			if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
				return luaL_error(L, "require: '%s' is not a module name", name.c_str());
		lua_getfield(L, LUA_REGISTRYINDEX, "mirage.loaded");
		if (lua_getfield(L, -1, name.c_str()) != LUA_TNIL) return 1;
		lua_pop(L, 1);
		std::string rel = name;
		for (char& c : rel)
			if (c == '.') c = '/';
		const auto file = self.dir_ / std::filesystem::path(std::u8string(rel.begin(), rel.end()) + u8".lua");
		std::ifstream in(file, std::ios::binary);
		if (!in) return luaL_error(L, "require: %s.lua is not in the mod's folder", rel.c_str());
		std::stringstream text;
		text << in.rdbuf();
		const std::string code = text.str(), chunk = "=" + rel + ".lua";
		if (luaL_loadbufferx(L, code.data(), code.size(), chunk.c_str(), "t") != LUA_OK) return lua_error(L);
		lua_call(L, 0, 1);
		if (lua_isnil(L, -1)) {
			lua_pop(L, 1);
			lua_pushboolean(L, 1);
		}
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, name.c_str());	// mirage.loaded[name]
		return 1;
	}

	void LuaMod::Bind()
	{
		g_api = api_;
		lua_State* L = state_.Get();
		const auto set = [&](const luaL_Reg* fns) {	// Into the table on top, each with `this` as upvalue
			lua_pushlightuserdata(L, this);
			luaL_setfuncs(L, fns, 1);
		};

		lua_newtable(L);
		lua_setfield(L, LUA_REGISTRYINDEX, "mirage.loaded");

		luaL_newmetatable(L, kModuleMeta);
		lua_newtable(L);
		const luaL_Reg moduleFns[] = {{"enabled", ModuleEnabled}, {"set_enabled", ModuleSetEnabled},
			{"checkbox", ModuleCheckbox}, {"slider", ModuleSlider}, {"slider_int", ModuleSliderInt}, {"text", ModuleText},
			{"key", ModuleKey}, {"color", ModuleColor}, {nullptr, nullptr}};
		set(moduleFns);
		lua_setfield(L, -2, "__index");
		lua_pop(L, 1);

		luaL_newmetatable(L, kSettingMeta);
		lua_newtable(L);
		const luaL_Reg settingFns[] = {{"get", SettingGet}, {"set", SettingSet}, {"down", SettingDown}, {"set_visible", SettingSetVisible},
			{nullptr, nullptr}};
		set(settingFns);
		lua_setfield(L, -2, "__index");
		lua_pop(L, 1);

		luaL_newmetatable(L, kProxyMeta);
		const luaL_Reg proxyFns[] = {{"__index", ProxyIndex}, {"__newindex", ProxyNewIndex}, {"__eq", ProxyEq}, {"__len", ProxyLen},
			{"__tostring", ProxyToString}, {nullptr, nullptr}};
		set(proxyFns);
		lua_pop(L, 1);

		luaL_newmetatable(L, kCallMeta);
		const luaL_Reg callFns[] = {{"__index", CallIndex}, {nullptr, nullptr}};
		set(callFns);
		lua_pop(L, 1);

		luaL_newmetatable(L, kCallbackMeta);
		lua_newtable(L);
		const luaL_Reg callbackFns[] = {{"remove", CallbackRemove}, {nullptr, nullptr}};
		set(callbackFns);
		lua_setfield(L, -2, "__index");
		lua_pop(L, 1);

		luaL_newmetatable(L, kScriptMeta);
		lua_newtable(L);
		const luaL_Reg scriptFns[] = {{"count", ScriptCount}, {"get", ScriptGet}, {"set", ScriptSet}, {"add", ScriptAdd},
			{"children", ScriptChildren}, {"set_children", ScriptSetChildren}, {"apply", ScriptApply}, {"dump", ScriptDump},
			{"close", ScriptClose}, {nullptr, nullptr}};
		set(scriptFns);
		lua_setfield(L, -2, "__index");
		lua_pop(L, 1);

		lua_newtable(L);	// mirage
		// Every area's functions (the LuaFunctions lists of runtime/lua/lua_*.cpp): into `mirage`, or a table of it
		for (const LuaFunctions* list : LuaFunctions::All()) {
			if (list->table) {
				if (lua_getfield(L, -1, list->table) != LUA_TTABLE) {
					lua_pop(L, 1);
					lua_newtable(L);
					lua_pushvalue(L, -1);
					lua_setfield(L, -3, list->table);
				}
			}
			set(list->fns.data());
			if (list->table) lua_pop(L, 1);
		}
		lua_pushstring(L, api_->mod_id(mod_));
		lua_setfield(L, -2, "id");
		lua_pushstring(L, api_->mod_version(mod_));
		lua_setfield(L, -2, "version");
		lua_pushstring(L, api_->mod_dir(mod_));
		lua_setfield(L, -2, "dir");
		lua_pushstring(L, api_->runtime_version);
		lua_setfield(L, -2, "runtime_version");

		lua_ui::Push(L, api_->ui());
		lua_setfield(L, -2, "ui");

		if (!lua::InstallVec(L, -1)) Log(MIRAGE_LOG_ERROR, "mirage.vec didn't install");
		lua_setglobal(L, "mirage");

		lua_pushlightuserdata(L, this);
		lua_pushcclosure(L, Require, 1);
		lua_setglobal(L, "require");
	}

	bool LuaMod::Load(const std::filesystem::path& file, std::string& error)
	{
		if (!state_.Valid()) {
			error = "no Lua state (out of memory)";
			return false;
		}
		dir_ = file.parent_path();
		std::ifstream in(file, std::ios::binary);
		if (!in) {
			error = "can't be read";
			return false;
		}
		std::stringstream text;
		text << in.rdbuf();
		std::lock_guard lock(lock_);
		Bind();
		const auto name = file.filename().u8string();
		const lua::Result r = state_.Run(text.str(), std::string(name.begin(), name.end()));
		if (!r.ok) error = r.error;
		return r.ok;
	}

	LuaModPtr LoadLua(const MirageApi* api, MirageMod* mod, const std::filesystem::path& file, std::string& error)
	{
		LuaModPtr m(new LuaMod(api, mod));
		if (!m->Load(file, error)) return nullptr;
		return m;
	}

	// mirage: this file's functions (Bind puts every list in)
	const LuaFunctions kFunctions(nullptr, {
		{"log", LuaMod::LuaLog<MIRAGE_LOG_INFO>}, {"info", LuaMod::LuaLog<MIRAGE_LOG_INFO>},
		{"warn", LuaMod::LuaLog<MIRAGE_LOG_WARN>}, {"error", LuaMod::LuaLog<MIRAGE_LOG_ERROR>}
	});
}
