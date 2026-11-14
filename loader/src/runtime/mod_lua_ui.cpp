// mirage.ui: the UI layer (MirageUi) for Lua mods. Widgets that edit a value take it and return (changed, new value):
//
//   local changed, on = mirage.ui.checkbox("Enabled", on)
//   if mirage.ui.button("Reset") then ... end
//   local entered, text = mirage.ui.input_text("##cmd", text, mirage.ui.INPUT_ENTER_RETURNS)
//
// Combo indices are 1-based, like Lua's own lists. Only from on_render (windows) and on_panel (the module's panel).
#include "runtime/mod_lua_ui.h"

#include <lauxlib.h>
#include <lua.h>

#include <algorithm>
#include <string>
#include <vector>

namespace mirage::mods::lua_ui
{
	namespace
	{
		const MirageUi& U(lua_State* L) { return *static_cast<const MirageUi*>(lua_touserdata(L, lua_upvalueindex(1))); }

		float F(lua_State* L, int i, float def = 0.f) { return static_cast<float>(luaL_optnumber(L, i, def)); }
		int32_t I(lua_State* L, int i, int32_t def = 0) { return static_cast<int32_t>(luaL_optinteger(L, i, def)); }
		uint32_t Flags(lua_State* L, int i) { return static_cast<uint32_t>(luaL_optinteger(L, i, 0)); }
		uint32_t Color(lua_State* L, int i) { return static_cast<uint32_t>(luaL_checkinteger(L, i)); }
		const char* Str(lua_State* L, int i) { return luaL_checkstring(L, i); }
		const char* OptStr(lua_State* L, int i) { return luaL_optstring(L, i, nullptr); }
		int Bool(lua_State* L, bool b) { lua_pushboolean(L, b); return 1; }

		// ---- Windows and layout ------------------------------------------------------------------------------------
		int WindowBegin(lua_State* L)
		{
			const bool hasOpen = !lua_isnoneornil(L, 3);
			int32_t open = hasOpen ? lua_toboolean(L, 3) : 1;
			const bool shown = U(L).window_begin(Str(L, 1), hasOpen ? &open : nullptr, Flags(L, 2)) != 0;
			lua_pushboolean(L, shown);
			if (!hasOpen) return 1;
			lua_pushboolean(L, open);
			return 2;
		}
		int WindowEnd(lua_State* L) { U(L).window_end(); return 0; }
		int WindowNextPos(lua_State* L) { U(L).window_next_pos(F(L, 1), F(L, 2), lua_toboolean(L, 3)); return 0; }
		int WindowNextSize(lua_State* L) { U(L).window_next_size(F(L, 1), F(L, 2), lua_toboolean(L, 3)); return 0; }
		int WindowNextBgAlpha(lua_State* L) { U(L).window_next_bg_alpha(F(L, 1, 1.f)); return 0; }
		int ChildBegin(lua_State* L) { return Bool(L, U(L).child_begin(Str(L, 1), F(L, 2), F(L, 3), lua_toboolean(L, 4))); }
		int ChildEnd(lua_State* L) { U(L).child_end(); return 0; }
		int SameLine(lua_State* L) { U(L).same_line(F(L, 1, 0.f), F(L, 2, -1.f)); return 0; }
		int Separator(lua_State* L) { U(L).separator(); return 0; }
		int Spacing(lua_State* L) { U(L).spacing(); return 0; }
		int Indent(lua_State* L) { U(L).indent(F(L, 1)); return 0; }
		int Unindent(lua_State* L) { U(L).unindent(F(L, 1)); return 0; }
		int NextWidth(lua_State* L) { U(L).next_width(F(L, 1)); return 0; }
		int ContentWidth(lua_State* L) { lua_pushnumber(L, U(L).content_width()); return 1; }
		int ContentHeight(lua_State* L) { lua_pushnumber(L, U(L).content_height()); return 1; }
		int LineHeight(lua_State* L) { lua_pushnumber(L, U(L).line_height()); return 1; }
		int CursorScreenPos(lua_State* L)
		{
			float x, y;
			U(L).cursor_screen_pos(&x, &y);
			lua_pushnumber(L, x);
			lua_pushnumber(L, y);
			return 2;
		}
		int Dummy(lua_State* L) { U(L).dummy(F(L, 1), F(L, 2)); return 0; }
		int IdPush(lua_State* L)
		{
			if (lua_type(L, 1) == LUA_TNUMBER) U(L).id_push_int(static_cast<int32_t>(lua_tointeger(L, 1)));
			else U(L).id_push(Str(L, 1));
			return 0;
		}
		int IdPop(lua_State* L) { U(L).id_pop(); return 0; }
		int DisabledBegin(lua_State* L) { U(L).disabled_begin(lua_isnone(L, 1) ? 1 : lua_toboolean(L, 1)); return 0; }
		int DisabledEnd(lua_State* L) { U(L).disabled_end(); return 0; }
		int ColorPush(lua_State* L) { U(L).color_push(I(L, 1), Color(L, 2)); return 0; }
		int ColorPop(lua_State* L) { U(L).color_pop(I(L, 1, 1)); return 0; }
		int FontScale(lua_State* L) { U(L).font_scale(F(L, 1, 1.f)); return 0; }

