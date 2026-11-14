// The Object Inspector (Mirage, step 16): every property of the selected object (editor::Select: the World Explorer, a
// click on an object reference here, or "The player"), live, with structs and arrays opened in place. Numbers, bools
// and names are edited in place; each edit is an undoable step of the change log (Undo puts the old value back).
// var() properties (the ones the class lets an editor change) are marked. The rows are read on the game thread
// five times a second while the window is shown (editor_core); edits are written there too.

#include "features/editor/editor_core.h"

#include "core/module.h"
#include "game/game.h"
#include "gui.h"
#include "sdk/SDK.hpp"
#include "theme.h"

#include <imgui.h>

#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace
{
	class ObjectInspector final : public Module
	{
	public:
		ObjectInspector() : Module("Object Inspector", "Debug", "Every property of the selected object, live and editable (undoable)", 31) {}

		void OnTick() override
		{
			std::vector<std::pair<std::string, std::string>> writes;
			Classes::UObject* select = nullptr;
			bool undo = false, player = false;
			std::string filter;
			std::set<std::string> opened;
			{
				std::lock_guard lock(mutex_);
				writes.swap(pendingWrites_);
				select = pendingSelect_;
				pendingSelect_ = nullptr;
				undo = pendingUndo_;
				pendingUndo_ = false;
				player = pendingPlayer_;
				pendingPlayer_ = false;
				filter = filter_;
				opened = expanded_;
			}
			if (player) editor::Select(game::Pawn());
			if (select) editor::Select(select);
			for (const auto& [path, text] : writes) {
				std::string error;
				const bool ok = editor::SetValue(editor::Selected(), path, text, &error);
				std::lock_guard lock(mutex_);
				status_ = ok ? path + " = " + text : error;
			}
			if (undo) {
				const bool ok = editor::Undo();
				std::lock_guard lock(mutex_);
				status_ = ok ? "undone" : "nothing to undo";
			}
			const auto now = std::chrono::steady_clock::now();
			const bool changed = !writes.empty() || undo || select || player;
			if (!shown_.load() || (!changed && now - lastRead_ < std::chrono::milliseconds(200))) return;
			lastRead_ = now;
			Classes::UObject* o = editor::Selected();
			std::vector<editor::Row> rows = editor::Rows(o, filter, opened);
			std::lock_guard lock(mutex_);
			rows_ = std::move(rows);
			gone_ = !o && !editor::SelectedLabel().empty();
		}

		void OnRender() override
		{
			shown_ = gui::visible;
			if (!gui::visible) return;
			editor::NoteFrame(editor::Window::Inspector);
			theme::Begin();
			ImGui::SetNextWindowPos({480.f, 160.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({520.f, 620.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({340.f, 220.f}, {FLT_MAX, FLT_MAX});
			if (ImGui::Begin("Object Inspector", nullptr, ImGuiWindowFlags_NoCollapse)) Draw();
			ImGui::End();
			theme::End();
		}

		void OnDisable() override { shown_ = false; }

	private:
		void Draw()
		{
			std::lock_guard lock(mutex_);
			const std::string label = editor::SelectedLabel();
			ImGui::Text("%s", label.empty() ? "Nothing selected" : label.c_str());
			if (gone_) {
				ImGui::SameLine();
				ImGui::TextColored({0.95f, 0.5f, 0.3f, 1.f}, "(gone)");
			}
			if (ImGui::Button("The player")) pendingPlayer_ = true;
			ImGui::SameLine();
			const std::size_t depth = editor::UndoDepth();
			ImGui::BeginDisabled(depth == 0);
			const std::string undo = "Undo (" + std::to_string(depth) + ")";
			if (ImGui::Button(undo.c_str())) pendingUndo_ = true;
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::SetNextItemWidth(-1.f);
			ImGui::InputTextWithHint("##filter", "property", filter_, sizeof filter_);
			if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());

			if (!ImGui::BeginTable("##props", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) return;
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, 90.f);
			ImGui::TableHeadersRow();
			for (const editor::Row& r : rows_) {
				ImGui::PushID(r.path.c_str());
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Indent(static_cast<float>(r.depth) * 14.f);
				if (r.expandable) {
					const bool open = expanded_.count(r.path) != 0;
					if (ImGui::SmallButton(open ? "-" : "+")) {
						if (open) expanded_.erase(r.path);
						else expanded_.insert(r.path);
					}
					ImGui::SameLine();
				}
				if (r.editorOnly) ImGui::TextColored({0.6f, 0.85f, 1.f, 1.f}, "%s", r.name.c_str());
				else ImGui::TextUnformatted(r.name.c_str());
				if (r.editorOnly && ImGui::IsItemHovered()) ImGui::SetTooltip("var(): the class lets an editor change it");
				ImGui::Unindent(static_cast<float>(r.depth) * 14.f);
				ImGui::TableNextColumn();
				DrawValue(r);
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", r.kind.c_str());
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		void DrawValue(const editor::Row& r)
		{
			if (r.kind == "bool") {
				bool on = r.value == "true";
				if (ImGui::Checkbox("##v", &on)) pendingWrites_.emplace_back(r.path, on ? "true" : "false");
				return;
			}
			if (r.editable) {
				// Typed in: written on Enter (the row keeps refreshing, so the field shows the live value until then)
				char buf[256];
				const bool editing = editing_ == r.path;
				std::snprintf(buf, sizeof buf, "%s", editing ? editText_.c_str() : r.value.c_str());
				ImGui::SetNextItemWidth(-1.f);
				if (ImGui::InputText("##v", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
					pendingWrites_.emplace_back(r.path, buf);
					editing_.clear();
				} else if (ImGui::IsItemActive()) {
					editing_ = r.path;
					editText_ = buf;
				} else if (editing) {
					editing_.clear();
				}
				return;
			}
			if (r.ref) {
				if (ImGui::Selectable(r.value.c_str())) pendingSelect_ = r.ref;
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select it");
				return;
			}
			ImGui::TextDisabled("%s", r.value.c_str());
		}

		std::mutex mutex_;
		std::vector<editor::Row> rows_;
		std::vector<std::pair<std::string, std::string>> pendingWrites_;
		Classes::UObject* pendingSelect_ = nullptr;
		bool pendingUndo_ = false, pendingPlayer_ = false;
		std::set<std::string> expanded_;
		char filter_[96] = "";
		std::string status_, editing_, editText_;
		bool gone_ = false;
		std::atomic<bool> shown_{false};
		std::chrono::steady_clock::time_point lastRead_{};
	};
	MEM_MODULE(ObjectInspector)
}
