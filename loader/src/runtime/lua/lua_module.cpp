// Lua mods: mirage.module{...}: modules, their settings and the mod's config (runtime/lua/lua_mod.h)
#include "runtime/lua/lua_mod.h"

namespace mirage::mods
{
	// ---- mirage.module{...} ----------------------------------------------------------------------------------------
	int LuaMod::LuaModule(lua_State* L)
	{
		LuaMod& self = Self(L);
		luaL_checktype(L, 1, LUA_TTABLE);
		const auto field = [&](const char* key) { return lua_getfield(L, 1, key); };	// Pushes it
		const auto text = [&](const char* key) {
			std::string s;
			if (field(key) == LUA_TSTRING) s = lua_tostring(L, -1);
			lua_pop(L, 1);
			return s;
		};
		const auto integer = [&](const char* key) {
			lua_Integer v = 0;
			if (field(key) == LUA_TNUMBER) v = lua_tointeger(L, -1);
			lua_pop(L, 1);
			return static_cast<int32_t>(v);
		};
		const auto flag = [&](const char* key) {
			field(key);
			const bool v = lua_toboolean(L, -1) != 0;
			lua_pop(L, 1);
			return v;
		};

		auto refs = std::make_unique<ModuleRefs>();
		refs->owner = &self;
		refs->name = text("name");
		if (refs->name.empty()) return luaL_error(L, "mirage.module: name is required");
		for (int h = 0; h < kHooks; ++h) {
			const int type = field(kHookNames[h]);
			if (type == LUA_TFUNCTION) refs->refs[h] = luaL_ref(L, LUA_REGISTRYINDEX);	// Pops it
			else if (type == LUA_TNIL) lua_pop(L, 1);
			else return luaL_error(L, "mirage.module: %s must be a function", kHookNames[h]);
		}
		const std::string tab = text("tab"), description = text("description");

		MirageModuleDesc d{};
		d.size = sizeof d;
		d.name = refs->name.c_str();
		d.tab = tab.empty() ? nullptr : tab.c_str();
		d.description = description.c_str();
		d.order = integer("order");
		d.key = integer("key");
		d.flags = (flag("action") ? MIRAGE_MODULE_ACTION : 0u) | (flag("enabled_by_default") ? MIRAGE_MODULE_ENABLED_BY_DEFAULT : 0u) |
				  (flag("every_tick") ? MIRAGE_MODULE_EVERY_TICK : 0u) | (flag("panel_open") ? MIRAGE_MODULE_PANEL_OPEN : 0u);
		d.user = refs.get();
		d.on_enable = &Invoke<kEnable>;
		d.on_disable = &Invoke<kDisable>;
		d.on_tick = &Invoke<kTick>;
		d.on_render = &Invoke<kRender>;
		d.on_trigger = &Invoke<kTrigger>;
		d.on_panel = refs->refs[kPanel] != LUA_NOREF ? &Invoke<kPanel> : nullptr;
		refs->handle = self.api_->module_register(self.mod_, &d);
		if (!refs->handle) {
			for (int ref : refs->refs) luaL_unref(L, LUA_REGISTRYINDEX, ref);
			return luaL_error(L, "mirage.module: '%s' can't be registered (is the name taken?)", refs->name.c_str());
		}
		auto** box = static_cast<ModuleRefs**>(lua_newuserdatauv(L, sizeof(ModuleRefs*), 0));
		*box = refs.get();
		luaL_setmetatable(L, kModuleMeta);
		self.modules_.push_back(std::move(refs));
		return 1;
	}

	int LuaMod::ModuleEnabled(lua_State* L)
	{
		lua_pushboolean(L, Self(L).api_->module_enabled(CheckModule(L)));
		return 1;
	}
	int LuaMod::ModuleSetEnabled(lua_State* L)
	{
		Self(L).api_->module_set_enabled(CheckModule(L), lua_toboolean(L, 2));
		return 0;
	}
	int LuaMod::ModuleCheckbox(lua_State* L)
	{
		MirageModule* m = CheckModule(L);
		return PushSetting(L, Self(L).api_->setting_checkbox(m, luaL_checkstring(L, 2), lua_toboolean(L, 3), OptString(L, 4)),
						   SettingKind::Checkbox);
	}
	int LuaMod::ModuleSlider(lua_State* L)
	{
		MirageModule* m = CheckModule(L);
		const auto value = static_cast<float>(luaL_checknumber(L, 3));
		const auto min = static_cast<float>(luaL_checknumber(L, 4)), max = static_cast<float>(luaL_checknumber(L, 5));
		return PushSetting(L, Self(L).api_->setting_slider(m, luaL_checkstring(L, 2), value, min, max,
															luaL_optstring(L, 6, "%.2f"), OptString(L, 7)),
						   SettingKind::Slider);
	}
	int LuaMod::ModuleSliderInt(lua_State* L)
	{
		MirageModule* m = CheckModule(L);
		const auto value = static_cast<int32_t>(luaL_checkinteger(L, 3));
		const auto min = static_cast<int32_t>(luaL_checkinteger(L, 4)), max = static_cast<int32_t>(luaL_checkinteger(L, 5));
		return PushSetting(L, Self(L).api_->setting_slider_int(m, luaL_checkstring(L, 2), value, min, max, OptString(L, 6)),
						   SettingKind::SliderInt);
	}
	int LuaMod::ModuleText(lua_State* L)
	{
		MirageModule* m = CheckModule(L);
		return PushSetting(L, Self(L).api_->setting_text(m, luaL_checkstring(L, 2), OptString(L, 3), OptString(L, 4)),
						   SettingKind::Text);
	}
	int LuaMod::ModuleKey(lua_State* L)
	{
		MirageModule* m = CheckModule(L);
		const auto vk = static_cast<int32_t>(luaL_optinteger(L, 3, 0));
		return PushSetting(L, Self(L).api_->setting_key(m, luaL_checkstring(L, 2), vk, OptString(L, 4)), SettingKind::Key);
	}

