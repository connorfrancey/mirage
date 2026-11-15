// Lua mods: the `mirage` table, bound to MirageApi only (runtime/mod_lua.h).
//
// One state per mod. Callbacks come from the game thread (on_tick) and the render thread (on_render), so every entry
// into the state holds the mod's lock. Each call gets the state's instruction budget; a callback that errors is
// logged with the mod's name and its module switched off (it would otherwise log the same error every frame).

#include "runtime/mod_lua.h"

#include "runtime/lua_vm.h"
#include "runtime/mod_lua_ui.h"

#include <lauxlib.h>
#include <lua.h>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace mirage::mods
{
	namespace
	{
		constexpr const char* kModuleMeta = "mirage.module";
		constexpr const char* kSettingMeta = "mirage.setting";
		constexpr const char* kProxyMeta = "mirage.value";
		constexpr const char* kScriptMeta = "mirage.script";		// A function's bytecode, open for patching		// An object, or a struct in an object or a call
		constexpr const char* kCallMeta = "mirage.call";		// What a hook gets: valid during the hook only
		constexpr const char* kCallbackMeta = "mirage.callback";	// A subscription or a hook: :remove()
		enum ProxyKind { kObject = 0, kInObject = 1, kInCall = 2 };
		struct Proxy			// uservalue 1: the path (string); 2: the call box (kInCall)
		{
			int kind;
			void* handle;			// MirageObject* or MirageCall*; an object's is cleared once it is gone (Refresh)
			MirageHandle id;		// kObject / kInObject: what identifies the object (step 14); id.object is kept for ==
		};
		// The runtime's API (the same table for every mod): for what runs outside a Lua C function (event callbacks push
		// objects), where Self(L) has no upvalue to read
		const MirageApi* g_api = nullptr;

		bool IsObjectKind(int kind) { return kind == kObject || kind == kInObject; }
		// An object proxy whose object is gone (or replaced by another at its address) lets go of it: from then on it
		// reads as a dead object (nil values, failed calls), and never reaches the new one
		void Refresh(Proxy* p)
		{
			if (p && IsObjectKind(p->kind) && p->handle && g_api && MIRAGE_API_HAS(g_api, handle_object) &&
				!g_api->handle_object(&p->id))
				p->handle = nullptr;
		}
		struct CallBox
		{
			MirageCall* call;		// nullptr once the hook returned
		};

		enum class SettingKind : std::uint8_t { Checkbox, Slider, SliderInt, Text, Key, Choice, Color };
		struct SettingBox
		{
			MirageSetting* handle;
			SettingKind kind;
		};
		enum Hook { kEnable, kDisable, kTick, kRender, kTrigger, kPanel, kHooks };
		const char* const kHookNames[kHooks] = {"on_enable", "on_disable", "on_tick", "on_render", "on_trigger", "on_panel"};
	}

	class LuaMod
	{
	public:
		struct ModuleRefs		// A module's Lua callbacks: the `user` of its MirageModuleDesc
		{
			LuaMod* owner = nullptr;
			MirageModule* handle = nullptr;
			std::string name;
			int refs[kHooks] = {LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF, LUA_NOREF};
			bool failed = false;
		};

		LuaMod(const MirageApi* api, MirageMod* mod) : api_(api), mod_(mod), state_(32u << 20, 20'000'000) {}

		bool Load(const std::filesystem::path& file, std::string& error);
		std::size_t Memory() const { return state_.MemoryUsed(); }

	private:
		static LuaMod& Self(lua_State* L) { return *static_cast<LuaMod*>(lua_touserdata(L, lua_upvalueindex(1))); }
		void Bind();
		void Log(int level, const std::string& text) { api_->log(mod_, level, text.c_str()); }

		// ---- Callbacks from the runtime ----------------------------------------------------------------------------
		template <int H>
		static void MIRAGE_CALL Invoke(void* user)
		{
			auto* refs = static_cast<ModuleRefs*>(user);
			refs->owner->Run(*refs, H);
		}
		void Run(ModuleRefs& refs, int hook)
		{
			std::lock_guard lock(lock_);
			if (refs.failed || refs.refs[hook] == LUA_NOREF) return;
			lua_State* L = state_.Get();
			lua_rawgeti(L, LUA_REGISTRYINDEX, refs.refs[hook]);
			const lua::Result r = state_.Call(0, 0);
			if (r.ok) return;
			refs.failed = true;
			Log(MIRAGE_LOG_ERROR, refs.name + " " + kHookNames[hook] + ": " + r.error + " (the module is stopped)");
			if (hook != kDisable && refs.handle && api_->module_enabled(refs.handle)) api_->module_set_enabled(refs.handle, 0);
		}

		// ---- The `mirage` table -----------------------------------------------------------------------------------
		static std::string Concat(lua_State* L)
		{
			std::string text;
			for (int i = 1, n = lua_gettop(L); i <= n; ++i) {
				if (i > 1) text += ' ';
				std::size_t len = 0;
				const char* s = luaL_tolstring(L, i, &len);
				text.append(s, len);
				lua_pop(L, 1);
			}
			return text;
		}
		template <int Level>
		static int LuaLog(lua_State* L)
		{
			Self(L).Log(Level, Concat(L));
			return 0;
		}

		static int LuaModule(lua_State* L);
		static int ModuleEnabled(lua_State* L);
		static int ModuleSetEnabled(lua_State* L);
		static int ModuleCheckbox(lua_State* L);
		static int ModuleSlider(lua_State* L);
		static int ModuleSliderInt(lua_State* L);
		static int ModuleText(lua_State* L);
		static int ModuleKey(lua_State* L);
		static int FindSetting(lua_State* L);
		static int SettingGet(lua_State* L);
		static int SettingSet(lua_State* L);
		static int SettingDown(lua_State* L);
		static int ConfigGet(lua_State* L);
		static int ConfigSet(lua_State* L);
		static int DrawText(lua_State* L);
		static int DrawLine(lua_State* L);
		static int DrawRect(lua_State* L);
		static int DrawRectFilled(lua_State* L);
		static int DrawCircle(lua_State* L);
		static int DrawCircleFilled(lua_State* L);
		static int FrameRate(lua_State* L);
		static int DrawTextSized(lua_State* L);
		static int TextSizeSized(lua_State* L);
		static int DrawRectRounded(lua_State* L);
		static int DrawTriangle(lua_State* L);
		static int SettingSetVisible(lua_State* L);
		static int All(lua_State* L);
		static int MenuOpen(lua_State* L);
		static int InputKey(lua_State* L);
		static int Share(lua_State* L);
		static int Shared(lua_State* L);
		static int ModuleColor(lua_State* L);
		static int ScreenSize(lua_State* L);
		static int TextSize(lua_State* L);
		static int Rgba(lua_State* L);
		static int Require(lua_State* L);

		// ---- Game objects, events, hooks (step 6) ------------------------------------------------------------------
		struct Callback			// An event subscription or a function hook's Lua function
		{
			LuaMod* owner = nullptr;
			int fn = LUA_NOREF;
			MirageSubscription* subscription = nullptr;
			MirageHook* hook = nullptr;
			std::string what;
			bool failed = false;
		};
		static void MIRAGE_CALL OnEvent(void* user, const MirageEvent* e);
		static void MIRAGE_CALL OnHook(void* user, MirageCall* call);
		static void MIRAGE_CALL OnNative(void* user, MirageNativeCall* call);
		static int NativeFn(lua_State* L);
		void Fail(Callback& c, const std::string& error);
		static int On(lua_State* L);
		static int HookFn(lua_State* L);
		static int InjectFn(lua_State* L);
		static int CallbackRemove(lua_State* L);
		static int Pawn(lua_State* L);
		static int Controller(lua_State* L);
		static int World(lua_State* L);
		static int Find(lua_State* L);
		static int LoadClass(lua_State* L);
		static int DefineClass(lua_State* L);
		static int PathOf(lua_State* L);
		static int KeepFn(lua_State* L);
		static int LayerActor(lua_State* L);
		static int ReleaseFn(lua_State* L);
		static int Name(lua_State* L);
		static int ClassName(lua_State* L);
		static int IsA(lua_State* L);
		static int Valid(lua_State* L);
		static int Transaction(lua_State* L);
		static int Revert(lua_State* L);
		static int Changes(lua_State* L);
		static int SymbolFn(lua_State* L);
		static int TestFn(lua_State* L);
		static int MovementFn(lua_State* L);
		static int KismetFn(lua_State* L);
		static int KismetLinkFn(lua_State* L);
		static int KismetUnlinkFn(lua_State* L);
		static int KismetCreateFn(lua_State* L);
		static int KismetFireFn(lua_State* L);
		static int KismetTraceFn(lua_State* L);
		static int KismetTraceReadFn(lua_State* L);
		static int TransitionsFn(lua_State* L);
		static int32_t MIRAGE_CALL OnTest(void* user, char* message, uint32_t capacity);
		static int Actors(lua_State* L);
		static int CallGame(lua_State* L);
		static int ProxyIndex(lua_State* L);
		static int ProxyLen(lua_State* L);
		static int ProxyNewIndex(lua_State* L);
		static int ProxyEq(lua_State* L);
		static int ProxyToString(lua_State* L);
		static int CallIndex(lua_State* L);
		static int CallSkip(lua_State* L);
		static MirageScript* CheckScript(lua_State* L);
		static bool ToToken(lua_State* L, int index, MirageToken& t);
		static int Notify(lua_State* L);
		static int ScriptOpen(lua_State* L);
		static int ScriptRestore(lua_State* L);
		static int ScriptCount(lua_State* L);
		static int ScriptGet(lua_State* L);
		static int ScriptSet(lua_State* L);
		static int ScriptAdd(lua_State* L);
		static int ScriptChildren(lua_State* L);
		static int ScriptSetChildren(lua_State* L);
		static int ScriptApply(lua_State* L);
		static int ScriptDump(lua_State* L);
		static int ScriptClose(lua_State* L);
		static void PushObject(lua_State* L, MirageObject* o);
		static void PushProxy(lua_State* L, int kind, void* handle, const std::string& path, int owner);
		static void PushValue(lua_State* L, const MirageValue& v, int kind, void* handle, const std::string& path, int owner);
		static bool ToValue(lua_State* L, int index, MirageValue& out);
		static MirageObject* CheckObject(lua_State* L, int index);
		int Set(lua_State* L, int kind, void* handle, const std::string& path, int value);

		static MirageModule* CheckModule(lua_State* L)
		{
			auto** refs = static_cast<ModuleRefs**>(luaL_checkudata(L, 1, kModuleMeta));
			if (!(*refs)->handle) luaL_error(L, "the module %s wasn't registered", (*refs)->name.c_str());
			return (*refs)->handle;
		}
		static int PushSetting(lua_State* L, MirageSetting* handle, SettingKind kind)
		{
			if (!handle) {
				lua_pushnil(L);
				return 1;
			}
			auto* box = static_cast<SettingBox*>(lua_newuserdatauv(L, sizeof(SettingBox), 0));
			*box = {handle, kind};
			luaL_setmetatable(L, kSettingMeta);
			return 1;
		}
		static const char* OptString(lua_State* L, int index) { return luaL_optstring(L, index, ""); }

		const MirageApi* api_;
		MirageMod* mod_;
		lua::State state_;
		std::recursive_mutex lock_;		// Recursive: a callback may switch its module off, which calls on_disable
		std::vector<std::unique_ptr<ModuleRefs>> modules_;
		std::vector<std::unique_ptr<Callback>> callbacks_;		// Events and hooks (kept until the mod unloads)
		std::filesystem::path dir_;
	};

	void LuaModDeleter::operator()(LuaMod* m) const { delete m; }

	std::size_t LuaMemory(const LuaMod& m) { return m.Memory(); }

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

	namespace
	{
		uint32_t Color(lua_State* L, int index)
		{
			return static_cast<uint32_t>(luaL_optinteger(L, index, 0xFFFFFFFF));
		}
		float Num(lua_State* L, int index) { return static_cast<float>(luaL_checknumber(L, index)); }
	}

	// mirage.draw.text_sized(x, y, color, size, text); text_size_sized(text, size) -> w, h
	int LuaMod::DrawTextSized(lua_State* L)
	{
		Self(L).api_->draw_text_sized(Num(L, 1), Num(L, 2), Color(L, 3), Num(L, 4), luaL_checkstring(L, 5));
		return 0;
	}
	int LuaMod::TextSizeSized(lua_State* L)
	{
		float w = 0.f, h = 0.f;
		Self(L).api_->text_size_sized(luaL_checkstring(L, 1), Num(L, 2), &w, &h);
		lua_pushnumber(L, w);
		lua_pushnumber(L, h);
		return 2;
	}
	// mirage.draw.rect_rounded(x1, y1, x2, y2, color, rounding, thickness (0: filled))
	int LuaMod::DrawRectRounded(lua_State* L)
	{
		Self(L).api_->draw_rect_rounded(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), Color(L, 5),
										static_cast<float>(luaL_optnumber(L, 6, 0.0)), static_cast<float>(luaL_optnumber(L, 7, 0.0)));
		return 0;
	}
	int LuaMod::DrawTriangle(lua_State* L)
	{
		Self(L).api_->draw_triangle_filled(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), Num(L, 5), Num(L, 6), Color(L, 7));
		return 0;
	}
	// setting:set_visible(bool)
	int LuaMod::SettingSetVisible(lua_State* L)
	{
		const auto* box = static_cast<SettingBox*>(luaL_checkudata(L, 1, kSettingMeta));
		Self(L).api_->setting_set_visible(box->handle, lua_toboolean(L, 2));
		return 0;
	}

	int LuaMod::DrawText(lua_State* L)
	{
		Self(L).api_->draw_text(Num(L, 1), Num(L, 2), Color(L, 3), luaL_checkstring(L, 4));
		return 0;
	}
	int LuaMod::DrawLine(lua_State* L)
	{
		Self(L).api_->draw_line(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), Color(L, 5),
								static_cast<float>(luaL_optnumber(L, 6, 1.0)));
		return 0;
	}
	int LuaMod::DrawRect(lua_State* L)
	{
		Self(L).api_->draw_rect(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), Color(L, 5),
								static_cast<float>(luaL_optnumber(L, 6, 1.0)));
		return 0;
	}
	int LuaMod::DrawRectFilled(lua_State* L)
	{
		Self(L).api_->draw_rect_filled(Num(L, 1), Num(L, 2), Num(L, 3), Num(L, 4), Color(L, 5));
		return 0;
	}
	int LuaMod::DrawCircle(lua_State* L)
	{
		Self(L).api_->draw_circle(Num(L, 1), Num(L, 2), Num(L, 3), Color(L, 4), static_cast<float>(luaL_optnumber(L, 5, 1.0)));
		return 0;
	}
	int LuaMod::DrawCircleFilled(lua_State* L)
	{
		Self(L).api_->draw_circle_filled(Num(L, 1), Num(L, 2), Num(L, 3), Color(L, 4));
		return 0;
	}
	int LuaMod::FrameRate(lua_State* L)
	{
		lua_pushnumber(L, Self(L).api_->frame_rate());
		return 1;
	}
	int LuaMod::ModuleColor(lua_State* L)
	{
		MirageModule* m = CheckModule(L);
		const auto rgba = static_cast<uint32_t>(luaL_optinteger(L, 3, 0xFFFFFFFF));
		return PushSetting(L, Self(L).api_->setting_color(m, luaL_checkstring(L, 2), rgba, OptString(L, 4)), SettingKind::Color);
	}
	int LuaMod::ScreenSize(lua_State* L)
	{
		float w = 0, h = 0;
		Self(L).api_->screen_size(&w, &h);
		lua_pushnumber(L, w);
		lua_pushnumber(L, h);
		return 2;
	}
	int LuaMod::TextSize(lua_State* L)
	{
		float w = 0, h = 0;
		Self(L).api_->text_size(luaL_checkstring(L, 1), &w, &h);
		lua_pushnumber(L, w);
		lua_pushnumber(L, h);
		return 2;
	}
	int LuaMod::Rgba(lua_State* L)
	{
		const auto part = [&](int i, lua_Integer fallback) {
			const lua_Integer v = luaL_optinteger(L, i, fallback);
			return static_cast<uint32_t>(v < 0 ? 0 : v > 255 ? 255 : v);
		};
		lua_pushinteger(L, static_cast<lua_Integer>((part(4, 255) << 24) | (part(3, 0) << 16) | (part(2, 0) << 8) | part(1, 0)));
		return 1;
	}

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

	// ---- Game objects --------------------------------------------------------------------------------------------
	void LuaMod::PushProxy(lua_State* L, int kind, void* handle, const std::string& path, int owner)
	{
		auto* p = static_cast<Proxy*>(lua_newuserdatauv(L, sizeof(Proxy), 2));
		p->kind = kind;
		p->handle = handle;
		p->id = MirageHandle{};
		if (IsObjectKind(kind) && handle && g_api && MIRAGE_API_HAS(g_api, object_handle)) {
			p->id.size = sizeof(MirageHandle);
			if (!g_api->object_handle(static_cast<MirageObject*>(handle), &p->id)) p->handle = nullptr;	// Already gone
			p->id.object = static_cast<MirageObject*>(handle);
		}
		lua_pushlstring(L, path.data(), path.size());
		lua_setiuservalue(L, -2, 1);
		if (owner) {
			lua_pushvalue(L, owner);
			lua_setiuservalue(L, -2, 2);
		}
		luaL_setmetatable(L, kProxyMeta);
	}

	void LuaMod::PushObject(lua_State* L, MirageObject* o)
	{
		if (!o) lua_pushnil(L);
		else PushProxy(L, kObject, o, "", 0);
	}

	// A value as Lua sees it: numbers, booleans, strings, objects; a struct is a proxy reaching into it
	void LuaMod::PushValue(lua_State* L, const MirageValue& v, int kind, void* handle, const std::string& path, int owner)
	{
		switch (v.type) {
			case MIRAGE_VALUE_BOOL: lua_pushboolean(L, v.i); break;
			case MIRAGE_VALUE_INT: lua_pushinteger(L, v.i); break;
			case MIRAGE_VALUE_FLOAT: lua_pushnumber(L, v.f); break;
			case MIRAGE_VALUE_OBJECT: PushObject(L, v.o); break;
			case MIRAGE_VALUE_TEXT: lua_pushstring(L, v.text); break;
			case MIRAGE_VALUE_STRUCT: case MIRAGE_VALUE_ARRAY:
				PushProxy(L, kind == kObject ? kInObject : kind, handle, path, owner);
				break;
			default: lua_pushnil(L); break;
		}
	}

	MirageObject* LuaMod::CheckObject(lua_State* L, int index)
	{
		auto* p = static_cast<Proxy*>(luaL_testudata(L, index, kProxyMeta));
		if (!p || p->kind != kObject) luaL_typeerror(L, index, "game object");
		Refresh(p);
		return static_cast<MirageObject*>(p->handle);
	}

	bool LuaMod::ToValue(lua_State* L, int index, MirageValue& out)
	{
		out = MirageValue{};
		switch (lua_type(L, index)) {
			case LUA_TBOOLEAN: out.type = MIRAGE_VALUE_BOOL; out.i = lua_toboolean(L, index); return true;
			case LUA_TNUMBER:
				if (lua_isinteger(L, index)) {
					out.type = MIRAGE_VALUE_INT;
					out.i = static_cast<int32_t>(lua_tointeger(L, index));
				} else {
					out.type = MIRAGE_VALUE_FLOAT;
					out.f = static_cast<float>(lua_tonumber(L, index));
				}
				return true;
			case LUA_TSTRING: {
				out.type = MIRAGE_VALUE_TEXT;
				std::size_t n = 0;
				const char* t = lua_tolstring(L, index, &n);
				std::memcpy(out.text, t, std::min<std::size_t>(n, sizeof out.text - 1));
				return true;
			}
			case LUA_TNIL: out.type = MIRAGE_VALUE_OBJECT; return true;
			case LUA_TUSERDATA:
				if (auto* p = static_cast<Proxy*>(luaL_testudata(L, index, kProxyMeta)); p && p->kind == kObject) {
					Refresh(p);
					out.type = MIRAGE_VALUE_OBJECT;
					out.o = static_cast<MirageObject*>(p->handle);
					return true;
				}
				return false;
			default: return false;
		}
	}

	// Writes the Lua value at `value` to `path`; a table sets the struct's members one by one
	int LuaMod::Set(lua_State* L, int kind, void* handle, const std::string& path, int value)
	{
		if (lua_type(L, value) == LUA_TTABLE) {
			lua_pushnil(L);
			while (lua_next(L, value)) {
				if (lua_type(L, -2) == LUA_TSTRING) Set(L, kind, handle, path + "." + lua_tostring(L, -2), lua_gettop(L));
				lua_pop(L, 1);
			}
			return 0;
		}
		MirageValue v;
		if (!ToValue(L, value, v)) return luaL_error(L, "%s: can't be set to a %s", path.c_str(), luaL_typename(L, value));
		const int ok = kind == kInCall ? api_->call_set(static_cast<MirageCall*>(handle), path.c_str(), &v)
									   : api_->object_set(static_cast<MirageObject*>(handle), path.c_str(), &v);
		if (!ok) return luaL_error(L, "%s can't be set (no such member, another type, or the object is gone)", path.c_str());
		return 0;
	}

	namespace
	{
		// The proxy at 1, with its path and owner (a call box, or 0) on the stack
		struct ProxyAt
		{
			Proxy* p;
			std::string path;
			int owner = 0;
		};
		ProxyAt OpenProxy(lua_State* L)
		{
			ProxyAt r{static_cast<Proxy*>(luaL_checkudata(L, 1, kProxyMeta)), {}};
			lua_getiuservalue(L, 1, 1);
			r.path = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
			lua_pop(L, 1);
			if (r.p->kind == kInCall) {
				lua_getiuservalue(L, 1, 2);
				auto* box = static_cast<CallBox*>(lua_touserdata(L, -1));
				if (!box || !box->call) luaL_error(L, "a hook's parameters are only there while the hook runs");
				r.owner = lua_gettop(L);
			}
			return r;
		}
		std::string Join(const std::string& path, const char* key) { return path.empty() ? key : path + "." + key; }
		// t.Name or t[1] (Lua's 1-based index: the path's [0])
		std::string Key(lua_State* L, const std::string& path)
		{
			if (lua_type(L, 2) == LUA_TNUMBER) {
				const lua_Integer i = luaL_checkinteger(L, 2);
				return path + "[" + std::to_string(i - 1) + "]";
			}
			return Join(path, luaL_checkstring(L, 2));
		}
	}

	int LuaMod::ProxyIndex(lua_State* L)
	{
		LuaMod& self = Self(L);
		const ProxyAt at = OpenProxy(L);
		Refresh(at.p);
		if (lua_type(L, 2) == LUA_TNUMBER && luaL_checkinteger(L, 2) < 1) {
			lua_pushnil(L);
			return 1;
		}
		const std::string path = Key(L, at.path);
		MirageValue v;
		const int ok = at.p->kind == kInCall ? self.api_->call_get(static_cast<MirageCall*>(at.p->handle), path.c_str(), &v)
											 : self.api_->object_get(static_cast<MirageObject*>(at.p->handle), path.c_str(), &v);
		if (!ok) {
			lua_pushnil(L);
			return 1;
		}
		PushValue(L, v, at.p->kind, at.p->handle, path, at.owner);
		return 1;
	}

	int LuaMod::ProxyNewIndex(lua_State* L)
	{
		const ProxyAt at = OpenProxy(L);
		Refresh(at.p);
		return Self(L).Set(L, at.p->kind, at.p->handle, Key(L, at.path), 3);
	}

	// #array
	int LuaMod::ProxyLen(lua_State* L)
	{
		LuaMod& self = Self(L);
		const ProxyAt at = OpenProxy(L);
		Refresh(at.p);
		MirageValue v;
		const int ok = at.p->kind == kInCall ? self.api_->call_get(static_cast<MirageCall*>(at.p->handle), at.path.c_str(), &v)
											 : self.api_->object_get(static_cast<MirageObject*>(at.p->handle), at.path.c_str(), &v);
		lua_pushinteger(L, ok && v.type == MIRAGE_VALUE_ARRAY ? v.i : 0);
		return 1;
	}

	int LuaMod::ProxyEq(lua_State* L)
	{
		auto* a = static_cast<Proxy*>(luaL_testudata(L, 1, kProxyMeta));
		auto* b = static_cast<Proxy*>(luaL_testudata(L, 2, kProxyMeta));
		// The same object: compared by the address it had (a removed actor's proxy equals the one a mod kept of it)
		const auto address = [](const Proxy* p) { return p->id.object ? static_cast<void*>(p->id.object) : p->handle; };
		lua_pushboolean(L, a && b && a->kind == kObject && b->kind == kObject && address(a) == address(b));
		return 1;
	}

	int LuaMod::ProxyToString(lua_State* L)
	{
		LuaMod& self = Self(L);
		const ProxyAt at = OpenProxy(L);
		Refresh(at.p);
		if (at.p->kind == kObject) {
			char name[128];
			self.api_->object_name(static_cast<MirageObject*>(at.p->handle), name, sizeof name);
			lua_pushstring(L, name);
		} else {
			lua_pushfstring(L, "<%s>", at.path.c_str());
		}
		return 1;
	}

	int LuaMod::Pawn(lua_State* L) { PushObject(L, Self(L).api_->player_pawn()); return 1; }
	int LuaMod::Controller(lua_State* L) { PushObject(L, Self(L).api_->player_controller()); return 1; }
	int LuaMod::World(lua_State* L) { PushObject(L, Self(L).api_->world_info()); return 1; }
	int LuaMod::Find(lua_State* L) { PushObject(L, Self(L).api_->object_find(luaL_checkstring(L, 1))); return 1; }

	// mirage.load_class("classes/MirageBouncePad.uc") -> the class, compiled from UnrealScript and loaded by the engine
	// (nil and the reason when it doesn't compile or load). Game thread.
	int LuaMod::LoadClass(lua_State* L)
	{
		LuaMod& self = Self(L);
		const char* path = luaL_checkstring(L, 1);
		char error[1024]{};
		MirageObject* cls = self.api_->class_load(self.mod_, path, error, sizeof(error));
		if (!cls) {
			lua_pushnil(L);
			lua_pushstring(L, error);
			return 2;
		}
		PushObject(L, cls);
		return 1;
	}

	// mirage.define_class { name = "MyPad", extends = "Trigger", placeable = true,
	//     vars = { "float Strength", "int Bounces" }, defaults = { Strength = 1500, bNoDelete = false },
	//     functions = { Touch = function(self, params, call) ... end,
	//                   Count = { "function int Count()", function(self) return 3 end } },
	//     extra = "<more UnrealScript>" }
	// -> the class. The runtime writes and compiles the class; each function's code is the Lua function, called at the
	// head of its (empty) script body with the object, its parameters (read and written by name) and the call; a value it
	// returns is the function's return value. Game thread (a tick, an event).
	int LuaMod::DefineClass(lua_State* L)
	{
		LuaMod& self = Self(L);
		luaL_checktype(L, 1, LUA_TTABLE);
		nlohmann::json spec;
		const auto field = [&](const char* key) -> std::string {
			lua_getfield(L, 1, key);
			std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
			lua_pop(L, 1);
			return v;
		};
		spec["name"] = field("name");
		spec["extends"] = field("extends");
		lua_getfield(L, 1, "placeable");
		spec["placeable"] = lua_toboolean(L, -1) != 0;
		lua_pop(L, 1);
		spec["extra"] = field("extra");
		// vars: a list of declarations
		spec["vars"] = nlohmann::json::array();
		lua_getfield(L, 1, "vars");
		if (lua_istable(L, -1))
			for (lua_Integer i = 1; lua_rawgeti(L, -1, i) != LUA_TNIL; ++i) {
				spec["vars"].push_back(lua_tostring(L, -1));
				lua_pop(L, 1);
			}
		lua_pop(L, 2);
		// defaults: { Name = value } or a list of "Name=Value" lines
		spec["defaults"] = nlohmann::json::array();
		bool keyed = false;		// { Name = value } (no order of its own) rather than a list of lines
		lua_getfield(L, 1, "defaults");
		if (lua_istable(L, -1) && lua_rawlen(L, -1) > 0) {		// A list of lines, in its order
			for (lua_Integer i = 1; lua_rawgeti(L, -1, i) != LUA_TNIL; ++i) {
				spec["defaults"].push_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : "");
				lua_pop(L, 1);
			}
			lua_pop(L, 1);
		} else if (lua_istable(L, -1)) {
			lua_pushnil(L);
			while (lua_next(L, -2)) {
				std::string value;
				if (lua_isboolean(L, -1)) value = lua_toboolean(L, -1) ? "True" : "False";
				else if (lua_isinteger(L, -1)) value = std::to_string(lua_tointeger(L, -1));
				else if (lua_type(L, -1) == LUA_TNUMBER) value = fmt::format("{}", lua_tonumber(L, -1));
				else if (lua_isstring(L, -1)) value = lua_tostring(L, -1);
				if (lua_type(L, -2) == LUA_TSTRING) {
					spec["defaults"].push_back(std::string(lua_tostring(L, -2)) + "=" + value);
					keyed = true;
				}
				else spec["defaults"].push_back(value);
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);
		// pairs() has no fixed order: sorted, the same description gives the same source (and the same package and
		// class after a hot reload)
		if (keyed) {
			std::vector<std::string> d = spec["defaults"].get<std::vector<std::string>>();
			std::sort(d.begin(), d.end());
			spec["defaults"] = d;
		}
		// functions: { Name = fn } (an override) or { Name = { "declaration", fn } } (a new one)
		struct Handler
		{
			std::string name;
			int ref;
		};
		std::vector<Handler> handlers;
		spec["functions"] = nlohmann::json::array();
		lua_getfield(L, 1, "functions");
		if (lua_istable(L, -1)) {
			lua_pushnil(L);
			while (lua_next(L, -2)) {
				if (lua_type(L, -2) != LUA_TSTRING) {
					lua_pop(L, 1);
					continue;
				}
				const std::string name = lua_tostring(L, -2);
				if (lua_isfunction(L, -1)) {
					spec["functions"].push_back(name);
					lua_pushvalue(L, -1);
				} else if (lua_istable(L, -1)) {
					lua_rawgeti(L, -1, 1);
					spec["functions"].push_back(lua_isstring(L, -1) ? lua_tostring(L, -1) : name);
					lua_pop(L, 1);
					lua_rawgeti(L, -1, 2);
				} else {
					lua_pushnil(L);
				}
				if (lua_isfunction(L, -1)) handlers.push_back({name, luaL_ref(L, LUA_REGISTRYINDEX)});
				else lua_pop(L, 1);
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);
		{
			std::vector<std::string> f = spec["functions"].get<std::vector<std::string>>();
			std::sort(f.begin(), f.end());
			spec["functions"] = f;
		}

		char error[2048]{};
		MirageObject* cls = self.api_->class_define(self.mod_, spec.dump().c_str(), error, sizeof(error));
		if (!cls) {
			for (const Handler& h : handlers) luaL_unref(L, LUA_REGISTRYINDEX, h.ref);
			return luaL_error(L, "mirage.define_class %s: %s", spec["name"].get<std::string>().c_str(), error);
		}
		char path[512]{};
		self.api_->object_path(cls, path, sizeof(path));
		// Each handler, behind a wrapper: (self, params, call), its result the return value
		static const char* kWrap =
			"local h = ...\n"
			"return function(call)\n"
			"  local r = h(call.object, call.params, call)\n"
			"  if r ~= nil then call.params.ReturnValue = r; call:skip() end\n"
			"end\n";
		for (const Handler& h : handlers) {
			if (luaL_loadstring(L, kWrap) != LUA_OK) return lua_error(L);
			lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
			lua_call(L, 1, 1);
			luaL_unref(L, LUA_REGISTRYINDEX, h.ref);
			const std::string function = std::string(path) + "." + h.name;
			auto c = std::make_unique<Callback>();
			c->owner = &self;
			c->what = "class " + spec["name"].get<std::string>() + "'s " + h.name;
			c->fn = luaL_ref(L, LUA_REGISTRYINDEX);
			c->hook = self.api_->inject_function(self.mod_, function.c_str(), "head", 0, &OnHook, c.get());
			if (!c->hook) {
				luaL_unref(L, LUA_REGISTRYINDEX, c->fn);
				return luaL_error(L, "mirage.define_class: %s can't take its Lua code (see the log)", function.c_str());
			}
			self.callbacks_.push_back(std::move(c));
		}
		PushObject(L, cls);
		return 1;
	}

	// mirage.path(object) -> "Package.Outer.Name"
	int LuaMod::PathOf(lua_State* L)
	{
		char path[512]{};
		Self(L).api_->object_path(CheckObject(L, 1), path, sizeof(path));
		lua_pushstring(L, path);
		return 1;
	}
	// mirage.layer_actor("PadA" [, "other-mod"]) -> the actor of that name in a level layer (levels/<map>.toml), or nil
	int LuaMod::LayerActor(lua_State* L)
	{
		LuaMod& self = Self(L);
		PushObject(L, self.api_->layer_actor(self.mod_, luaL_optstring(L, 2, ""), luaL_checkstring(L, 1)));
		return 1;
	}

	// mirage.keep(object): kept from the garbage collector until mirage.release(object) or the mod's unload
	int LuaMod::KeepFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		lua_pushboolean(L, self.api_->object_keep(self.mod_, CheckObject(L, 1), 1));
		return 1;
	}
	int LuaMod::ReleaseFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		lua_pushboolean(L, self.api_->object_keep(self.mod_, CheckObject(L, 1), 0));
		return 1;
	}

	// mirage.all("GameEngine" [, defaults]) -> a list of the live objects of the class (and its subclasses)
	int LuaMod::All(lua_State* L)
	{
		const char* cls = luaL_checkstring(L, 1);
		const uint32_t flags = lua_toboolean(L, 2) ? MIRAGE_OBJECTS_DEFAULTS : 0u;
		const MirageApi& api = *Self(L).api_;
		std::vector<MirageObject*> found(64);
		int32_t n = api.objects_of_class(cls, flags, found.data(), static_cast<int32_t>(found.size()));
		if (n > static_cast<int32_t>(found.size())) {
			found.resize(static_cast<std::size_t>(n));
			n = std::min<int32_t>(api.objects_of_class(cls, flags, found.data(), n), n);
		}
		lua_createtable(L, std::max(n, 0), 0);
		for (int32_t i = 0; i < n; ++i) {
			PushObject(L, found[static_cast<std::size_t>(i)]);
			lua_rawseti(L, -2, i + 1);
		}
		return 1;
	}

	int LuaMod::Name(lua_State* L)
	{
		char name[256];
		Self(L).api_->object_name(CheckObject(L, 1), name, sizeof name);
		lua_pushstring(L, name);
		return 1;
	}
	int LuaMod::ClassName(lua_State* L)
	{
		char name[256];
		Self(L).api_->object_class_name(CheckObject(L, 1), name, sizeof name);
		lua_pushstring(L, name);
		return 1;
	}
	int LuaMod::IsA(lua_State* L)
	{
		lua_pushboolean(L, Self(L).api_->object_is_a(CheckObject(L, 1), luaL_checkstring(L, 2)));
		return 1;
	}

	// mirage.transaction("name", function() ... end) -> its id: the writes the function makes to game objects are
	// recorded with the values they replace, and put back by mirage.revert(id) or when the mod unloads (step 15). An
	// error in the function still closes the transaction (what it wrote is in it), then goes on as an error.
	int LuaMod::Transaction(lua_State* L)
	{
		LuaMod& self = Self(L);
		const char* name = luaL_checkstring(L, 1);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		const int id = self.api_->change_begin(self.mod_, name);
		if (!id) return luaL_error(L, "mirage.transaction: one is open already (they don't nest), or not on the game thread");
		lua_pushvalue(L, 2);
		const int status = lua_pcall(L, 0, 0, 0);
		self.api_->change_end(self.mod_);
		if (status != LUA_OK) return lua_error(L);
		lua_pushinteger(L, id);
		return 1;
	}

	// mirage.revert([id]) -> how many values were put back (no id: every transaction of the mod not reverted yet)
	int LuaMod::Revert(lua_State* L)
	{
		LuaMod& self = Self(L);
		lua_pushinteger(L, self.api_->change_revert(self.mod_, static_cast<int32_t>(luaL_optinteger(L, 1, -1))));
		return 1;
	}

	// mirage.changes() -> { { id, name, values = { "Object.Path", ... }, reverted }, ... }, oldest first
	// mirage.symbol(name): the address of the game's function or global by name (an integer), nil if unknown. Lua can't
	// detour native code itself; this is for reading what a native mod hooks, and for the developer's eye.
	int LuaMod::SymbolFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		void* at = MIRAGE_API_HAS(self.api_, symbol_find) ? self.api_->symbol_find(luaL_checkstring(L, 1)) : nullptr;
		if (!at) lua_pushnil(L);
		else lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(at)));
		return 1;
	}

	// mirage.test("name", function() ... end): a test the self-test runs ("Mod tests", `mirage mod test`). It passes
	// unless it raises an error or returns false (and a reason): return false, "the jump is normal"
	int LuaMod::TestFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		const char* name = luaL_checkstring(L, 1);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		if (!MIRAGE_API_HAS(self.api_, test_register)) return luaL_error(L, "mirage.test: this runtime has no mod tests");
		auto c = std::make_unique<Callback>();
		c->owner = &self;
		c->what = std::string("test ") + name;
		lua_pushvalue(L, 2);
		c->fn = luaL_ref(L, LUA_REGISTRYINDEX);
		if (!self.api_->test_register(self.mod_, name, &OnTest, c.get())) return luaL_error(L, "mirage.test: %s wasn't registered", name);
		self.callbacks_.push_back(std::move(c));
		return 0;
	}

	int32_t MIRAGE_CALL LuaMod::OnTest(void* user, char* message, uint32_t capacity)
	{
		auto* c = static_cast<Callback*>(user);
		LuaMod& self = *c->owner;
		std::lock_guard lock(self.lock_);
		lua_State* L = self.state_.Get();
		const int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, c->fn);
		const lua::Result r = self.state_.Call(0, 2);
		std::string why;
		if (!r.ok) why = r.error;
		else if (lua_isboolean(L, -2) && !lua_toboolean(L, -2)) why = lua_isstring(L, -1) ? lua_tostring(L, -1) : "it returned false";
		lua_settop(L, top);
		if (why.empty()) return 1;
		std::snprintf(message, capacity, "%s", why.c_str());
		return 0;
	}

	// mirage.movement(): what the player is doing, {move, previous, physics, velocity = {x, y, z}, speed, object}, or
	// nil without a player. move and previous are EMovement values (the SDK's TdGame enum); object is the current move.
	int LuaMod::MovementFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		MirageMovement m{};
		m.size = sizeof m;
		if (!MIRAGE_API_HAS(self.api_, movement_get) || !self.api_->movement_get(&m)) {
			lua_pushnil(L);
			return 1;
		}
		lua_createtable(L, 0, 6);
		lua_pushinteger(L, m.move);
		lua_setfield(L, -2, "move");
		lua_pushinteger(L, m.previous_move);
		lua_setfield(L, -2, "previous");
		lua_pushinteger(L, m.physics);
		lua_setfield(L, -2, "physics");
		lua_createtable(L, 0, 3);
		lua_pushnumber(L, m.velocity[0]);
		lua_setfield(L, -2, "x");
		lua_pushnumber(L, m.velocity[1]);
		lua_setfield(L, -2, "y");
		lua_pushnumber(L, m.velocity[2]);
		lua_setfield(L, -2, "z");
		lua_setfield(L, -2, "velocity");
		lua_pushnumber(L, m.speed);
		lua_setfield(L, -2, "speed");
		self.PushObject(L, m.move_object);
		lua_setfield(L, -2, "object");
		return 1;
	}

	namespace
	{
		void PushJson(lua_State* L, const nlohmann::json& j)
		{
			switch (j.type()) {
				case nlohmann::json::value_t::object:
					lua_createtable(L, 0, static_cast<int>(j.size()));
					for (auto it = j.begin(); it != j.end(); ++it) {
						PushJson(L, it.value());
						lua_setfield(L, -2, it.key().c_str());
					}
					break;
				case nlohmann::json::value_t::array: {
					lua_createtable(L, static_cast<int>(j.size()), 0);
					lua_Integer i = 1;
					for (const auto& v : j) {
						PushJson(L, v);
						lua_rawseti(L, -2, i++);
					}
					break;
				}
				case nlohmann::json::value_t::string: lua_pushstring(L, j.get_ref<const std::string&>().c_str()); break;
				case nlohmann::json::value_t::boolean: lua_pushboolean(L, j.get<bool>()); break;
				case nlohmann::json::value_t::number_integer:
				case nlohmann::json::value_t::number_unsigned: lua_pushinteger(L, j.get<lua_Integer>()); break;
				case nlohmann::json::value_t::number_float: lua_pushnumber(L, j.get<double>()); break;
				default: lua_pushnil(L);
			}
		}
	}

	// mirage.kismet(level): the loaded levels' sequences (or only `level`'s, its package name) as tables: a list of
	// {path, name, level, parent, enabled, ops = {{path, name, class, kind, title, ..., outputs = {{desc, links = {{op,
	// input}}}}}}}: api.h kismet_read has every field
	int LuaMod::KismetFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, kismet_read)) return luaL_error(L, "mirage.kismet: this runtime can't read Kismet");
		const char* level = luaL_optstring(L, 1, "");
		std::string text(self.api_->kismet_read(level, nullptr, 0) + 1, '\0');
		text.resize(self.api_->kismet_read(level, text.data(), static_cast<uint32_t>(text.size())));
		PushJson(L, nlohmann::json::parse(text, nullptr, false));
		return 1;
	}

	// mirage.kismet_link(from, output, to, input) / mirage.kismet_unlink(...): `output` and `input` a link's description
	// ("Touched", "In") or its index (0-based); nil: the first. True when done; recorded in the open transaction (or one
	// of its own), so the graph is put back on revert and unload.
	int LuaMod::KismetLinkFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, kismet_link)) return luaL_error(L, "mirage.kismet_link: this runtime can't edit Kismet");
		MirageObject* from = CheckObject(L, 1);
		MirageObject* to = CheckObject(L, 3);
		lua_pushboolean(L, self.api_->kismet_link(self.mod_, from, luaL_optstring(L, 2, ""), to, luaL_optstring(L, 4, "")) != 0);
		return 1;
	}
	int LuaMod::KismetUnlinkFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, kismet_unlink)) return luaL_error(L, "mirage.kismet_unlink: this runtime can't edit Kismet");
		MirageObject* from = CheckObject(L, 1);
		MirageObject* to = CheckObject(L, 3);
		lua_pushboolean(L, self.api_->kismet_unlink(self.mod_, from, luaL_optstring(L, 2, ""), to, luaL_optstring(L, 4, "")) != 0);
		return 1;
	}
	// mirage.kismet_create(sequence, class): a new op of `class` (a SequenceOp class name or path, a mod's defined class)
	// in `sequence`, or nil (the log says why)
	int LuaMod::KismetCreateFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, kismet_create)) return luaL_error(L, "mirage.kismet_create: this runtime can't edit Kismet");
		MirageObject* sequence = CheckObject(L, 1);
		MirageObject* op = self.api_->kismet_create(self.mod_, sequence, luaL_checkstring(L, 2));
		if (op) PushObject(L, op);
		else lua_pushnil(L);
		return 1;
	}

	// mirage.kismet_fire(event, output, instigator): fires a Kismet event as the engine does (output nil: the first,
	// "Touched" or an index; instigator nil: the player). True when it activated.
	int LuaMod::KismetFireFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, kismet_fire)) return luaL_error(L, "mirage.kismet_fire: this runtime can't fire Kismet events");
		MirageObject* event = CheckObject(L, 1);
		const char* output = luaL_optstring(L, 2, "");
		MirageObject* instigator = lua_isnoneornil(L, 3) ? nullptr : CheckObject(L, 3);
		lua_pushboolean(L, self.api_->kismet_fire(self.mod_, event, output, instigator) != 0);
		return 1;
	}
	// mirage.kismet_trace(on): the execution trace on or off; mirage.kismet_trace_read(): its entries since the last
	// read, {{tick, op, class, times}, ...}
	int LuaMod::KismetTraceFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, kismet_trace)) return luaL_error(L, "mirage.kismet_trace: this runtime has no Kismet trace");
		self.api_->kismet_trace(self.mod_, lua_toboolean(L, 1) ? 1 : 0);
		return 0;
	}
	int LuaMod::KismetTraceReadFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, kismet_trace_read)) return luaL_error(L, "mirage.kismet_trace_read: this runtime has no Kismet trace");
		std::string text;
		for (;;) {
			text.assign(self.api_->kismet_trace_read(self.mod_, nullptr, 0) + 1, '\0');
			const uint32_t n = self.api_->kismet_trace_read(self.mod_, text.data(), static_cast<uint32_t>(text.size()));
			if (n < text.size()) {
				text.resize(n);
				break;
			}
		}
		PushJson(L, nlohmann::json::parse(text, nullptr, false));
		return 1;
	}

	// mirage.transitions(): the player's last 256 move transitions, oldest first: {{tick, time, from, to, from_name,
	// to_name, skipped}, ...}
	int LuaMod::TransitionsFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		if (!MIRAGE_API_HAS(self.api_, movement_transitions)) return luaL_error(L, "mirage.transitions: this runtime has no transition record");
		std::string text(self.api_->movement_transitions(nullptr, 0) + 1, '\0');
		text.resize((std::min)(static_cast<std::size_t>(self.api_->movement_transitions(text.data(), static_cast<uint32_t>(text.size()))), text.size() - 1));
		PushJson(L, nlohmann::json::parse(text, nullptr, false));
		return 1;
	}

	int LuaMod::Changes(lua_State* L)
	{
		LuaMod& self = Self(L);
		std::string text(self.api_->change_list(self.mod_, nullptr, 0) + 1, '\0');
		self.api_->change_list(self.mod_, text.data(), static_cast<uint32_t>(text.size()));
		const auto j = nlohmann::json::parse(text.c_str(), nullptr, false);
		lua_newtable(L);
		if (!j.is_array()) return 1;
		int n = 0;
		for (const auto& t : j) {
			lua_newtable(L);
			lua_pushinteger(L, t.value("id", 0));
			lua_setfield(L, -2, "id");
			lua_pushstring(L, t.value("name", std::string()).c_str());
			lua_setfield(L, -2, "name");
			lua_pushboolean(L, t.value("reverted", false));
			lua_setfield(L, -2, "reverted");
			lua_newtable(L);
			int k = 0;
			for (const auto& v : t.value("values", nlohmann::json::array())) {
				lua_pushstring(L, v.get<std::string>().c_str());
				lua_rawseti(L, -2, ++k);
			}
			lua_setfield(L, -2, "values");
			lua_rawseti(L, -2, ++n);
		}
		return 1;
	}

	// mirage.valid(object) -> whether it is still the object it was (false once it is gone, or for nil)
	int LuaMod::Valid(lua_State* L)
	{
		auto* p = static_cast<Proxy*>(luaL_testudata(L, 1, kProxyMeta));
		Refresh(p);
		lua_pushboolean(L, p && p->kind == kObject && p->handle != nullptr);
		return 1;
	}

	// mirage.actors{ class = "TdBotPawn", tag = "Door", name = "TdBotPawn_3", near = actor | {x, y, z}, radius = 2000 }
	// -> the actors in play that match, nearest first when a radius is given (every field optional)
	int LuaMod::Actors(lua_State* L)
	{
		LuaMod& self = Self(L);
		MirageActorQuery q{};
		q.size = sizeof q;
		std::string cls, tag, name;
		if (lua_istable(L, 1)) {
			const auto text = [&](const char* key, std::string& out) {
				if (lua_getfield(L, 1, key) == LUA_TSTRING) out = lua_tostring(L, -1);
				lua_pop(L, 1);
			};
			text("class", cls);
			text("tag", tag);
			text("name", name);
			if (lua_getfield(L, 1, "radius") == LUA_TNUMBER) q.radius = static_cast<float>(lua_tonumber(L, -1));
			lua_pop(L, 1);
			lua_getfield(L, 1, "near");
			if (auto* p = static_cast<Proxy*>(luaL_testudata(L, -1, kProxyMeta)); p && p->kind == kObject) {
				Refresh(p);
				if (!p->handle) {		// Around an actor that is gone: nothing is near it
					lua_newtable(L);
					return 1;
				}
				q.near_object = static_cast<MirageObject*>(p->handle);
			} else if (lua_istable(L, -1)) {
				const auto coord = [&](int i, const char* key) {
					float v = 0.f;
					if (lua_rawgeti(L, -1, i) == LUA_TNUMBER) v = static_cast<float>(lua_tonumber(L, -1));
					lua_pop(L, 1);
					if (lua_getfield(L, -1, key) == LUA_TNUMBER) v = static_cast<float>(lua_tonumber(L, -1));
					lua_pop(L, 1);
					return v;
				};
				q.x = coord(1, "X");
				q.y = coord(2, "Y");
				q.z = coord(3, "Z");
			}
			lua_pop(L, 1);
		}
		q.class_name = cls.c_str();
		q.tag = tag.c_str();
		q.name = name.c_str();
		std::vector<MirageObject*> found(256);
		int32_t n = self.api_->actors_query(&q, found.data(), static_cast<int32_t>(found.size()));
		if (n > static_cast<int32_t>(found.size())) {
			found.resize(static_cast<std::size_t>(n));
			n = self.api_->actors_query(&q, found.data(), n);
		}
		lua_createtable(L, std::max(n, 0), 0);
		for (int32_t i = 0; i < n && i < static_cast<int32_t>(found.size()); ++i) {
			PushObject(L, found[static_cast<std::size_t>(i)]);
			lua_rawseti(L, -2, i + 1);
		}
		return 1;
	}

	// mirage.call(object, "Function", { Param = value, ... }): its return value (true if it returns nothing)
	int LuaMod::CallGame(lua_State* L)
	{
		LuaMod& self = Self(L);
		MirageObject* o = CheckObject(L, 1);
		const char* fn = luaL_checkstring(L, 2);
		MirageCall* call = self.api_->call_begin(o, fn);
		if (!call) return luaL_error(L, "mirage.call: no function %s on this object", fn);
		if (lua_istable(L, 3)) {
			lua_pushnil(L);
			while (lua_next(L, 3)) {
				if (lua_type(L, -2) == LUA_TSTRING) {
					const std::string param = lua_tostring(L, -2);
					MirageValue probe;
					if (!self.api_->call_get(call, param.c_str(), &probe)) {
						self.api_->call_end(call);
						return luaL_error(L, "mirage.call: %s has no parameter %s", fn, param.c_str());
					}
					self.Set(L, kInCall, call, param, lua_gettop(L));
				}
				lua_pop(L, 1);
			}
		}
		if (!self.api_->call_invoke(call)) {
			self.api_->call_end(call);
			return luaL_error(L, "mirage.call: %s didn't run", fn);
		}
		MirageValue ret;
		if (self.api_->call_get(call, "ReturnValue", &ret) && ret.type != MIRAGE_VALUE_STRUCT) PushValue(L, ret, kInCall, call, "", 0);
		else lua_pushboolean(L, 1);
		self.api_->call_end(call);
		return 1;
	}

	// ---- Events and hooks ------------------------------------------------------------------------------------------
	void LuaMod::Fail(Callback& c, const std::string& error)
	{
		c.failed = true;
		Log(MIRAGE_LOG_ERROR, c.what + ": " + error + " (removed)");
		if (c.hook) api_->hook_remove(c.hook);
		if (c.subscription) api_->event_unsubscribe(c.subscription);
	}

	void MIRAGE_CALL LuaMod::OnEvent(void* user, const MirageEvent* e)
	{
		auto* c = static_cast<Callback*>(user);
		LuaMod& self = *c->owner;
		std::lock_guard lock(self.lock_);
		if (c->failed) return;
		lua_State* L = self.state_.Get();
		lua_rawgeti(L, LUA_REGISTRYINDEX, c->fn);
		int n = 0;
		switch (e->type) {
			case MIRAGE_EVENT_TICK: lua_pushnumber(L, e->number); n = 1; break;
			case MIRAGE_EVENT_LEVEL_LOAD: lua_pushboolean(L, e->value); lua_pushstring(L, e->text ? e->text : ""); n = 2; break;
			case MIRAGE_EVENT_DEATH: lua_pushboolean(L, e->value); n = 1; break;
			case MIRAGE_EVENT_PLAYER_STATE: lua_pushinteger(L, e->value); lua_pushinteger(L, e->value2); n = 2; break;
			case MIRAGE_EVENT_KEY: lua_pushinteger(L, e->value); n = 1; break;
			case MIRAGE_EVENT_ACTOR_ADDED:
				PushObject(L, e->object);
				lua_pushstring(L, e->text ? e->text : "");
				n = 2;
				break;
			case MIRAGE_EVENT_ACTOR_REMOVED: {
				// The actor is gone: a proxy that is already dead, but == to the one the mod kept
				PushProxy(L, kObject, nullptr, "", 0);
				static_cast<Proxy*>(lua_touserdata(L, -1))->id.object = e->object;
				n = 1;
				break;
			}
			case MIRAGE_EVENT_LEVEL_STREAMED: lua_pushboolean(L, e->value); lua_pushstring(L, e->text ? e->text : ""); n = 2; break;
			default: break;
		}
		// A player state callback that returns true skips the game's handling of the new state (event_skip)
		const bool skippable = e->type == MIRAGE_EVENT_PLAYER_STATE;
		const lua::Result r = self.state_.Call(n, skippable ? 1 : 0);
		if (!r.ok) {
			self.Fail(*c, r.error);
			return;
		}
		if (skippable) {
			if (lua_toboolean(L, -1)) self.api_->event_skip(e);
			lua_pop(L, 1);
		}
	}

	void MIRAGE_CALL LuaMod::OnHook(void* user, MirageCall* call)
	{
		auto* c = static_cast<Callback*>(user);
		LuaMod& self = *c->owner;
		std::lock_guard lock(self.lock_);
		if (c->failed) return;
		lua_State* L = self.state_.Get();
		lua_rawgeti(L, LUA_REGISTRYINDEX, c->fn);
		auto* box = static_cast<CallBox*>(lua_newuserdatauv(L, sizeof(CallBox), 0));
		box->call = call;
		luaL_setmetatable(L, kCallMeta);
		lua_pushvalue(L, -1);
		const int boxRef = luaL_ref(L, LUA_REGISTRYINDEX);	// To void it after the hook, wherever Lua kept it
		const lua::Result r = self.state_.Call(1, 0);
		lua_rawgeti(L, LUA_REGISTRYINDEX, boxRef);
		static_cast<CallBox*>(lua_touserdata(L, -1))->call = nullptr;
		lua_pop(L, 1);
		luaL_unref(L, LUA_REGISTRYINDEX, boxRef);
		if (!r.ok) self.Fail(*c, r.error);
	}

	// call.object, call.params, call.func; call:skip()
	int LuaMod::CallIndex(lua_State* L)
	{
		LuaMod& self = Self(L);
		auto* box = static_cast<CallBox*>(luaL_checkudata(L, 1, kCallMeta));
		const std::string key = luaL_checkstring(L, 2);
		if (key == "skip") {
			lua_pushlightuserdata(L, &self);
			lua_pushcclosure(L, CallSkip, 1);
			return 1;
		}
		if (!box->call) return luaL_error(L, "a hook's call is only there while the hook runs");
		if (key == "object") PushObject(L, self.api_->call_object(box->call));
		else if (key == "params") PushProxy(L, kInCall, box->call, "", 1);
		else if (key == "func") lua_pushstring(L, self.api_->call_function(box->call));
		else lua_pushnil(L);
		return 1;
	}

	int LuaMod::CallSkip(lua_State* L)
	{
		auto* box = static_cast<CallBox*>(luaL_checkudata(L, 1, kCallMeta));
		if (!box->call) return luaL_error(L, "a hook's call is only there while the hook runs");
		Self(L).api_->call_skip(box->call);
		return 0;
	}

	// mirage.on("tick" | "level_load" | "death" | "player_state" | "key", function)
	int LuaMod::On(lua_State* L)
	{
		LuaMod& self = Self(L);
		static const char* const kEvents[] = {"tick", "level_load", "death", "player_state", "key", "state_restored", "memory_pressure",
											  "actor_added", "actor_removed", "level_streamed", "restart", nullptr};
		const int type = luaL_checkoption(L, 1, nullptr, kEvents);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		auto c = std::make_unique<Callback>();
		c->owner = &self;
		c->what = std::string("mirage.on ") + kEvents[type];
		lua_pushvalue(L, 2);
		c->fn = luaL_ref(L, LUA_REGISTRYINDEX);
		c->subscription = self.api_->event_subscribe(self.mod_, type, &OnEvent, c.get());
		if (!c->subscription) return luaL_error(L, "mirage.on: %s can't be subscribed to", kEvents[type]);
		auto** box = static_cast<Callback**>(lua_newuserdatauv(L, sizeof(Callback*), 0));
		*box = c.get();
		luaL_setmetatable(L, kCallbackMeta);
		self.callbacks_.push_back(std::move(c));
		return 1;
	}

	// mirage.hook("Engine.Actor.SetLocation", function(call) ... end, { when = "before", priority = 0 })
	int LuaMod::HookFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		const char* function = luaL_checkstring(L, 1);
		luaL_checktype(L, 2, LUA_TFUNCTION);
		int when = MIRAGE_HOOK_BEFORE, priority = 0;
		if (lua_istable(L, 3)) {
			lua_getfield(L, 3, "when");
			static const char* const kWhen[] = {"before", "after", "replace", nullptr};
			if (!lua_isnil(L, -1)) when = luaL_checkoption(L, -1, nullptr, kWhen);
			lua_pop(L, 1);
			lua_getfield(L, 3, "priority");
			priority = static_cast<int>(luaL_optinteger(L, -1, 0));
			lua_pop(L, 1);
		}
		auto c = std::make_unique<Callback>();
		c->owner = &self;
		c->what = std::string("hook of ") + function;
		lua_pushvalue(L, 2);
		c->fn = luaL_ref(L, LUA_REGISTRYINDEX);
		c->hook = self.api_->hook_function(self.mod_, function, when, priority, &OnHook, c.get());
		if (!c->hook) return luaL_error(L, "mirage.hook: %s can't be hooked (see the log)", function);
		auto** box = static_cast<Callback**>(lua_newuserdatauv(L, sizeof(Callback*), 0));
		*box = c.get();
		luaL_setmetatable(L, kCallbackMeta);
		self.callbacks_.push_back(std::move(c));
		return 1;
	}

	// mirage.inject("TdGame.TdMove_Jump.StartMove", "before:GetMobilityMultiplier", function(call) ... end, { priority = 0 })
	// The call reads and writes parameters and locals (call:get / call:set); call:skip() returns from the function there.
	int LuaMod::InjectFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		const char* function = luaL_checkstring(L, 1);
		const char* point = luaL_checkstring(L, 2);
		luaL_checktype(L, 3, LUA_TFUNCTION);
		int priority = 0;
		if (lua_istable(L, 4)) {
			lua_getfield(L, 4, "priority");
			priority = static_cast<int>(luaL_optinteger(L, -1, 0));
			lua_pop(L, 1);
		}
		auto c = std::make_unique<Callback>();
		c->owner = &self;
		c->what = std::string("injection into ") + function + " at " + point;
		lua_pushvalue(L, 3);
		c->fn = luaL_ref(L, LUA_REGISTRYINDEX);
		c->hook = self.api_->inject_function(self.mod_, function, point, priority, &OnHook, c.get());
		if (!c->hook) {
			luaL_unref(L, LUA_REGISTRYINDEX, c->fn);
			return luaL_error(L, "mirage.inject: %s at %s can't be injected (see the log)", function, point);
		}
		auto** box = static_cast<Callback**>(lua_newuserdatauv(L, sizeof(Callback*), 0));
		*box = c.get();
		luaL_setmetatable(L, kCallbackMeta);
		self.callbacks_.push_back(std::move(c));
		return 1;
	}

	// local index = mirage.native("halve", "float(float)", function(x) return x / 2 end)
	// The index goes in a script patch's NativeFunction token: the game's bytecode calls the Lua function with its
	// arguments (numbers, booleans, objects, names as strings) and gets its return value back.
	int LuaMod::NativeFn(lua_State* L)
	{
		LuaMod& self = Self(L);
		const char* name = luaL_checkstring(L, 1);
		const char* signature = luaL_checkstring(L, 2);
		luaL_checktype(L, 3, LUA_TFUNCTION);
		auto c = std::make_unique<Callback>();
		c->owner = &self;
		c->what = std::string("native ") + name;
		lua_pushvalue(L, 3);
		c->fn = luaL_ref(L, LUA_REGISTRYINDEX);
		const int32_t index = self.api_->native_register(self.mod_, name, signature, &OnNative, c.get());
		if (!index) {
			luaL_unref(L, LUA_REGISTRYINDEX, c->fn);
			return luaL_error(L, "mirage.native: %s %s can't be registered (see the log)", name, signature);
		}
		self.callbacks_.push_back(std::move(c));
		lua_pushinteger(L, index);
		return 1;
	}

	void MIRAGE_CALL LuaMod::OnNative(void* user, MirageNativeCall* call)
	{
		auto* c = static_cast<Callback*>(user);
		LuaMod& self = *c->owner;
		std::lock_guard lock(self.lock_);
		if (c->failed) return;
		lua_State* L = self.state_.Get();
		lua_rawgeti(L, LUA_REGISTRYINDEX, c->fn);
		int n = 0;
		for (MirageValue v{}; self.api_->native_arg(call, n, &v); ++n) {
			if (v.type == MIRAGE_VALUE_OBJECT) PushObject(L, v.o);
			else PushValue(L, v, kInCall, nullptr, "", 0);
		}
		const lua::Result r = self.state_.Call(n, 1);
		if (!r.ok) {
			self.Fail(*c, r.error);
			return;
		}
		MirageValue out{};
		if (ToValue(L, -1, out)) self.api_->native_return(call, &out);
		lua_pop(L, 1);
	}

	int LuaMod::CallbackRemove(lua_State* L)
	{
		auto** box = static_cast<Callback**>(luaL_checkudata(L, 1, kCallbackMeta));
		Callback& c = **box;
		if (c.failed) return 0;
		c.failed = true;
		if (c.hook) Self(L).api_->hook_remove(c.hook);
		if (c.subscription) Self(L).api_->event_unsubscribe(c.subscription);
		return 0;
	}

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
		const luaL_Reg fns[] = {{"log", LuaLog<MIRAGE_LOG_INFO>}, {"info", LuaLog<MIRAGE_LOG_INFO>},
			{"warn", LuaLog<MIRAGE_LOG_WARN>}, {"error", LuaLog<MIRAGE_LOG_ERROR>}, {"module", LuaModule},
			{"setting", FindSetting}, {"rgba", Rgba},
			{"on", On}, {"hook", HookFn}, {"inject", InjectFn}, {"native", NativeFn}, {"pawn", Pawn}, {"controller", Controller}, {"world", World}, {"find", Find}, {"load_class", LoadClass}, {"define_class", DefineClass},
			{"path", PathOf}, {"keep", KeepFn}, {"release", ReleaseFn}, {"layer_actor", LayerActor},
			{"name", Name}, {"class", ClassName}, {"is_a", IsA}, {"call", CallGame}, {"notify", Notify},
			{"frame_rate", FrameRate}, {"all", All}, {"menu_open", MenuOpen}, {"input_key", InputKey}, {"share", Share},
			{"shared", Shared}, {"valid", Valid}, {"actors", Actors}, {"transaction", Transaction}, {"revert", Revert},
			{"changes", Changes}, {"symbol", SymbolFn}, {"test", TestFn}, {"movement", MovementFn}, {"kismet", KismetFn},
			{"kismet_link", KismetLinkFn}, {"kismet_unlink", KismetUnlinkFn}, {"kismet_create", KismetCreateFn},
			{"kismet_fire", KismetFireFn}, {"kismet_trace", KismetTraceFn}, {"kismet_trace_read", KismetTraceReadFn},
			{"transitions", TransitionsFn}, {nullptr, nullptr}};
		set(fns);
		lua_pushstring(L, api_->mod_id(mod_));
		lua_setfield(L, -2, "id");
		lua_pushstring(L, api_->mod_version(mod_));
		lua_setfield(L, -2, "version");
		lua_pushstring(L, api_->mod_dir(mod_));
		lua_setfield(L, -2, "dir");
		lua_pushstring(L, api_->runtime_version);
		lua_setfield(L, -2, "runtime_version");

		lua_newtable(L);
		const luaL_Reg configFns[] = {{"get", ConfigGet}, {"set", ConfigSet}, {nullptr, nullptr}};
		set(configFns);
		lua_setfield(L, -2, "config");

		lua_newtable(L);
		const luaL_Reg drawFns[] = {{"text", DrawText}, {"line", DrawLine}, {"rect", DrawRect},
			{"rect_filled", DrawRectFilled}, {"circle", DrawCircle}, {"circle_filled", DrawCircleFilled},
			{"screen_size", ScreenSize}, {"text_size", TextSize}, {"text_sized", DrawTextSized},
			{"text_size_sized", TextSizeSized}, {"rect_rounded", DrawRectRounded}, {"triangle", DrawTriangle},
			{nullptr, nullptr}};
		set(drawFns);
		lua_setfield(L, -2, "draw");

		lua_ui::Push(L, api_->ui());
		lua_setfield(L, -2, "ui");

		lua_newtable(L);
		const luaL_Reg scriptLib[] = {{"open", ScriptOpen}, {"restore", ScriptRestore}, {nullptr, nullptr}};
		set(scriptLib);
		lua_setfield(L, -2, "script");
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
}