		// ---- Text --------------------------------------------------------------------------------------------------
		std::string Concat(lua_State* L, int from)
		{
			std::string s;
			for (int i = from, n = lua_gettop(L); i <= n; ++i) {
				if (i > from) s += ' ';
				s += luaL_tolstring(L, i, nullptr);
				lua_pop(L, 1);
			}
			return s;
		}
		int Text(lua_State* L) { U(L).text(Concat(L, 1).c_str()); return 0; }
		int TextColored(lua_State* L) { U(L).text_colored(Color(L, 1), Concat(L, 2).c_str()); return 0; }
		int TextDisabled(lua_State* L) { U(L).text_disabled(Concat(L, 1).c_str()); return 0; }
		int TextWrapped(lua_State* L) { U(L).text_wrapped(Concat(L, 1).c_str()); return 0; }
		int LabelText(lua_State* L) { U(L).label_text(Str(L, 1), Concat(L, 2).c_str()); return 0; }
		int BulletText(lua_State* L) { U(L).bullet_text(Concat(L, 1).c_str()); return 0; }

		// ---- Widgets -----------------------------------------------------------------------------------------------
		int Button(lua_State* L) { return Bool(L, U(L).button(Str(L, 1), F(L, 2), F(L, 3))); }
		int SmallButton(lua_State* L) { return Bool(L, U(L).small_button(Str(L, 1))); }
		int Changed(lua_State* L, bool changed)
		{
			lua_pushboolean(L, changed);
			lua_insert(L, -2);		// changed, value
			return 2;
		}
		int Checkbox(lua_State* L)
		{
			int32_t v = lua_toboolean(L, 2);
			const bool c = U(L).checkbox(Str(L, 1), &v);
			lua_pushboolean(L, v);
			return Changed(L, c);
		}
		int Radio(lua_State* L) { return Bool(L, U(L).radio(Str(L, 1), lua_toboolean(L, 2))); }
		int SliderFloat(lua_State* L)
		{
			float v = F(L, 2);
			const bool c = U(L).slider_float(Str(L, 1), &v, F(L, 3), F(L, 4, 1.f), OptStr(L, 5));
			lua_pushnumber(L, v);
			return Changed(L, c);
		}
		int SliderInt(lua_State* L)
		{
			int32_t v = I(L, 2);
			const bool c = U(L).slider_int(Str(L, 1), &v, I(L, 3), I(L, 4, 100), OptStr(L, 5));
			lua_pushinteger(L, v);
			return Changed(L, c);
		}
		int DragFloat(lua_State* L)
		{
			float v = F(L, 2);
			const bool c = U(L).drag_float(Str(L, 1), &v, F(L, 3, 1.f), F(L, 4), F(L, 5), OptStr(L, 6));
			lua_pushnumber(L, v);
			return Changed(L, c);
		}
		int DragInt(lua_State* L)
		{
			int32_t v = I(L, 2);
			const bool c = U(L).drag_int(Str(L, 1), &v, F(L, 3, 1.f), I(L, 4), I(L, 5));
			lua_pushinteger(L, v);
			return Changed(L, c);
		}
		int InputInt(lua_State* L)
		{
			int32_t v = I(L, 2);
			const bool c = U(L).input_int(Str(L, 1), &v, I(L, 3, 1));
			lua_pushinteger(L, v);
			return Changed(L, c);
		}
		int InputFloat(lua_State* L)
		{
			float v = F(L, 2);
			const bool c = U(L).input_float(Str(L, 1), &v, F(L, 3), OptStr(L, 4));
			lua_pushnumber(L, v);
			return Changed(L, c);
		}
		// input_text(label, text, flags, capacity = 1024, height = 0)
		int InputText(lua_State* L)
		{
			std::size_t len = 0;
			const char* text = luaL_optlstring(L, 2, "", &len);
			const auto capacity = static_cast<std::size_t>(std::clamp<lua_Integer>(luaL_optinteger(L, 4, 1024), 2, 1 << 20));
			std::vector<char> buf(std::max(capacity, len + 1), '\0');
			std::copy_n(text, len, buf.data());
			const bool c = U(L).input_text(Str(L, 1), buf.data(), static_cast<uint32_t>(buf.size()), Flags(L, 3), F(L, 5));
			lua_pushstring(L, buf.data());
			return Changed(L, c);
		}
		// combo(label, index (1-based), { "A", "B" })
		int Combo(lua_State* L)
		{
			luaL_checktype(L, 3, LUA_TTABLE);
			std::string items;
			for (lua_Integer i = 1, n = luaL_len(L, 3); i <= n; ++i) {
				lua_geti(L, 3, i);
				items += luaL_tolstring(L, -1, nullptr);
				items.push_back('\0');
				lua_pop(L, 2);
			}
			items.push_back('\0');
			int32_t index = I(L, 2, 1) - 1;
			const bool c = U(L).combo(Str(L, 1), &index, items.c_str());
			lua_pushinteger(L, index + 1);
			return Changed(L, c);
		}
		int Selectable(lua_State* L) { return Bool(L, U(L).selectable(Str(L, 1), lua_toboolean(L, 2), F(L, 3), F(L, 4))); }
		int ColorEdit(lua_State* L)
		{
			auto c = static_cast<uint32_t>(luaL_checkinteger(L, 2));
			const bool changed = U(L).color_edit(Str(L, 1), &c);
			lua_pushinteger(L, static_cast<lua_Integer>(c));
			return Changed(L, changed);
		}
		int ProgressBar(lua_State* L) { U(L).progress_bar(F(L, 1), F(L, 2, -1.f), F(L, 3), OptStr(L, 4)); return 0; }
		template <bool Histogram>
		int Plot(lua_State* L)
		{
			luaL_checktype(L, 2, LUA_TTABLE);
			std::vector<float> v(static_cast<std::size_t>(luaL_len(L, 2)));
			for (std::size_t i = 0; i < v.size(); ++i) {
				lua_geti(L, 2, static_cast<lua_Integer>(i + 1));
				v[i] = static_cast<float>(lua_tonumber(L, -1));
				lua_pop(L, 1);
			}
			const auto fn = Histogram ? U(L).plot_histogram : U(L).plot_lines;
			fn(Str(L, 1), v.data(), static_cast<int32_t>(v.size()), F(L, 3), F(L, 4), F(L, 5), F(L, 6), OptStr(L, 7));
			return 0;
		}