	// mirage.setting("key"): a setting declared in mod.toml, nil when there is none
	int LuaMod::FindSetting(lua_State* L)
	{
		LuaMod& self = Self(L);
		MirageSetting* h = self.api_->setting_find(self.mod_, luaL_checkstring(L, 1));
		if (!h) {
			lua_pushnil(L);
			return 1;
		}
		SettingKind kind = SettingKind::Text;
		switch (self.api_->setting_type(h)) {
			case MIRAGE_SETTING_BOOL: kind = SettingKind::Checkbox; break;
			case MIRAGE_SETTING_INT: kind = SettingKind::SliderInt; break;
			case MIRAGE_SETTING_FLOAT: kind = SettingKind::Slider; break;
			case MIRAGE_SETTING_CHOICE: kind = SettingKind::Choice; break;
			case MIRAGE_SETTING_KEY: kind = SettingKind::Key; break;
			case MIRAGE_SETTING_COLOR: kind = SettingKind::Color; break;
			default: break;
		}
		return PushSetting(L, h, kind);
	}

	int LuaMod::SettingGet(lua_State* L)
	{
		const auto* box = static_cast<SettingBox*>(luaL_checkudata(L, 1, kSettingMeta));
		const MirageApi* api = Self(L).api_;
		switch (box->kind) {
			case SettingKind::Checkbox: lua_pushboolean(L, api->setting_get_int(box->handle)); break;
			case SettingKind::Slider: lua_pushnumber(L, api->setting_get_float(box->handle)); break;
			case SettingKind::SliderInt: case SettingKind::Key: lua_pushinteger(L, api->setting_get_int(box->handle)); break;
			case SettingKind::Color: lua_pushinteger(L, static_cast<uint32_t>(api->setting_get_int(box->handle))); break;
			case SettingKind::Text: case SettingKind::Choice: {
				const uint32_t n = api->setting_get_text(box->handle, nullptr, 0);
				std::string s(n + 1, '\0');
				api->setting_get_text(box->handle, s.data(), n + 1);
				lua_pushlstring(L, s.data(), n);
				break;
			}
		}
		return 1;
	}
	int LuaMod::SettingSet(lua_State* L)
	{
		const auto* box = static_cast<SettingBox*>(luaL_checkudata(L, 1, kSettingMeta));
		const MirageApi* api = Self(L).api_;
		switch (box->kind) {
			case SettingKind::Checkbox: api->setting_set_int(box->handle, lua_toboolean(L, 2)); break;
			case SettingKind::Slider: api->setting_set_float(box->handle, static_cast<float>(luaL_checknumber(L, 2))); break;
			case SettingKind::SliderInt: api->setting_set_int(box->handle, static_cast<int32_t>(luaL_checkinteger(L, 2))); break;
			case SettingKind::Text: api->setting_set_text(box->handle, luaL_checkstring(L, 2)); break;
			case SettingKind::Key: return luaL_error(L, "a key setting is set by binding it in the menu");
			case SettingKind::Color: api->setting_set_int(box->handle, static_cast<int32_t>(static_cast<uint32_t>(luaL_checkinteger(L, 2)))); break;
			case SettingKind::Choice:
				if (lua_type(L, 2) == LUA_TNUMBER) api->setting_set_int(box->handle, static_cast<int32_t>(luaL_checkinteger(L, 2)) - 1);
				else api->setting_set_text(box->handle, luaL_checkstring(L, 2));
				break;
		}
		return 0;
	}
	int LuaMod::SettingDown(lua_State* L)
	{
		const auto* box = static_cast<SettingBox*>(luaL_checkudata(L, 1, kSettingMeta));
		lua_pushboolean(L, box->kind == SettingKind::Key && Self(L).api_->key_down(box->handle));
		return 1;
	}

	int LuaMod::ConfigGet(lua_State* L)
	{
		LuaMod& self = Self(L);
		const char* key = luaL_checkstring(L, 1);
		const bool hasFallback = !lua_isnoneornil(L, 2);
		const std::string fallback = hasFallback ? luaL_tolstring(L, 2, nullptr) : "";
		if (hasFallback) lua_pop(L, 1);
		const uint32_t n = self.api_->config_get(self.mod_, key, fallback.c_str(), nullptr, 0);
		std::string s(n + 1, '\0');
		self.api_->config_get(self.mod_, key, fallback.c_str(), s.data(), n + 1);
		if (n == 0 && !hasFallback) lua_pushnil(L);
		else lua_pushlstring(L, s.data(), n);
		return 1;
	}
	int LuaMod::ConfigSet(lua_State* L)
	{
		LuaMod& self = Self(L);
		const char* key = luaL_checkstring(L, 1);
		const std::string value = luaL_tolstring(L, 2, nullptr);
		lua_pop(L, 1);
		self.api_->config_set(self.mod_, key, value.c_str());
		return 0;
	}

	// mirage: this file's functions (Bind puts every list in)
	const LuaFunctions kFunctions(nullptr, {
		{"module", LuaMod::LuaModule}, {"setting", LuaMod::FindSetting}
	});
	// mirage.config: this file's functions (Bind puts every list in)
	const LuaFunctions kConfigFunctions("config", {
		{"get", LuaMod::ConfigGet}, {"set", LuaMod::ConfigSet}
	});
}
