// Lua mods: Game objects as Lua values (proxies), classes, change log, tests, movement (runtime/lua/lua_mod.h)
#include "runtime/lua/lua_mod.h"

namespace mirage::mods
{
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
		lua::PushVec(L, m.velocity[0], m.velocity[1], m.velocity[2]);
		lua_setfield(L, -2, "velocity");
		lua_pushnumber(L, m.speed);
		lua_setfield(L, -2, "speed");
		self.PushObject(L, m.move_object);
		lua_setfield(L, -2, "object");
		return 1;
	}

	// mirage: this file's functions (Bind puts every list in)
	const LuaFunctions kFunctions(nullptr, {
		{"pawn", LuaMod::Pawn}, {"controller", LuaMod::Controller}, {"world", LuaMod::World}, {"find", LuaMod::Find},
		{"load_class", LuaMod::LoadClass}, {"define_class", LuaMod::DefineClass}, {"path", LuaMod::PathOf},
		{"keep", LuaMod::KeepFn}, {"release", LuaMod::ReleaseFn}, {"layer_actor", LuaMod::LayerActor},
		{"name", LuaMod::Name}, {"class", LuaMod::ClassName}, {"is_a", LuaMod::IsA}, {"all", LuaMod::All},
		{"transaction", LuaMod::Transaction}, {"revert", LuaMod::Revert}, {"symbol", LuaMod::SymbolFn},
		{"test", LuaMod::TestFn}, {"movement", LuaMod::MovementFn}
	});
}