		int TreeNode(lua_State* L) { return Bool(L, U(L).tree_node(Str(L, 1), lua_toboolean(L, 2))); }
		int TreePop(lua_State* L) { U(L).tree_pop(); return 0; }
		int CollapsingHeader(lua_State* L) { return Bool(L, U(L).collapsing_header(Str(L, 1), lua_toboolean(L, 2))); }
		int TableBegin(lua_State* L) { return Bool(L, U(L).table_begin(Str(L, 1), I(L, 2, 1), Flags(L, 3), F(L, 4), F(L, 5))); }
		int TableColumn(lua_State* L) { U(L).table_column(Str(L, 1), Flags(L, 2), F(L, 3)); return 0; }
		int TableHeaders(lua_State* L) { U(L).table_headers(); return 0; }
		int TableNextRow(lua_State* L) { U(L).table_next_row(); return 0; }
		int TableNextColumn(lua_State* L) { return Bool(L, U(L).table_next_column()); }
		int TableEnd(lua_State* L) { U(L).table_end(); return 0; }

		int ItemHovered(lua_State* L) { return Bool(L, U(L).item_hovered()); }
		int ItemClicked(lua_State* L) { return Bool(L, U(L).item_clicked(I(L, 1))); }
		int ItemActive(lua_State* L) { return Bool(L, U(L).item_active()); }
		int ItemRect(lua_State* L)
		{
			float r[4];
			U(L).item_rect(&r[0], &r[1], &r[2], &r[3]);
			for (float v : r) lua_pushnumber(L, v);
			return 4;
		}
		int Tooltip(lua_State* L) { U(L).tooltip(Concat(L, 1).c_str()); return 0; }
		int PopupOpen(lua_State* L) { U(L).popup_open(Str(L, 1)); return 0; }
		int PopupBegin(lua_State* L) { return Bool(L, U(L).popup_begin(Str(L, 1))); }
		int PopupContextItem(lua_State* L) { return Bool(L, U(L).popup_context_item(OptStr(L, 1))); }
		int PopupEnd(lua_State* L) { U(L).popup_end(); return 0; }
		int PopupClose(lua_State* L) { U(L).popup_close(); return 0; }
		int FocusKeyboard(lua_State* L) { U(L).focus_keyboard(); return 0; }
		int ScrollY(lua_State* L) { lua_pushnumber(L, U(L).scroll_y()); return 1; }
		int ScrollMaxY(lua_State* L) { lua_pushnumber(L, U(L).scroll_max_y()); return 1; }
		int ScrollToBottom(lua_State* L) { U(L).scroll_to_bottom(); return 0; }

