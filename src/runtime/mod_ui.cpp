// The UI layer of the mod API (MirageUi): the menu's ImGui through a C table. Mirage's flag and color values are its
// own (the ABI), translated here, so an ImGui update never changes what a mod's numbers mean.
#include "runtime/mod_ui.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace mirage::mods::ui
{
	namespace
	{
		bool Ready() { return ImGui::GetCurrentContext() != nullptr; }
		const char* S(const char* s) { return s ? s : ""; }
		ImU32 C(uint32_t c) { return static_cast<ImU32>(c); }

		ImGuiWindowFlags WindowFlags(uint32_t f)
		{
			ImGuiWindowFlags r = 0;
			if (f & MIRAGE_WINDOW_NO_TITLE) r |= ImGuiWindowFlags_NoTitleBar;
			if (f & MIRAGE_WINDOW_NO_RESIZE) r |= ImGuiWindowFlags_NoResize;
			if (f & MIRAGE_WINDOW_NO_MOVE) r |= ImGuiWindowFlags_NoMove;
			if (f & MIRAGE_WINDOW_NO_SCROLLBAR) r |= ImGuiWindowFlags_NoScrollbar;
			if (f & MIRAGE_WINDOW_NO_COLLAPSE) r |= ImGuiWindowFlags_NoCollapse;
			if (f & MIRAGE_WINDOW_AUTO_SIZE) r |= ImGuiWindowFlags_AlwaysAutoResize;
			if (f & MIRAGE_WINDOW_NO_BACKGROUND) r |= ImGuiWindowFlags_NoBackground;
			if ((f & MIRAGE_WINDOW_NO_INPUTS) == MIRAGE_WINDOW_NO_INPUTS)
				r |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus;
			return r;
		}

		ImGuiCol Color(int32_t what)
		{
			switch (what) {
				case MIRAGE_COLOR_TEXT: return ImGuiCol_Text;
				case MIRAGE_COLOR_TEXT_DISABLED: return ImGuiCol_TextDisabled;
				case MIRAGE_COLOR_WINDOW_BG: return ImGuiCol_WindowBg;
				case MIRAGE_COLOR_CHILD_BG: return ImGuiCol_ChildBg;
				case MIRAGE_COLOR_BORDER: return ImGuiCol_Border;
				case MIRAGE_COLOR_FRAME_BG: return ImGuiCol_FrameBg;
				case MIRAGE_COLOR_BUTTON: return ImGuiCol_Button;
				case MIRAGE_COLOR_BUTTON_HOVERED: return ImGuiCol_ButtonHovered;
				case MIRAGE_COLOR_BUTTON_ACTIVE: return ImGuiCol_ButtonActive;
				case MIRAGE_COLOR_HEADER: return ImGuiCol_Header;
				case MIRAGE_COLOR_PLOT_LINES: return ImGuiCol_PlotLines;
				case MIRAGE_COLOR_PLOT_HISTOGRAM: return ImGuiCol_PlotHistogram;
				default: return -1;
			}
		}

		// Windows a mod opened, so a forgotten window_end can be closed for it (Guard)
		thread_local int t_windows = 0;
		thread_local int t_colors = 0;

		// ---- Windows ---------------------------------------------------------------------------------------------
		int32_t MIRAGE_CALL WindowBegin(const char* title, int32_t* open, uint32_t flags)
		{
			if (!Ready() || !title) return 0;
			++t_windows;
			if ((flags & MIRAGE_WINDOW_MENU_ONLY) && !MenuVisible()) {
				// Still a Begin (window_end follows), but nothing shows
				ImGui::SetNextWindowPos({-10000.f, -10000.f}, ImGuiCond_Always);
				ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);
				return 0;
			}
			bool b = open ? *open != 0 : true;
			const bool shown = ImGui::Begin(title, open ? &b : nullptr, WindowFlags(flags));
			if (open) *open = b ? 1 : 0;
			return shown ? 1 : 0;
		}
		void MIRAGE_CALL WindowEnd()
		{
			if (!Ready() || t_windows <= 0) return;
			--t_windows;
			ImGui::End();
		}
		void MIRAGE_CALL WindowNextPos(float x, float y, int32_t always)
		{
			if (!Ready()) return;
			const ImVec2 d = ImGui::GetIO().DisplaySize;
			const bool center = x < 0.f || y < 0.f;
			ImGui::SetNextWindowPos(center ? ImVec2{d.x * 0.5f, d.y * 0.5f} : ImVec2{x, y},
									always ? ImGuiCond_Always : ImGuiCond_FirstUseEver, center ? ImVec2{0.5f, 0.5f} : ImVec2{});
		}
		void MIRAGE_CALL WindowNextSize(float w, float h, int32_t always)
		{
			if (Ready()) ImGui::SetNextWindowSize({w, h}, always ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
		}
		void MIRAGE_CALL WindowNextBgAlpha(float a) { if (Ready()) ImGui::SetNextWindowBgAlpha(a); }
		int32_t MIRAGE_CALL ChildBegin(const char* id, float w, float h, int32_t border)
		{
			if (!Ready()) return 0;
			++t_windows;
			return ImGui::BeginChild(S(id), {w, h}, border ? ImGuiChildFlags_Borders : ImGuiChildFlags_None) ? 1 : 0;
		}
		void MIRAGE_CALL ChildEnd()
		{
			if (!Ready() || t_windows <= 0) return;
			--t_windows;
			ImGui::EndChild();
		}

		// ---- Layout ----------------------------------------------------------------------------------------------
		void MIRAGE_CALL SameLine(float offset, float spacing) { if (Ready()) ImGui::SameLine(offset, spacing); }
		void MIRAGE_CALL Separator() { if (Ready()) ImGui::Separator(); }
		void MIRAGE_CALL Spacing() { if (Ready()) ImGui::Spacing(); }
		void MIRAGE_CALL Indent(float w) { if (Ready()) ImGui::Indent(w); }
		void MIRAGE_CALL Unindent(float w) { if (Ready()) ImGui::Unindent(w); }
		void MIRAGE_CALL NextWidth(float w) { if (Ready()) ImGui::SetNextItemWidth(w); }
		float MIRAGE_CALL ContentWidth() { return Ready() ? ImGui::GetContentRegionAvail().x : 0.f; }
		float MIRAGE_CALL ContentHeight() { return Ready() ? ImGui::GetContentRegionAvail().y : 0.f; }
		float MIRAGE_CALL LineHeight() { return Ready() ? ImGui::GetTextLineHeightWithSpacing() : 0.f; }
		void MIRAGE_CALL CursorScreenPos(float* x, float* y)
		{
			const ImVec2 p = Ready() ? ImGui::GetCursorScreenPos() : ImVec2{};
			if (x) *x = p.x;
			if (y) *y = p.y;
		}
		void MIRAGE_CALL Dummy(float w, float h) { if (Ready()) ImGui::Dummy({w, h}); }
		void MIRAGE_CALL IdPush(const char* id) { if (Ready()) ImGui::PushID(S(id)); }
		void MIRAGE_CALL IdPushInt(int32_t id) { if (Ready()) ImGui::PushID(id); }
		void MIRAGE_CALL IdPop() { if (Ready()) ImGui::PopID(); }
		void MIRAGE_CALL DisabledBegin(int32_t d) { if (Ready()) ImGui::BeginDisabled(d != 0); }
		void MIRAGE_CALL DisabledEnd() { if (Ready()) ImGui::EndDisabled(); }
		void MIRAGE_CALL ColorPush(int32_t what, uint32_t color)
		{
			const ImGuiCol c = Color(what);
			if (!Ready() || c < 0) return;
			ImGui::PushStyleColor(c, C(color));
			++t_colors;
		}
		void MIRAGE_CALL ColorPop(int32_t count)
		{
			count = std::min(count, t_colors);
			if (!Ready() || count <= 0) return;
			ImGui::PopStyleColor(count);
			t_colors -= count;
		}
		void MIRAGE_CALL FontScale(float s) { if (Ready()) ImGui::SetWindowFontScale(s > 0.f ? s : 1.f); }

		// ---- Text ------------------------------------------------------------------------------------------------
		void MIRAGE_CALL Text(const char* t) { if (Ready()) ImGui::TextUnformatted(S(t)); }
		void MIRAGE_CALL TextColored(uint32_t c, const char* t)
		{
			if (!Ready()) return;
			ImGui::PushStyleColor(ImGuiCol_Text, C(c));
			ImGui::TextUnformatted(S(t));
			ImGui::PopStyleColor();
		}
		void MIRAGE_CALL TextDisabled(const char* t)
		{
			if (!Ready()) return;
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
			ImGui::TextUnformatted(S(t));
			ImGui::PopStyleColor();
		}
		void MIRAGE_CALL TextWrapped(const char* t)
		{
			if (!Ready()) return;
			ImGui::PushTextWrapPos(0.f);
			ImGui::TextUnformatted(S(t));
			ImGui::PopTextWrapPos();
		}
		void MIRAGE_CALL LabelText(const char* label, const char* t) { if (Ready()) ImGui::LabelText(S(label), "%s", S(t)); }
		void MIRAGE_CALL BulletText(const char* t) { if (Ready()) ImGui::BulletText("%s", S(t)); }

		// ---- Widgets ---------------------------------------------------------------------------------------------
		int32_t MIRAGE_CALL Button(const char* l, float w, float h) { return Ready() && ImGui::Button(S(l), {w, h}); }
		int32_t MIRAGE_CALL SmallButton(const char* l) { return Ready() && ImGui::SmallButton(S(l)); }
		int32_t MIRAGE_CALL Checkbox(const char* l, int32_t* v)
		{
			if (!Ready() || !v) return 0;
			bool b = *v != 0;
			const bool changed = ImGui::Checkbox(S(l), &b);
			*v = b ? 1 : 0;
			return changed;
		}
		int32_t MIRAGE_CALL Radio(const char* l, int32_t active) { return Ready() && ImGui::RadioButton(S(l), active != 0); }
		int32_t MIRAGE_CALL SliderFloat(const char* l, float* v, float mn, float mx, const char* fmt)
		{
			return Ready() && v && ImGui::SliderFloat(S(l), v, mn, mx, fmt ? fmt : "%.3f");
		}
		int32_t MIRAGE_CALL SliderInt(const char* l, int32_t* v, int32_t mn, int32_t mx, const char* fmt)
		{
			return Ready() && v && ImGui::SliderInt(S(l), reinterpret_cast<int*>(v), mn, mx, fmt ? fmt : "%d");
		}
		int32_t MIRAGE_CALL DragFloat(const char* l, float* v, float speed, float mn, float mx, const char* fmt)
		{
			return Ready() && v && ImGui::DragFloat(S(l), v, speed, mn, mx, fmt ? fmt : "%.3f");
		}
		int32_t MIRAGE_CALL DragInt(const char* l, int32_t* v, float speed, int32_t mn, int32_t mx)
		{
			return Ready() && v && ImGui::DragInt(S(l), reinterpret_cast<int*>(v), speed, mn, mx);
		}
		int32_t MIRAGE_CALL InputInt(const char* l, int32_t* v, int32_t step)
		{
			return Ready() && v && ImGui::InputInt(S(l), reinterpret_cast<int*>(v), step);
		}
		int32_t MIRAGE_CALL InputFloat(const char* l, float* v, float step, const char* fmt)
		{
			return Ready() && v && ImGui::InputFloat(S(l), v, step, step * 10.f, fmt ? fmt : "%.3f");
		}
		int32_t MIRAGE_CALL InputText(const char* l, char* buf, uint32_t cap, uint32_t flags, float h)
		{
			if (!Ready() || !buf || cap == 0) return 0;
			ImGuiInputTextFlags f = 0;
			if (flags & MIRAGE_INPUT_ENTER_RETURNS) f |= ImGuiInputTextFlags_EnterReturnsTrue;
			if (flags & MIRAGE_INPUT_READ_ONLY) f |= ImGuiInputTextFlags_ReadOnly;
			if (flags & MIRAGE_INPUT_PASSWORD) f |= ImGuiInputTextFlags_Password;
			if (flags & MIRAGE_INPUT_MULTILINE) return ImGui::InputTextMultiline(S(l), buf, cap, {0.f, h}, f);
			return ImGui::InputText(S(l), buf, cap, f);
		}
		int32_t MIRAGE_CALL Combo(const char* l, int32_t* current, const char* items)
		{
			if (!Ready() || !current || !items) return 0;
			int c = *current;
			const bool changed = ImGui::Combo(S(l), &c, items);
			*current = c;
			return changed;
		}
		int32_t MIRAGE_CALL Selectable(const char* l, int32_t selected, float w, float h)
		{
			return Ready() && ImGui::Selectable(S(l), selected != 0, 0, {w, h});
		}
		int32_t MIRAGE_CALL ColorEdit(const char* l, uint32_t* rgba)
		{
			if (!Ready() || !rgba) return 0;
			ImVec4 c = ImGui::ColorConvertU32ToFloat4(*rgba);
			const bool changed = ImGui::ColorEdit4(S(l), &c.x, ImGuiColorEditFlags_AlphaBar);
			*rgba = ImGui::ColorConvertFloat4ToU32(c);
			return changed;
		}
		void MIRAGE_CALL ProgressBar(float fraction, float w, float h, const char* overlay)
		{
			if (Ready()) ImGui::ProgressBar(fraction, {w, h}, overlay);
		}
		void MIRAGE_CALL PlotLines(const char* l, const float* v, int32_t n, float mn, float mx, float w, float h, const char* overlay)
		{
			if (!Ready() || !v || n <= 0) return;
			const bool autoRange = mn == mx;
			ImGui::PlotLines(S(l), v, n, 0, overlay, autoRange ? FLT_MAX : mn, autoRange ? FLT_MAX : mx, {w, h});
		}
		void MIRAGE_CALL PlotHistogram(const char* l, const float* v, int32_t n, float mn, float mx, float w, float h, const char* overlay)
		{
			if (!Ready() || !v || n <= 0) return;
			const bool autoRange = mn == mx;
			ImGui::PlotHistogram(S(l), v, n, 0, overlay, autoRange ? FLT_MAX : mn, autoRange ? FLT_MAX : mx, {w, h});
		}

		// ---- Trees and tables ------------------------------------------------------------------------------------
		int32_t MIRAGE_CALL TreeNode(const char* l, int32_t open)
		{
			return Ready() && ImGui::TreeNodeEx(S(l), open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
		}
		void MIRAGE_CALL TreePop() { if (Ready()) ImGui::TreePop(); }
		int32_t MIRAGE_CALL CollapsingHeader(const char* l, int32_t open)
		{
			return Ready() && ImGui::CollapsingHeader(S(l), open ? ImGuiTreeNodeFlags_DefaultOpen : 0);
		}
		int32_t MIRAGE_CALL TableBegin(const char* id, int32_t columns, uint32_t flags, float w, float h)
		{
			if (!Ready() || columns <= 0 || columns > 64) return 0;
			ImGuiTableFlags f = 0;
			if (flags & MIRAGE_TABLE_RESIZABLE) f |= ImGuiTableFlags_Resizable;
			if (flags & MIRAGE_TABLE_ROW_BG) f |= ImGuiTableFlags_RowBg;
			if (flags & MIRAGE_TABLE_BORDERS) f |= ImGuiTableFlags_Borders;
			if (flags & MIRAGE_TABLE_SIZING_FIXED) f |= ImGuiTableFlags_SizingFixedFit;
			if (flags & MIRAGE_TABLE_SCROLL_Y) f |= ImGuiTableFlags_ScrollY;
			return ImGui::BeginTable(S(id), columns, f, {w, h});
		}
		void MIRAGE_CALL TableColumn(const char* name, uint32_t flags, float w)
		{
			if (!Ready()) return;
			ImGuiTableColumnFlags f = 0;
			if (flags & MIRAGE_COLUMN_STRETCH) f |= ImGuiTableColumnFlags_WidthStretch;
			if (flags & MIRAGE_COLUMN_FIXED) f |= ImGuiTableColumnFlags_WidthFixed;
			ImGui::TableSetupColumn(S(name), f, w);
		}
		void MIRAGE_CALL TableHeaders() { if (Ready()) ImGui::TableHeadersRow(); }
		void MIRAGE_CALL TableNextRow() { if (Ready()) ImGui::TableNextRow(); }
		int32_t MIRAGE_CALL TableNextColumn() { return Ready() && ImGui::TableNextColumn(); }
		void MIRAGE_CALL TableEnd() { if (Ready()) ImGui::EndTable(); }

		// ---- Items and popups ------------------------------------------------------------------------------------
		int32_t MIRAGE_CALL ItemHovered() { return Ready() && ImGui::IsItemHovered(); }
		int32_t MIRAGE_CALL ItemClicked(int32_t b) { return Ready() && b >= 0 && b < 3 && ImGui::IsItemClicked(b); }
		int32_t MIRAGE_CALL ItemActive() { return Ready() && ImGui::IsItemActive(); }
		void MIRAGE_CALL ItemRect(float* x1, float* y1, float* x2, float* y2)
		{
			const ImVec2 a = Ready() ? ImGui::GetItemRectMin() : ImVec2{}, b = Ready() ? ImGui::GetItemRectMax() : ImVec2{};
			if (x1) *x1 = a.x;
			if (y1) *y1 = a.y;
			if (x2) *x2 = b.x;
			if (y2) *y2 = b.y;
		}
		void MIRAGE_CALL Tooltip(const char* t) { if (Ready() && t) ImGui::SetTooltip("%s", t); }
		void MIRAGE_CALL PopupOpen(const char* id) { if (Ready()) ImGui::OpenPopup(S(id)); }
		int32_t MIRAGE_CALL PopupBegin(const char* id) { return Ready() && ImGui::BeginPopup(S(id)); }
		int32_t MIRAGE_CALL PopupContextItem(const char* id) { return Ready() && ImGui::BeginPopupContextItem(id); }
		void MIRAGE_CALL PopupEnd() { if (Ready()) ImGui::EndPopup(); }
		void MIRAGE_CALL PopupClose() { if (Ready()) ImGui::CloseCurrentPopup(); }
		void MIRAGE_CALL FocusKeyboard() { if (Ready()) ImGui::SetKeyboardFocusHere(); }
		float MIRAGE_CALL ScrollY() { return Ready() ? ImGui::GetScrollY() : 0.f; }
		float MIRAGE_CALL ScrollMaxY() { return Ready() ? ImGui::GetScrollMaxY() : 0.f; }
		void MIRAGE_CALL ScrollToBottom() { if (Ready()) ImGui::SetScrollHereY(1.f); }

		// ---- Drawing in the window -------------------------------------------------------------------------------
		ImDrawList* WindowList() { return Ready() ? ImGui::GetWindowDrawList() : nullptr; }
		void MIRAGE_CALL WindowDrawRect(float x1, float y1, float x2, float y2, uint32_t c, float rounding, float thickness)
		{
			ImDrawList* dl = WindowList();
			if (!dl) return;
			if (thickness <= 0.f) dl->AddRectFilled({x1, y1}, {x2, y2}, C(c), rounding);
			else dl->AddRect({x1, y1}, {x2, y2}, C(c), rounding, 0, thickness);
		}
		void MIRAGE_CALL WindowDrawText(float x, float y, uint32_t c, const char* t)
		{
			if (ImDrawList* dl = WindowList(); dl && t) dl->AddText({x, y}, C(c), t);
		}
		void MIRAGE_CALL WindowDrawLine(float x1, float y1, float x2, float y2, uint32_t c, float thickness)
		{
			if (ImDrawList* dl = WindowList()) dl->AddLine({x1, y1}, {x2, y2}, C(c), thickness);
		}
		void MIRAGE_CALL WindowDrawCircle(float x, float y, float r, uint32_t c, float thickness)
		{
			ImDrawList* dl = WindowList();
			if (!dl) return;
			if (thickness <= 0.f) dl->AddCircleFilled({x, y}, r, C(c));
			else dl->AddCircle({x, y}, r, C(c), 0, thickness);
		}

		// ---- Input -----------------------------------------------------------------------------------------------
		int32_t MIRAGE_CALL KeyPressed(int32_t vk)
		{
			if (!Ready()) return 0;
			const ImGuiKey k = KeyFromVk(vk);
			return k != ImGuiKey_None && ImGui::IsKeyPressed(k);
		}
		int32_t MIRAGE_CALL WantsKeyboard() { return Ready() && ImGui::GetIO().WantTextInput; }
		void MIRAGE_CALL MousePos(float* x, float* y)
		{
			const ImVec2 p = Ready() ? ImGui::GetIO().MousePos : ImVec2{};
			if (x) *x = p.x;
			if (y) *y = p.y;
		}
		float MIRAGE_CALL Time() { return Ready() ? static_cast<float>(ImGui::GetTime()) : 0.f; }
		float MIRAGE_CALL DeltaTime() { return Ready() ? ImGui::GetIO().DeltaTime : 0.f; }

		void MIRAGE_CALL NextOpen(int32_t open, int32_t always)
		{
			if (Ready()) ImGui::SetNextItemOpen(open != 0, always ? ImGuiCond_Always : ImGuiCond_Once);
		}
		int32_t MIRAGE_CALL InputTextHint(const char* l, const char* hint, char* buf, uint32_t cap, uint32_t flags)
		{
			if (!Ready() || !buf || cap == 0) return 0;
			ImGuiInputTextFlags f = 0;
			if (flags & MIRAGE_INPUT_ENTER_RETURNS) f |= ImGuiInputTextFlags_EnterReturnsTrue;
			if (flags & MIRAGE_INPUT_READ_ONLY) f |= ImGuiInputTextFlags_ReadOnly;
			if (flags & MIRAGE_INPUT_PASSWORD) f |= ImGuiInputTextFlags_Password;
			return ImGui::InputTextWithHint(S(l), S(hint), buf, cap, f);
		}
		void MIRAGE_CALL AlignTextToFrame() { if (Ready()) ImGui::AlignTextToFramePadding(); }
		void MIRAGE_CALL TextSize(const char* t, float* w, float* h)
		{
			const ImVec2 s = Ready() && t ? ImGui::CalcTextSize(t) : ImVec2{};
			if (w) *w = s.x;
			if (h) *h = s.y;
		}
		float MIRAGE_CALL FrameHeight() { return Ready() ? ImGui::GetFrameHeight() : 0.f; }

		void MIRAGE_CALL WindowRect(float* x1, float* y1, float* x2, float* y2)
		{
			ImVec2 a{}, s{};
			if (Ready()) {
				a = ImGui::GetWindowPos();
				s = ImGui::GetWindowSize();
			}
			if (x1) *x1 = a.x;
			if (y1) *y1 = a.y;
			if (x2) *x2 = a.x + s.x;
			if (y2) *y2 = a.y + s.y;
		}
		void MIRAGE_CALL OverlayRect(float x1, float y1, float x2, float y2, uint32_t c, float rounding, float thickness)
		{
			if (!Ready()) return;
			ImDrawList* dl = ImGui::GetForegroundDrawList();
			if (thickness <= 0.f) dl->AddRectFilled({x1, y1}, {x2, y2}, C(c), rounding);
			else dl->AddRect({x1, y1}, {x2, y2}, C(c), rounding, 0, thickness);
		}

		void MIRAGE_CALL WindowDrawTextSized(float x, float y, uint32_t c, float size, const char* t)
		{
			if (ImDrawList* dl = WindowList(); dl && t) dl->AddText(nullptr, size > 0.f ? size : ImGui::GetFontSize(), {x, y}, C(c), t);
		}
		thread_local int t_styles = 0;
		void MIRAGE_CALL StylePush(int32_t what, float x, float y)
		{
			if (!Ready()) return;
			switch (what) {
				case MIRAGE_STYLE_WINDOW_PADDING: ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {x, y}); break;
				case MIRAGE_STYLE_WINDOW_BORDER: ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, x); break;
				case MIRAGE_STYLE_ITEM_SPACING: ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {x, y}); break;
				case MIRAGE_STYLE_FRAME_PADDING: ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {x, y}); break;
				default: return;
			}
			++t_styles;
		}
		void MIRAGE_CALL StylePop(int32_t count)
		{
			count = std::min(count, t_styles);
			if (!Ready() || count <= 0) return;
			ImGui::PopStyleVar(count);
			t_styles -= count;
		}

		const MirageUi kUi = {
			sizeof(MirageUi),
			WindowBegin, WindowEnd, WindowNextPos, WindowNextSize, WindowNextBgAlpha, ChildBegin, ChildEnd,
			SameLine, Separator, Spacing, Indent, Unindent, NextWidth, ContentWidth, ContentHeight, LineHeight,
			CursorScreenPos, Dummy, IdPush, IdPushInt, IdPop, DisabledBegin, DisabledEnd, ColorPush, ColorPop, FontScale,
			Text, TextColored, TextDisabled, TextWrapped, LabelText, BulletText,
			Button, SmallButton, Checkbox, Radio, SliderFloat, SliderInt, DragFloat, DragInt, InputInt, InputFloat,
			InputText, Combo, Selectable, ColorEdit, ProgressBar, PlotLines, PlotHistogram,
			TreeNode, TreePop, CollapsingHeader,
			TableBegin, TableColumn, TableHeaders, TableNextRow, TableNextColumn, TableEnd,
			ItemHovered, ItemClicked, ItemActive, ItemRect, Tooltip, PopupOpen, PopupBegin, PopupContextItem, PopupEnd,
			PopupClose, FocusKeyboard,
			ScrollY, ScrollMaxY, ScrollToBottom,
			WindowDrawRect, WindowDrawText, WindowDrawLine, WindowDrawCircle,
			KeyPressed, WantsKeyboard, MousePos, Time, DeltaTime,
			NextOpen, InputTextHint, AlignTextToFrame, TextSize, FrameHeight, WindowRect, OverlayRect, WindowDrawTextSized, StylePush,
			StylePop,
		};
	}

	const MirageUi* Table() { return &kUi; }

	ImGuiKey KeyFromVk(int vk)
	{
		if (vk >= '0' && vk <= '9') return static_cast<ImGuiKey>(ImGuiKey_0 + (vk - '0'));
		if (vk >= 'A' && vk <= 'Z') return static_cast<ImGuiKey>(ImGuiKey_A + (vk - 'A'));
		if (vk >= 0x70 && vk <= 0x7B) return static_cast<ImGuiKey>(ImGuiKey_F1 + (vk - 0x70));
		if (vk >= 0x60 && vk <= 0x69) return static_cast<ImGuiKey>(ImGuiKey_Keypad0 + (vk - 0x60));
		switch (vk) {
			case 0x08: return ImGuiKey_Backspace;
			case 0x09: return ImGuiKey_Tab;
			case 0x0D: return ImGuiKey_Enter;
			case 0x1B: return ImGuiKey_Escape;
			case 0x20: return ImGuiKey_Space;
			case 0x21: return ImGuiKey_PageUp;
			case 0x22: return ImGuiKey_PageDown;
			case 0x23: return ImGuiKey_End;
			case 0x24: return ImGuiKey_Home;
			case 0x25: return ImGuiKey_LeftArrow;
			case 0x26: return ImGuiKey_UpArrow;
			case 0x27: return ImGuiKey_RightArrow;
			case 0x28: return ImGuiKey_DownArrow;
			case 0x2D: return ImGuiKey_Insert;
			case 0x2E: return ImGuiKey_Delete;
			case 0xC0: return ImGuiKey_GraveAccent;
			default: return ImGuiKey_None;
		}
	}

	// Around a mod's render-thread callback (mod_ui.h)
	Guard::Guard()
	{
		windows_ = t_windows;
		colors_ = t_colors;
		styles_ = t_styles;
		t_windows = 0;
		t_colors = 0;
		t_styles = 0;
		if (Ready()) {
			state_ = new ImGuiErrorRecoveryState;
			ImGui::ErrorRecoveryStoreState(static_cast<ImGuiErrorRecoveryState*>(state_));
		}
	}
	Guard::~Guard()
	{
		if (state_) {
			// ImGui closes what was left open since the store (windows, children, colors, IDs, ...), quietly
			ImGuiIO& io = ImGui::GetIO();
			const bool assertWas = io.ConfigErrorRecoveryEnableAssert;
			io.ConfigErrorRecoveryEnableAssert = false;
			ImGui::ErrorRecoveryTryToRecoverState(static_cast<ImGuiErrorRecoveryState*>(state_));
			io.ConfigErrorRecoveryEnableAssert = assertWas;
			delete static_cast<ImGuiErrorRecoveryState*>(state_);
		}
		t_windows = windows_;
		t_colors = colors_;
		t_styles = styles_;
	}
}
