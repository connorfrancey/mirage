// Lua mods: mirage.on and mirage.hook: events, function hooks, natives (runtime/lua/lua_mod.h)
#include "runtime/lua/lua_mod.h"

namespace mirage::mods
{
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
			case MIRAGE_EVENT_ACTION: lua_pushstring(L, e->text ? e->text : ""); lua_pushboolean(L, e->value); n = 2; break;
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

	// mirage.on("tick" | "level_load" | "death" | "player_state" | "key" | ... | "action", function); "action": (name, pressed)
	int LuaMod::On(lua_State* L)
	{
		LuaMod& self = Self(L);
		static const char* const kEvents[] = {"tick", "level_load", "death", "player_state", "key", "state_restored", "memory_pressure",
											  "actor_added", "actor_removed", "level_streamed", "restart", "action", nullptr};
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

	// mirage: this file's functions (Bind puts every list in)
	const LuaFunctions kFunctions(nullptr, {
		{"on", LuaMod::On}, {"hook", LuaMod::HookFn}, {"inject", LuaMod::InjectFn}, {"native", LuaMod::NativeFn}
	});
}