		int DrawRect(lua_State* L) { U(L).window_draw_rect(F(L, 1), F(L, 2), F(L, 3), F(L, 4), Color(L, 5), F(L, 6), F(L, 7)); return 0; }
		int DrawText(lua_State* L) { U(L).window_draw_text(F(L, 1), F(L, 2), Color(L, 3), Str(L, 4)); return 0; }
		int DrawLine(lua_State* L) { U(L).window_draw_line(F(L, 1), F(L, 2), F(L, 3), F(L, 4), Color(L, 5), F(L, 6, 1.f)); return 0; }
		int DrawCircle(lua_State* L) { U(L).window_draw_circle(F(L, 1), F(L, 2), F(L, 3), Color(L, 4), F(L, 5)); return 0; }

		int KeyPressed(lua_State* L) { return Bool(L, U(L).key_pressed(I(L, 1))); }
		int WantsKeyboard(lua_State* L) { return Bool(L, U(L).wants_keyboard()); }
		int MousePos(lua_State* L)
		{
			float x, y;
			U(L).mouse_pos(&x, &y);
			lua_pushnumber(L, x);
			lua_pushnumber(L, y);
			return 2;
		}
		int Time(lua_State* L) { lua_pushnumber(L, U(L).time()); return 1; }
		int DeltaTime(lua_State* L) { lua_pushnumber(L, U(L).delta_time()); return 1; }

		int NextOpen(lua_State* L) { U(L).next_open(lua_toboolean(L, 1), lua_toboolean(L, 2)); return 0; }
		// input_text_hint(label, hint, text, flags, capacity = 1024) -> changed, text
		int InputTextHint(lua_State* L)
		{
			std::size_t len = 0;
			const char* text = luaL_optlstring(L, 3, "", &len);
			const auto capacity = static_cast<std::size_t>(std::clamp<lua_Integer>(luaL_optinteger(L, 5, 1024), 2, 1 << 20));
			std::vector<char> buf(std::max(capacity, len + 1), '\0');
			std::copy_n(text, len, buf.data());
			const bool c = U(L).input_text_hint(Str(L, 1), Str(L, 2), buf.data(), static_cast<uint32_t>(buf.size()), Flags(L, 4));
			lua_pushstring(L, buf.data());
			return Changed(L, c);
		}
		int AlignTextToFrame(lua_State* L) { U(L).align_text_to_frame(); return 0; }
		int TextSize(lua_State* L)
		{
			float w, h;
			U(L).text_size(Str(L, 1), &w, &h);
			lua_pushnumber(L, w);
			lua_pushnumber(L, h);
			return 2;
		}
		int FrameHeight(lua_State* L) { lua_pushnumber(L, U(L).frame_height()); return 1; }

