// Time Trials (Mirage, step 13): the mods' time-trial routes (runtime/routes.h). The window lists them by map with
// their best times and starts one (loading its map when needed); while a route is on, a HUD shows its clock,
// checkpoints and best, the next checkpoint and the finish, and the ghost of the best run.

#include "core/module.h"
#include "game/game.h"
#include "gui.h"
#include "render/math.h"
#include "runtime/routes.h"
#include "theme.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
	namespace routes = mirage::routes;

	std::string Clock(double t)
	{
		if (t < 0.0) return "-";
		char buf[32];
		const int m = static_cast<int>(t / 60.0);
		if (m > 0) std::snprintf(buf, sizeof buf, "%d:%06.3f", m, t - m * 60.0);
		else std::snprintf(buf, sizeof buf, "%.3f", t);
		return buf;
	}

	class TimeTrials final : public Module
	{
	public:
		TimeTrials() : Module("Time Trials", "Levels", "The mods' time-trial routes: start one, race the clock and your best run's ghost", 1) {}

		void OnEnable() override { game::SetCameraTracking(true); }

		void OnRender() override
		{
			const routes::State st = routes::Current();
			if (st.phase != routes::Phase::Idle) DrawHud(st);
			if (!gui::visible) return;
			theme::Begin();
			ImGui::SetNextWindowPos({320.f, 140.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({520.f, 360.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({360.f, 200.f}, {FLT_MAX, FLT_MAX});
			if (ImGui::Begin("Time Trials", nullptr, ImGuiWindowFlags_NoCollapse)) DrawList(st);
			ImGui::End();
		}

	private:
		void DrawList(const routes::State& st)
		{
			bool ghost = routes::GhostOn();
			if (ImGui::Checkbox("Race the ghost of my best run", &ghost)) routes::SetGhost(ghost);
			if (st.phase != routes::Phase::Idle) {
				ImGui::SameLine();
				if (ImGui::Button("Stop")) routes::Stop();
			}
			ImGui::Separator();
			const auto list = routes::List();
			if (list.empty()) {
				ImGui::TextDisabled("No routes: a mod's routes/<map>.toml adds them");
				return;
			}
			std::map<std::string, std::vector<const routes::Route*>> byMap;
			for (const auto& r : list) byMap[r.map].push_back(&r);
			const std::string here = [] {
				std::string m = game::MapName();
				for (char& c : m) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
				return m;
			}();
			for (const auto& [map, rs] : byMap) {
				ImGui::PushID(map.c_str());
				if (ImGui::CollapsingHeader((map + (map == here ? "  (this level)" : "")).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
					for (const routes::Route* r : rs) {
						ImGui::PushID(r->Id().c_str());
						const bool current = st.phase != routes::Phase::Idle && st.route.Id() == r->Id();
						if (ImGui::Button(current ? "Again" : "Start")) routes::Start(r->Id());
						ImGui::SameLine();
						ImGui::Text("%s", r->name.c_str());
						ImGui::SameLine();
						ImGui::TextDisabled("%zu checkpoint(s), %s", r->checkpoints.size(), r->mod.c_str());
						if (current && st.best >= 0.0) {
							ImGui::SameLine();
							ImGui::Text("best %s", Clock(st.best).c_str());
							ImGui::SameLine();
							if (ImGui::SmallButton("Forget best")) routes::Forget(r->Id());
						}
						ImGui::PopID();
					}
				}
				ImGui::PopID();
			}
		}

		// A world point on screen, from the camera the game reports
		static bool Project(const routes::Point& p, ImVec2& out)
		{
			const rmath::Camera c = game::GetCamera();
			const ImVec2 size = ImGui::GetIO().DisplaySize;
			if (!c.valid || size.x <= 0.f || size.y <= 0.f) return false;
			const rmath::Mat4 vp = rmath::Mul(rmath::ViewMatrix(c), rmath::ProjectionMatrix(c.fov, size.x / size.y, 10.f));
			return rmath::Project(vp, {p.x, p.y, p.z}, size.x, size.y, out.x, out.y);
		}

		void Marker(ImDrawList* dl, const routes::Point& p, ImU32 color, const char* label)
		{
			ImVec2 at;
			if (!Project(p, at)) return;
			dl->AddCircle(at, 12.f, color, 0, 3.f);
			dl->AddText({at.x + 15.f, at.y - 8.f}, color, label);
		}

		void DrawHud(const routes::State& st)
		{
			ImDrawList* dl = ImGui::GetBackgroundDrawList();
			const ImVec2 size = ImGui::GetIO().DisplaySize;
			const auto& r = st.route;
			std::string line = r.name + "   ";
			switch (st.phase) {
			case routes::Phase::Loading: line += "loading..."; break;
			case routes::Phase::Ready: line += "go when ready"; break;
			case routes::Phase::Running:
				line += Clock(st.time) + "   checkpoint " + std::to_string(std::min(st.splits.size(), r.checkpoints.size())) + "/" +
						std::to_string(r.checkpoints.size());
				break;
			case routes::Phase::Finished:
				line += "finished " + Clock(st.time) + (st.newBest ? "  (best!)" : "") +
						(r.ratings.empty() ? "" : "  " + std::string(static_cast<std::size_t>(st.stars), '*'));
				break;
			default: break;
			}
			if (st.best >= 0.0) line += "   best " + Clock(st.best);
			const ImVec2 text = ImGui::CalcTextSize(line.c_str());
			const ImVec2 at{(size.x - text.x) * 0.5f, 40.f};
			dl->AddRectFilled({at.x - 10.f, at.y - 6.f}, {at.x + text.x + 10.f, at.y + text.y + 6.f}, IM_COL32(0, 0, 0, 150), 6.f);
			dl->AddText(at, IM_COL32(255, 255, 255, 255), line.c_str());

			// Where to go: the start, the next checkpoint or the finish; the ghost
			if (st.phase == routes::Phase::Ready) Marker(dl, r.start, IM_COL32(80, 220, 120, 255), "start");
			if (st.phase == routes::Phase::Running) {
				const std::size_t next = st.splits.size();
				if (next < r.checkpoints.size()) Marker(dl, r.checkpoints[next], IM_COL32(255, 200, 60, 255), "checkpoint");
				else Marker(dl, r.finish, IM_COL32(255, 90, 90, 255), "finish");
				if (st.ghost && routes::GhostOn()) Marker(dl, st.ghostAt, IM_COL32(140, 180, 255, 200), "ghost");
			}
		}
	};
	MEM_MODULE(TimeTrials)
}
