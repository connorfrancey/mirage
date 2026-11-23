#pragma once
// Lua mods, inside (runtime/mod_lua.h is what the rest of the loader sees): the LuaMod class and what its parts
// share. runtime/mod_lua.cpp binds the `mirage` table and loads a mod's main.lua; each runtime/lua/lua_<area>.cpp
// holds one area of the table (modules, drawing, objects, Kismet, the world, events, script patching).

#include "runtime/mod_lua.h"
#include "runtime/lua/lua_functions.h"
#include "runtime/lua_vec.h"
#include "runtime/lua_vm.h"
#include "runtime/mod_lua_ui.h"
#include <lauxlib.h>
#include <lua.h>
#include <fmt/format.h>
#include "util/glz_compat.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <cmath>
#include <vector>

namespace mirage::mods
{
	namespace lua_detail
	{
		inline constexpr const char* kModuleMeta = "mirage.module";
		inline constexpr const char* kSettingMeta = "mirage.setting";
		inline constexpr const char* kProxyMeta = "mirage.value";
		inline constexpr const char* kScriptMeta = "mirage.script";		// A function's bytecode, open for patching		// An object, or a struct in an object or a call
		inline constexpr const char* kCallMeta = "mirage.call";		// What a hook gets: valid during the hook only
		inline constexpr const char* kCallbackMeta = "mirage.callback";	// A subscription or a hook: :remove()
		enum ProxyKind { kObject = 0, kInObject = 1, kInCall = 2 };
		struct Proxy			// uservalue 1: the path (string); 2: the call box (kInCall)
		{
			int kind;
			void* handle;			// MirageObject* or MirageCall*; an object's is cleared once it is gone (Refresh)
			MirageHandle id;		// kObject / kInObject: what identifies the object (step 14); id.object is kept for ==
		};
		// The runtime's API (the same table for every mod): for what runs outside a Lua C function (event callbacks push
		// objects), where Self(L) has no upvalue to read
		inline const MirageApi* g_api = nullptr;

		inline bool IsObjectKind(int kind) { return kind == kObject || kind == kInObject; }
		// An object proxy whose object is gone (or replaced by another at its address) lets go of it: from then on it
		// reads as a dead object (nil values, failed calls), and never reaches the new one
		inline void Refresh(Proxy* p)
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
		inline constexpr const char* kHookNames[kHooks] = {"on_enable", "on_disable", "on_tick", "on_render", "on_trigger", "on_panel"};

		// A JSON value as Lua values: objects as tables by key, arrays as sequences (Kismet and transitions read JSON)
		inline void PushJson(lua_State* L, const jc::json& j)
		{
			if (j.is_object()) {
				lua_createtable(L, 0, static_cast<int>(j.size()));
				for (const auto& [key, v] : j.get_object()) {
					PushJson(L, v);
					lua_setfield(L, -2, key.c_str());
				}
			} else if (j.is_array()) {
				lua_createtable(L, static_cast<int>(j.size()), 0);
				lua_Integer i = 1;
				for (const auto& v : j.get_array()) {
					PushJson(L, v);
					lua_rawseti(L, -2, i++);
				}
			} else if (const auto* str = j.get_if<std::string>()) {
				lua_pushstring(L, str->c_str());
			} else if (const auto* b = j.get_if<bool>()) {
				lua_pushboolean(L, *b);
			} else if (const auto* n = j.get_if<std::int64_t>()) {
				lua_pushinteger(L, static_cast<lua_Integer>(*n));
			} else if (const auto* d = j.get_if<double>()) {
				lua_pushnumber(L, *d);
			} else {
				lua_pushnil(L);
			}
		}
	}
	using namespace lua_detail;

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

		// The console (step 27's REPL): `code` run in this mod's own state, as an expression first ("1 + 2",
		// "mirage.pawn()") and as statements when it isn't one; what it prints comes back in `printed`. Game thread.
		lua::Result Eval(const std::string& code, std::string& printed)
		{
			std::lock_guard lock(lock_);
			lua_State* L = state_.Get();
			if (!L) return {false, "", "no Lua state"};
			lua_getglobal(L, "print");
			const int saved = luaL_ref(L, LUA_REGISTRYINDEX);
			lua_pushlightuserdata(L, &printed);
			lua_pushcclosure(L, [](lua_State* L) -> int {
				auto* out = static_cast<std::string*>(lua_touserdata(L, lua_upvalueindex(1)));
				for (int i = 1, n = lua_gettop(L); i <= n; ++i) {
					std::size_t len = 0;
					const char* s = luaL_tolstring(L, i, &len);
					if (i > 1) *out += "\t";
					out->append(s, len);
					lua_pop(L, 1);
				}
				*out += "\n";
				return 0;
			}, 1);
			lua_setglobal(L, "print");
			const std::string expression = "return " + code;
			const bool isExpression = luaL_loadbufferx(L, expression.data(), expression.size(), "=console", "t") == LUA_OK;
			lua_pop(L, 1);		// The compiled chunk, or the message
			const lua::Result r = state_.Run(isExpression ? expression : code, "console");
			lua_rawgeti(L, LUA_REGISTRYINDEX, saved);
			lua_setglobal(L, "print");
			luaL_unref(L, LUA_REGISTRYINDEX, saved);
			return r;
		}

	public:		// Every member: the area files (runtime/lua/lua_*.cpp) define them
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
		static int PortalClass(lua_State* L);
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
		static int BodyFn(lua_State* L);
		static int SetBodyFn(lua_State* L);
		static int ImpulseFn(lua_State* L);
		static int ForceFn(lua_State* L);
		static int SpawnPropFn(lua_State* L);
		static int SpawnFn(lua_State* L);
		static int ModelFn(lua_State* L);
		static int SoundPlayFn(lua_State* L);
		static int SoundStopFn(lua_State* L);
		static int SoundPlayingFn(lua_State* L);
		static int PostfxFn(lua_State* L);
		static int PostfxSetFn(lua_State* L);
		static int PostfxEnableFn(lua_State* L);
		static int PostfxErrorFn(lua_State* L);
		static int MenuSliderFn(lua_State* L);
		static int MenuSliderValueFn(lua_State* L);
		static int CollisionFn(lua_State* L);
		static int SetCollisionFn(lua_State* L);
		static int TraceFn(lua_State* L);
		static int CameraFn(lua_State* L);
		static int WorldToScreenFn(lua_State* L);
		static int Draw3dLines(lua_State* L);
		static int Draw3dClear(lua_State* L);
		static int OverlapFn(lua_State* L);
		static int JointFn(lua_State* L);
		static int TriggerFn(lua_State* L);
		static int TriggerCountFn(lua_State* L);
		static int PieceFn(lua_State* L);
		static int MoverFn(lua_State* L);
		static int MoveToFn(lua_State* L);
		static int MoverMovingFn(lua_State* L);
		static int MoveFn(lua_State* L);
		static int MoveStartFn(lua_State* L);
		static int MoveRemoveFn(lua_State* L);
		static void MIRAGE_CALL OnMove(void* user, int32_t phase, float seconds);
		static int AnimPlayFn(lua_State* L);
		static int AnimStopFn(lua_State* L);
		static int AnimStateFn(lua_State* L);
		static int AnimSetsFn(lua_State* L);
		static void MIRAGE_CALL OnTrigger(void* user, MirageObject* trigger, MirageObject* other, int32_t kind);
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
}