		int WindowRect(lua_State* L)
		{
			float r[4];
			U(L).window_rect(&r[0], &r[1], &r[2], &r[3]);
			for (float v : r) lua_pushnumber(L, v);
			return 4;
		}
		int OverlayRect(lua_State* L) { U(L).overlay_rect(F(L, 1), F(L, 2), F(L, 3), F(L, 4), Color(L, 5), F(L, 6), F(L, 7)); return 0; }

		const luaL_Reg kFns[] = {
			{"window_begin", WindowBegin}, {"window_end", WindowEnd}, {"window_next_pos", WindowNextPos},
			{"window_next_size", WindowNextSize}, {"window_next_bg_alpha", WindowNextBgAlpha},
			{"child_begin", ChildBegin}, {"child_end", ChildEnd},
			{"same_line", SameLine}, {"separator", Separator}, {"spacing", Spacing}, {"indent", Indent},
			{"unindent", Unindent}, {"next_width", NextWidth}, {"content_width", ContentWidth},
			{"content_height", ContentHeight}, {"line_height", LineHeight}, {"cursor_screen_pos", CursorScreenPos},
			{"dummy", Dummy}, {"id_push", IdPush}, {"id_pop", IdPop}, {"disabled_begin", DisabledBegin},
			{"disabled_end", DisabledEnd}, {"color_push", ColorPush}, {"color_pop", ColorPop}, {"font_scale", FontScale},
			{"text", Text}, {"text_colored", TextColored}, {"text_disabled", TextDisabled}, {"text_wrapped", TextWrapped},
			{"label_text", LabelText}, {"bullet_text", BulletText},
			{"button", Button}, {"small_button", SmallButton}, {"checkbox", Checkbox}, {"radio", Radio},
			{"slider_float", SliderFloat}, {"slider_int", SliderInt}, {"drag_float", DragFloat}, {"drag_int", DragInt},
			{"input_int", InputInt}, {"input_float", InputFloat}, {"input_text", InputText}, {"combo", Combo},
			{"selectable", Selectable}, {"color_edit", ColorEdit}, {"progress_bar", ProgressBar},
			{"plot_lines", Plot<false>}, {"plot_histogram", Plot<true>},
			{"tree_node", TreeNode}, {"tree_pop", TreePop}, {"collapsing_header", CollapsingHeader},
			{"table_begin", TableBegin}, {"table_column", TableColumn}, {"table_headers", TableHeaders},
			{"table_next_row", TableNextRow}, {"table_next_column", TableNextColumn}, {"table_end", TableEnd},
			{"item_hovered", ItemHovered}, {"item_clicked", ItemClicked}, {"item_active", ItemActive},
			{"item_rect", ItemRect}, {"tooltip", Tooltip}, {"popup_open", PopupOpen}, {"popup_begin", PopupBegin},
			{"popup_context_item", PopupContextItem}, {"popup_end", PopupEnd}, {"popup_close", PopupClose},
			{"focus_keyboard", FocusKeyboard}, {"scroll_y", ScrollY}, {"scroll_max_y", ScrollMaxY},
			{"scroll_to_bottom", ScrollToBottom},
			{"draw_rect", DrawRect}, {"draw_text", DrawText}, {"draw_line", DrawLine}, {"draw_circle", DrawCircle},
			{"key_pressed", KeyPressed}, {"wants_keyboard", WantsKeyboard}, {"mouse_pos", MousePos}, {"time", Time},
			{"delta_time", DeltaTime}, {"next_open", NextOpen}, {"input_text_hint", InputTextHint},
			{"align_text_to_frame", AlignTextToFrame}, {"text_size", TextSize}, {"frame_height", FrameHeight},
			{"window_rect", WindowRect}, {"overlay_rect", OverlayRect},
			{nullptr, nullptr},
		};

