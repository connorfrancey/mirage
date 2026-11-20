// Lua mods: mirage.draw: drawing on the screen, colours, text sizes (runtime/lua/lua_mod.h)
#include "runtime/lua/lua_mod.h"

namespace mirage::mods
{
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

	// mirage: this file's functions (Bind puts every list in)
	const LuaFunctions kFunctions(nullptr, {
		{"rgba", LuaMod::Rgba}, {"frame_rate", LuaMod::FrameRate}
	});
	// mirage.draw: this file's functions (Bind puts every list in)
	const LuaFunctions kDrawFunctions("draw", {
		{"text", LuaMod::DrawText}, {"line", LuaMod::DrawLine}, {"rect", LuaMod::DrawRect},
		{"rect_filled", LuaMod::DrawRectFilled}, {"circle", LuaMod::DrawCircle},
		{"circle_filled", LuaMod::DrawCircleFilled}, {"screen_size", LuaMod::ScreenSize},
		{"text_size", LuaMod::TextSize}, {"text_sized", LuaMod::DrawTextSized},
		{"text_size_sized", LuaMod::TextSizeSized}, {"rect_rounded", LuaMod::DrawRectRounded},
		{"triangle", LuaMod::DrawTriangle}
	});
}
