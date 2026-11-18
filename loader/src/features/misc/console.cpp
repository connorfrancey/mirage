// The console (Mirage step 27's REPL): a line of Lua run in a loaded Lua mod's own state (runtime/mods.h, Eval), so
// its globals, its modules and the whole `mirage` table are there to try things on the running game: "1 + 2",
// "mirage.pawn().Location", "x = mirage.find('Class Engine.Actor')", "print(mirage.movement().speed)". A line that is an
// expression shows its value; what it prints is shown with it; an error shows Lua's message. Lines run on the game
// thread; the window draws on the render thread and shows each result when it comes back. Up and Down walk the
// history.

#include "core/module.h"
#include "game/game.h"
#include "gui.h"
#include "runtime/mods.h"
#include "theme.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace
{
	struct Line
	{
		std::string text;
		int kind = 0;			// 0 input, 1 value, 2 printed, 3 error
	};

	class Console final : public Module
	{
	public:
		Console() : Module("Console", "Misc", "Runs Lua in a mod's own state on the running game (a REPL)", 2) {}

		void OnRender() override
		{
			if (!gui::visible) return;
			theme::Begin();
			ImGui::SetNextWindowPos({340.f, 160.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({640.f, 380.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({360.f, 200.f}, {FLT_MAX, FLT_MAX});
			bool open = true;
			if (ImGui::Begin("Console", &open, ImGuiWindowFlags_NoCollapse)) Draw();
			ImGui::End();
			theme::End();
			if (!open) SetEnabled(false);
		}

	private:
		void Draw()
		{
			const auto mods = mirage::mods::LuaMods();
			if (std::find(mods.begin(), mods.end(), mod_) == mods.end()) mod_ = mods.empty() ? "" : mods.front();
			ImGui::SetNextItemWidth(220.f);
			if (ImGui::BeginCombo("Mod", mod_.empty() ? "(no Lua mod loaded)" : mod_.c_str())) {
				for (const auto& id : mods)
					if (ImGui::Selectable(id.c_str(), id == mod_)) mod_ = id;
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			if (ImGui::SmallButton("Clear")) {
				std::lock_guard lock(state_->lock);
				state_->lines.clear();
			}
			const float input = ImGui::GetFrameHeightWithSpacing();
			ImGui::BeginChild("##out", {0.f, -input}, ImGuiChildFlags_Borders);
			{
				std::lock_guard lock(state_->lock);
				static const ImVec4 kColors[] = {{0.6f, 0.75f, 0.9f, 1.f}, {0.9f, 0.9f, 0.9f, 1.f}, {0.7f, 0.7f, 0.7f, 1.f}, {0.95f, 0.45f, 0.4f, 1.f}};
				for (const Line& l : state_->lines) {
					ImGui::PushStyleColor(ImGuiCol_Text, kColors[l.kind]);
					ImGui::TextWrapped("%s%s", l.kind == 0 ? "> " : "", l.text.c_str());
					ImGui::PopStyleColor();
				}
				if (state_->added) {
					ImGui::SetScrollHereY(1.f);
					state_->added = false;
				}
			}
			ImGui::EndChild();
			ImGui::SetNextItemWidth(-FLT_MIN);
			const bool run = ImGui::InputText("##line", buffer_, sizeof buffer_,
											  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory, &History, this);
			if (run && buffer_[0] && !mod_.empty()) {
				Submit(mod_, buffer_);
				history_.push_back(buffer_);
				historyAt_ = -1;
				buffer_[0] = '\0';
			}
			if (run) ImGui::SetKeyboardFocusHere(-1);
		}

		// The line runs on the game thread; its result comes back into the scrollback
		void Submit(const std::string& mod, const std::string& code)
		{
			{
				std::lock_guard lock(state_->lock);
				Add(*state_, code, 0);
			}
			game::RunOnGameThread([state = state_, mod, code] {
				const auto r = mirage::mods::Eval(mod, code);
				std::lock_guard lock(state->lock);
				std::size_t from = 0;
				while (from < r.printed.size()) {
					const std::size_t end = r.printed.find('\n', from);
					Add(*state, r.printed.substr(from, end - from), 2);
					from = end == std::string::npos ? r.printed.size() : end + 1;
				}
				if (!r.ok) Add(*state, r.error, 3);
				else if (!r.value.empty()) Add(*state, r.value, 1);
			});
		}

		static int History(ImGuiInputTextCallbackData* data)
		{
			auto* self = static_cast<Console*>(data->UserData);
			if (self->history_.empty()) return 0;
			const int n = static_cast<int>(self->history_.size());
			int& at = self->historyAt_;
			if (data->EventKey == ImGuiKey_UpArrow) at = at < 0 ? n - 1 : std::max(0, at - 1);
			else if (data->EventKey == ImGuiKey_DownArrow) at = at < 0 || at >= n - 1 ? -1 : at + 1;
			data->DeleteChars(0, data->BufTextLen);
			if (at >= 0) data->InsertChars(0, self->history_[static_cast<std::size_t>(at)].c_str());
			return 0;
		}

		struct State
		{
			std::mutex lock;
			std::deque<Line> lines;
			bool added = false;
		};
		static void Add(State& s, std::string text, int kind)
		{
			s.lines.push_back({std::move(text), kind});
			while (s.lines.size() > 1000) s.lines.pop_front();
			s.added = true;
		}

		std::shared_ptr<State> state_ = std::make_shared<State>();	// Shared with lines still running when the menu closes
		std::string mod_;
		char buffer_[1024]{};
		std::vector<std::string> history_;
		int historyAt_ = -1;
	};
	MEM_MODULE(Console)
}