		struct Constant
		{
			const char* name;
			lua_Integer value;
		};
		const Constant kConstants[] = {
			{"WINDOW_NO_TITLE", MIRAGE_WINDOW_NO_TITLE}, {"WINDOW_NO_RESIZE", MIRAGE_WINDOW_NO_RESIZE},
			{"WINDOW_NO_MOVE", MIRAGE_WINDOW_NO_MOVE}, {"WINDOW_NO_SCROLLBAR", MIRAGE_WINDOW_NO_SCROLLBAR},
			{"WINDOW_NO_COLLAPSE", MIRAGE_WINDOW_NO_COLLAPSE}, {"WINDOW_AUTO_SIZE", MIRAGE_WINDOW_AUTO_SIZE},
			{"WINDOW_NO_BACKGROUND", MIRAGE_WINDOW_NO_BACKGROUND}, {"WINDOW_NO_INPUTS", MIRAGE_WINDOW_NO_INPUTS},
			{"WINDOW_MENU_ONLY", MIRAGE_WINDOW_MENU_ONLY},
			{"INPUT_ENTER_RETURNS", MIRAGE_INPUT_ENTER_RETURNS}, {"INPUT_READ_ONLY", MIRAGE_INPUT_READ_ONLY},
			{"INPUT_PASSWORD", MIRAGE_INPUT_PASSWORD}, {"INPUT_MULTILINE", MIRAGE_INPUT_MULTILINE},
			{"TABLE_RESIZABLE", MIRAGE_TABLE_RESIZABLE}, {"TABLE_ROW_BG", MIRAGE_TABLE_ROW_BG},
			{"TABLE_BORDERS", MIRAGE_TABLE_BORDERS}, {"TABLE_SIZING_FIXED", MIRAGE_TABLE_SIZING_FIXED},
			{"TABLE_SCROLL_Y", MIRAGE_TABLE_SCROLL_Y},
			{"COLUMN_STRETCH", MIRAGE_COLUMN_STRETCH}, {"COLUMN_FIXED", MIRAGE_COLUMN_FIXED},
			{"COLOR_TEXT", MIRAGE_COLOR_TEXT}, {"COLOR_TEXT_DISABLED", MIRAGE_COLOR_TEXT_DISABLED},
			{"COLOR_WINDOW_BG", MIRAGE_COLOR_WINDOW_BG}, {"COLOR_CHILD_BG", MIRAGE_COLOR_CHILD_BG},
			{"COLOR_BORDER", MIRAGE_COLOR_BORDER}, {"COLOR_FRAME_BG", MIRAGE_COLOR_FRAME_BG},
			{"COLOR_BUTTON", MIRAGE_COLOR_BUTTON}, {"COLOR_BUTTON_HOVERED", MIRAGE_COLOR_BUTTON_HOVERED},
			{"COLOR_BUTTON_ACTIVE", MIRAGE_COLOR_BUTTON_ACTIVE}, {"COLOR_HEADER", MIRAGE_COLOR_HEADER},
			{"COLOR_PLOT_LINES", MIRAGE_COLOR_PLOT_LINES}, {"COLOR_PLOT_HISTOGRAM", MIRAGE_COLOR_PLOT_HISTOGRAM},
		};
	}

	void Push(lua_State* L, const MirageUi* ui)
	{
		lua_newtable(L);
		for (const luaL_Reg* r = kFns; r->name; ++r) {
			lua_pushlightuserdata(L, const_cast<MirageUi*>(ui));
			lua_pushcclosure(L, r->func, 1);
			lua_setfield(L, -2, r->name);
		}
		for (const Constant& c : kConstants) {
			lua_pushinteger(L, c.value);
			lua_setfield(L, -2, c.name);
		}
	}
}
