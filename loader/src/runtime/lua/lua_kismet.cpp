// Lua mods: mirage.kismet: the levels' Kismet, read and edited (runtime/lua/lua_mod.h)
#include "runtime/lua/lua_mod.h"

namespace mirage::mods
{
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
		PushJson(L, jc::parse(text));
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
		PushJson(L, jc::parse(text));
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
		PushJson(L, jc::parse(text));
		return 1;
	}

	// mirage: this file's functions (Bind puts every list in)
	const LuaFunctions kFunctions(nullptr, {
		{"kismet", LuaMod::KismetFn}, {"kismet_link", LuaMod::KismetLinkFn}, {"kismet_unlink", LuaMod::KismetUnlinkFn},
		{"kismet_create", LuaMod::KismetCreateFn}, {"kismet_fire", LuaMod::KismetFireFn},
		{"kismet_trace", LuaMod::KismetTraceFn}, {"kismet_trace_read", LuaMod::KismetTraceReadFn},
		{"transitions", LuaMod::TransitionsFn}
	});
}
