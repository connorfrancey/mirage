// The World Explorer (Mirage, step 16): every actor in play, nearest first or grouped by class, filtered by name or
// class; click one to select it for the Object Inspector (and every other editor), or pick the actor under the
// crosshair. The list is read on the game thread once a second while the window is shown (editor_core).

#include "features/editor/editor_core.h"

#include "core/module.h"
#include "gui.h"
#include "theme.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}

	class WorldExplorer final : public Module
	{
	public:
		WorldExplorer() : Module("World Explorer", "Debug", "Every actor in play: find one and select it for the Object Inspector", 30) {}

		void OnTick() override
		{
			Classes::UObject* select = nullptr;
			bool pick = false;
			{
				std::lock_guard lock(mutex_);
				select = pendingSelect_;
				pendingSelect_ = nullptr;
				pick = pendingPick_;
				pendingPick_ = false;
			}
			if (select) editor::Select(select);		// Checked there: a pointer that is gone since selects nothing
			if (pick) editor::Select(editor::LookedAt());
			const auto now = std::chrono::steady_clock::now();
			if (!shown_.load() || now - lastRead_ < std::chrono::milliseconds(1000)) return;
			lastRead_ = now;
			std::vector<editor::ActorRow> rows = editor::Actors();
			std::lock_guard lock(mutex_);
			rows_ = std::move(rows);
		}

		void OnRender() override
		{
			shown_ = gui::visible;
			if (!gui::visible) return;
			editor::NoteFrame(editor::Window::Explorer);
			theme::Begin();
			ImGui::SetNextWindowPos({40.f, 160.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({420.f, 520.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({300.f, 200.f}, {FLT_MAX, FLT_MAX});
			if (ImGui::Begin("World Explorer", nullptr, ImGuiWindowFlags_NoCollapse)) Draw();
			ImGui::End();
			theme::End();
		}

		void OnDisable() override { shown_ = false; }

	private:
		void Draw()
		{
			std::lock_guard lock(mutex_);
			ImGui::SetNextItemWidth(-160.f);
			ImGui::InputTextWithHint("##filter", "name or class", filter_, sizeof filter_);
			ImGui::SameLine();
			ImGui::Checkbox("By class", &byClass_);
			ImGui::SameLine();
			if (ImGui::Button("Look at")) pendingPick_ = true;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("Select the actor under the crosshair");
			const std::string selected = editor::SelectedLabel();
			ImGui::TextDisabled("%zu actors in play; selected: %s", rows_.size(), selected.empty() ? "none" : selected.c_str());

			const std::string f = Lower(filter_);
			const auto matches = [&](const editor::ActorRow& r) {
				return f.empty() || Lower(r.name).find(f) != std::string::npos || Lower(r.cls).find(f) != std::string::npos;
			};
			const auto row = [&](const editor::ActorRow& r, int i) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				const std::string label = r.name + "##" + std::to_string(i);
				const bool isSelected = selected.rfind(r.name + " (", 0) == 0;
				if (ImGui::Selectable(label.c_str(), isSelected, ImGuiSelectableFlags_SpanAllColumns)) pendingSelect_ = r.actor;
				ImGui::TableNextColumn();
				ImGui::TextDisabled("%s", r.cls.c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%.1f", r.distance / 100.f);
			};
			if (!ImGui::BeginTable("##actors", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) return;
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Actor", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("Class", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("m", ImGuiTableColumnFlags_WidthFixed, 50.f);
			ImGui::TableHeadersRow();
			int shown = 0;
			if (byClass_) {
				std::map<std::string, std::vector<int>> groups;
				for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
					if (matches(rows_[static_cast<std::size_t>(i)])) groups[rows_[static_cast<std::size_t>(i)].cls].push_back(i);
				for (const auto& [cls, members] : groups) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					const std::string head = cls + " (" + std::to_string(members.size()) + ")";
					const bool open = ImGui::TreeNodeEx(head.c_str(), ImGuiTreeNodeFlags_SpanAllColumns);
					if (!open) continue;
					for (int i : members)
						if (++shown <= 2000) row(rows_[static_cast<std::size_t>(i)], i);
					ImGui::TreePop();
				}
			} else {
				for (int i = 0; i < static_cast<int>(rows_.size()); ++i)
					if (matches(rows_[static_cast<std::size_t>(i)]) && ++shown <= 2000) row(rows_[static_cast<std::size_t>(i)], i);
			}
			ImGui::EndTable();
		}

		std::mutex mutex_;
		std::vector<editor::ActorRow> rows_;
		Classes::UObject* pendingSelect_ = nullptr;
		bool pendingPick_ = false;
		char filter_[96] = "";
		bool byClass_ = false;
		std::atomic<bool> shown_{false};
		std::chrono::steady_clock::time_point lastRead_{};
	};
	MEM_MODULE(WorldExplorer)
}
