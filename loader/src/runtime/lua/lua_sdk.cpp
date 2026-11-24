// Game-thread reflection access for reviewed MirageAPIGen contracts.
#include "runtime/lua/lua_mod.h"
#include "runtime/reflect.h"
#include "game/game.h"
#include <sdk/Core.hpp>

#include <limits>

namespace mirage::mods::sdk_binding
{
	namespace
	{
		bool Matches(const reflect::Member& member, int32_t type)
		{
			switch (type) {
				case MIRAGE_VALUE_FLOAT:
					return member.kind == layout::Kind::Float && member.elementSize == sizeof(float);
				case MIRAGE_VALUE_INT:
					return member.kind == layout::Kind::Int && member.elementSize == sizeof(int32_t);
				case MIRAGE_VALUE_BOOL:
					return member.kind == layout::Kind::Bool && member.elementSize == sizeof(uint32_t) && member.boolMask;
				default: return false;
			}
		}

		MirageObject* Resolve(lua_State* L, const char* owner, const char* property, int32_t type)
		{
			if (!game::IsGameThread()) {
				luaL_error(L, "mirage.sdk requires the game thread");
				return nullptr;
			}
			const auto* api = LuaMod::Self(L).api_;
			if (!MIRAGE_API_HAS(api, handle_object)) return nullptr;
			MirageObject* object = LuaMod::CheckObject(L, 1);
			if (!object) return nullptr;
			auto* cls = reinterpret_cast<Classes::UClass*>(api->object_find(owner));
			const auto* instance = reinterpret_cast<Classes::UObject*>(object);
			if (!cls || !instance->IsA(cls)) return nullptr;
			const auto* declared = reflect::Find(cls, property);
			const auto* actual = reflect::Find(instance->Class, property);
			return actual && declared && actual->declaration == declared->declaration &&
				actual->arrayDim == 1 && Matches(*actual, type) ? object : nullptr;
		}
	}

	int Read(lua_State* L, const char* owner, const char* property, int32_t type)
	{
		MirageObject* object = Resolve(L, owner, property, type);
		MirageValue result{};
		if (!object || !LuaMod::Self(L).api_->object_get(object, property, &result) || result.type != type) {
			lua_pushnil(L);
			return 1;
		}
		LuaMod::PushValue(L, result, kObject, nullptr, "", 0);
		return 1;
	}

	int Write(lua_State* L, const char* owner, const char* property, int32_t type)
	{
		MirageObject* object = Resolve(L, owner, property, type);
		MirageValue input{};
		input.type = type;
		switch (type) {
			case MIRAGE_VALUE_FLOAT: {
				const lua_Number value = luaL_checknumber(L, 2);
				if (!std::isfinite(value) || std::abs(value) > (std::numeric_limits<float>::max)())
					return luaL_argerror(L, 2, "value is outside the Float range");
				input.f = static_cast<float>(value);
				break;
			}
			case MIRAGE_VALUE_INT: {
				const lua_Integer value = luaL_checkinteger(L, 2);
				if (value < INT32_MIN || value > INT32_MAX) return luaL_argerror(L, 2, "value is outside the Int range");
				input.i = static_cast<int32_t>(value);
				break;
			}
			case MIRAGE_VALUE_BOOL:
				luaL_checktype(L, 2, LUA_TBOOLEAN);
				input.i = lua_toboolean(L, 2);
				break;
			default: return luaL_error(L, "unsupported mirage.sdk write type");
		}
		lua_pushboolean(L, object && LuaMod::Self(L).api_->object_set(object, property, &input));
		return 1;
	}
}
