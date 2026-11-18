// The Kismet node editor (Mirage step 27; see kismet_editor.h). The model (the open sequence, the edits) lives on the
// game thread; the window draws a copy of the graph and queues every edit to the game thread.

#include "features/misc/kismet_editor.h"

#include "core/module.h"
#include "game/game.h"
#include "gui.h"
#include "runtime/mods.h"
#include "sdk/SDK.hpp"
#include "theme.h"

#include <fmt/format.h>
#include <imgui.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace ki = mirage::kismet;

namespace
{
	// ---- The model (game thread) -------------------------------------------------------------------------------------
	struct Edit
	{
		enum class Kind { Link, Unlink, Op } kind = Kind::Link;
		Classes::USequenceOp* from = nullptr;
		Classes::USequenceOp* to = nullptr;		// Kind::Op: the op made
		int output = 0, input = 0, at = -1;		// at: where an unlinked link was in its output's list
		std::string cls;						// Kind::Op: the class as the layer names it ("Engine.SeqAct_Log")
		int x = 0, y = 0;
	};

	Classes::USequence* g_seq = nullptr;
	int g_slot = -1;							// Its GObjects slot: alive while the slot still holds it
	std::vector<Edit> g_edits;

	std::mutex g_lock;							// The copies the window draws
	ki::Sequence g_graph;
	std::vector<ki::Sequence> g_list;			// Every sequence, names only
	std::atomic<unsigned long long> g_frames{0};
	std::atomic<std::size_t> g_count{0};		// Edits, for the window

	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	}
	std::string Tail(const std::string& path) { return path.substr(path.rfind('.') + 1); }

	bool Alive()
	{
		const auto& objects = Classes::UObject::GetGlobalObjects();
		return g_seq && g_slot >= 0 && static_cast<std::size_t>(g_slot) < objects.Num() && objects.GetByIndex(g_slot) == g_seq;
	}
	void Refresh()
	{
		ki::Sequence graph;
		if (Alive()) graph = ki::ReadSequence(g_seq);
		std::lock_guard lock(g_lock);
		g_graph = std::move(graph);
		g_count = g_edits.size();
	}
	// An op of the open sequence by its path (objects are named uniquely inside their sequence)
	Classes::USequenceOp* OpOf(const std::string& path)
	{
		if (!Alive()) return nullptr;
		const std::string name = Tail(path);
		for (std::size_t i = 0; i < g_seq->SequenceObjects.Num(); ++i) {
			auto* o = g_seq->SequenceObjects[i];
			if (o && o->IsA(Classes::USequenceOp::StaticClass()) && o->GetName() == name) return static_cast<Classes::USequenceOp*>(o);
		}
		return nullptr;
	}
	Classes::UClass* ClassNamed(const std::string& name, std::string& full)
	{
		if (name.find('.') != std::string::npos) {
			full = name;
			return Classes::UObject::FindClass("Class " + name);
		}
		for (const char* package : {"Engine.", "GameFramework.", "TdGame."})
			if (auto* c = Classes::UObject::FindClass(std::string("Class ") + package + name)) {
				full = package + name;
				return c;
			}
		return nullptr;
	}
	// How the layer file names an op: its name, or "<its sequence>.<name>" when the name repeats in its level (a layer
	// resolves a name in the map's own level first, so only repeats within the same level are ambiguous)
	std::string LevelRef(Classes::USequenceOp* op)
	{
		const std::string name = op->GetName(), level = ki::LevelOf(op);
		int same = 0;
		for (auto* o : ki::LevelObjects()) same += o->GetName() == name && ki::LevelOf(o) == level ? 1 : 0;
		if (same <= 1 || !op->ParentSequence) return name;
		return op->ParentSequence->GetName() + "." + name;
	}
	std::string LayerName(const Classes::USequenceOp* made) { return "Ed_" + made->GetName(); }
	bool Made(const Classes::USequenceOp* op)
	{
		return std::any_of(g_edits.begin(), g_edits.end(), [&](const Edit& e) { return e.kind == Edit::Kind::Op && e.to == op; });
	}
	std::string RefOf(Classes::USequenceOp* op) { return Made(op) ? LayerName(op) : LevelRef(op); }

	void Undo(const Edit& e)
	{
		std::string why;
		switch (e.kind) {
		case Edit::Kind::Link: ki::RemoveLink(e.from, e.output, e.to, e.input); break;
		case Edit::Kind::Unlink: ki::AddLink(e.from, e.output, e.to, e.input, e.at, &why); break;
		case Edit::Kind::Op: if (Alive()) ki::RemoveOp(g_seq, e.to); break;
		}
	}
	void UndoAll()
	{
		if (Alive())
			for (auto e = g_edits.rbegin(); e != g_edits.rend(); ++e) Undo(*e);
		g_edits.clear();
	}

	// ---- The window (render thread) ----------------------------------------------------------------------------------
	constexpr float kNodeWidth = 170.f, kHeader = 20.f, kRow = 16.f, kPin = 5.f;

	class KismetEditor final : public Module
	{
	public:
		KismetEditor() : Module("Kismet Editor", "Misc", "The level's Kismet as a node graph: link, unlink and add ops live, save them as a mod's Kismet layer", 3) {}

		void OnDisable() override { game::RunOnGameThread([] { UndoAll(); Refresh(); }); }
		void OnTick() override
		{
			if (++ticks_ % 15 == 0 && gui::visible) Refresh();		// About four times a second while it can be seen
		}
		void OnRender() override
		{
			if (!gui::visible) return;
			++g_frames;
			theme::Begin();
			ImGui::SetNextWindowPos({260.f, 90.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSize({1000.f, 620.f}, ImGuiCond_FirstUseEver);
			ImGui::SetNextWindowSizeConstraints({560.f, 320.f}, {FLT_MAX, FLT_MAX});
			bool open = true;
			if (ImGui::Begin("Kismet Editor", &open, ImGuiWindowFlags_NoCollapse)) Draw();
			ImGui::End();
			theme::End();
			if (!open) SetEnabled(false);
		}

	private:
		void Draw()
		{
			if (!listed_) {
				listed_ = true;
				game::RunOnGameThread([] {
					std::vector<ki::Sequence> list;
					for (auto* s : ki::Sequences()) list.push_back(ki::ReadSequence(s, false));
					std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
					std::lock_guard lock(g_lock);
					g_list = std::move(list);
				});
			}
			ki::Sequence graph;
			std::vector<ki::Sequence> list;
			{
				std::lock_guard lock(g_lock);
				graph = g_graph;
				list = g_list;
			}
			DrawToolbar(graph);
			ImGui::BeginChild("##sequences", {240.f, 0.f}, ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
			if (ImGui::SmallButton("Refresh")) listed_ = false;
			for (const auto& s : list) {
				const int depth = static_cast<int>(std::count(s.path.begin(), s.path.end(), '.'));
				ImGui::Indent(static_cast<float>(std::max(0, depth - 3)) * 8.f);
				if (ImGui::Selectable((s.name + "##" + s.path).c_str(), s.path == graph.path))
					game::RunOnGameThread([path = s.path] { kismet_editor::Open(path); });
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s (%s)", s.path.c_str(), s.level.c_str());
				ImGui::Unindent(static_cast<float>(std::max(0, depth - 3)) * 8.f);
			}
			ImGui::EndChild();
			ImGui::SameLine();
			ImGui::BeginChild("##canvas", {0.f, 0.f}, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			if (graph.path.empty()) ImGui::TextDisabled("Pick a sequence on the left");
			else DrawCanvas(graph);
			ImGui::EndChild();
		}

		void DrawToolbar(const ki::Sequence& graph)
		{
			ImGui::SetNextItemWidth(220.f);
			ImGui::InputTextWithHint("##class", "Engine.SeqAct_Log", class_, sizeof class_);
			ImGui::SameLine();
			if (ImGui::Button("Add op") && !graph.path.empty() && class_[0]) {
				const int x = static_cast<int>(-pan_.x / zoom_ + 200.f), y = static_cast<int>(-pan_.y / zoom_ + 150.f);
				game::RunOnGameThread([cls = std::string(class_), x, y, this] {
					std::string why;
					if (kismet_editor::Add(cls, x, y, &why).empty()) SetStatus("Add: " + why);
				});
			}
			ImGui::SameLine();
			const auto mods = mirage::mods::List();
			ImGui::SetNextItemWidth(160.f);
			if (ImGui::BeginCombo("##mod", mod_.empty() ? "Save into mod..." : mod_.c_str())) {
				for (const auto& m : mods)
					if (m.loaded && ImGui::Selectable(m.id.c_str(), m.id == mod_)) mod_ = m.id;
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			const int edits = static_cast<int>(g_count.load());
			ImGui::BeginDisabled(mod_.empty() || edits == 0);
			if (ImGui::Button("Save as layer"))
				game::RunOnGameThread([mod = mod_, this] {
					std::string why;
					SetStatus(kismet_editor::Save(mod, &why) ? "Saved into " + mod + "'s Kismet layer" : "Save: " + why);
				});
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::BeginDisabled(edits == 0);
			if (ImGui::Button("Discard")) game::RunOnGameThread([] { kismet_editor::Discard(); });
			ImGui::EndDisabled();
			ImGui::SameLine();
			ImGui::TextDisabled("%d edit%s", edits, edits == 1 ? "" : "s");
			{
				std::lock_guard lock(g_lock);
				if (!status_.empty()) {
					ImGui::SameLine();
					ImGui::TextUnformatted(status_.c_str());
				}
			}
		}

		void SetStatus(std::string text)
		{
			std::lock_guard lock(g_lock);
			status_ = std::move(text);
		}

		// Where an op is drawn (sequence coordinates): where the designers put it, moved by the user here (not saved),
		// or on a grid when cooking dropped the positions
		ImVec2 At(const ki::Op& op, int index, bool grid) const
		{
			ImVec2 p = grid ? ImVec2{static_cast<float>(index % 8) * 220.f, static_cast<float>(index / 8) * 140.f}
							: ImVec2{static_cast<float>(op.x), static_cast<float>(op.y)};
			if (const auto it = moved_.find(op.path); it != moved_.end()) p = {p.x + it->second.x, p.y + it->second.y};
			return p;
		}

		void DrawCanvas(const ki::Sequence& graph)
		{
			ImDrawList* dl = ImGui::GetWindowDrawList();
			const ImVec2 origin = ImGui::GetCursorScreenPos();
			const ImVec2 size = ImGui::GetContentRegionAvail();
			ImGui::InvisibleButton("##area", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
			const bool hovered = ImGui::IsItemHovered();
			const ImGuiIO& io = ImGui::GetIO();
			if (graph.path != shown_) {			// Another sequence: fit its ops in view
				shown_ = graph.path;
				moved_.clear();
				Fit(graph, size);
			}
			// Pan with the right (or middle) button, zoom with the wheel around the mouse
			if (hovered && (ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle))) {
				pan_.x += io.MouseDelta.x;
				pan_.y += io.MouseDelta.y;
			}
			if (hovered && io.MouseWheel != 0.f) {
				const float before = zoom_;
				zoom_ = std::clamp(zoom_ * (io.MouseWheel > 0 ? 1.15f : 1.f / 1.15f), 0.2f, 2.5f);
				const ImVec2 m{io.MousePos.x - origin.x, io.MousePos.y - origin.y};
				pan_.x = m.x - (m.x - pan_.x) * zoom_ / before;
				pan_.y = m.y - (m.y - pan_.y) * zoom_ / before;
			}
			const auto screen = [&](ImVec2 p) { return ImVec2{origin.x + pan_.x + p.x * zoom_, origin.y + pan_.y + p.y * zoom_}; };

			const bool grid = std::all_of(graph.ops.begin(), graph.ops.end(), [](const ki::Op& o) { return o.x == 0 && o.y == 0; });
			std::map<std::string, std::pair<const ki::Op*, ImVec2>> where;		// Path -> op, its top-left on screen
			for (std::size_t i = 0; i < graph.ops.size(); ++i) where[graph.ops[i].path] = {&graph.ops[i], screen(At(graph.ops[i], static_cast<int>(i), grid))};
			const auto inPin = [&](const ImVec2& tl, int i) { return ImVec2{tl.x, tl.y + (kHeader + kRow * (i + 0.5f)) * zoom_}; };
			const auto outPin = [&](const ImVec2& tl, int i) { return ImVec2{tl.x + kNodeWidth * zoom_, tl.y + (kHeader + kRow * (i + 0.5f)) * zoom_}; };

			dl->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
			// Wires: output links, then variable links (faint)
			struct Wire { std::string from, to; int out, in; ImVec2 a, b; };
			std::vector<Wire> wires;
			for (const auto& [path, entry] : where) {
				const ki::Op& op = *entry.first;
				for (std::size_t o = 0; o < op.outputs.size(); ++o)
					for (const ki::Link& l : op.outputs[o].links)
						if (const auto t = where.find(l.op); t != where.end())
							wires.push_back({path, l.op, static_cast<int>(o), l.input, outPin(entry.second, static_cast<int>(o)), inPin(t->second.second, l.input)});
				for (std::size_t v = 0; v < op.variables.size(); ++v)
					for (const auto& var : op.variables[v].variables)
						if (const auto t = where.find(var); t != where.end()) {
							const float rows = static_cast<float>(std::max(op.inputs.size(), op.outputs.size()));
							const ImVec2 a{entry.second.x + (12.f + 40.f * v) * zoom_, entry.second.y + (kHeader + kRow * rows + 8.f) * zoom_};
							dl->AddLine(a, {t->second.second.x + 30.f * zoom_, t->second.second.y}, IM_COL32(150, 120, 200, 90));
						}
			}
			int hoveredWire = -1;
			for (std::size_t w = 0; w < wires.size(); ++w) {
				const Wire& wr = wires[w];
				const float d = std::max(40.f * zoom_, std::fabs(wr.b.x - wr.a.x) * 0.5f);
				const ImVec2 c1{wr.a.x + d, wr.a.y}, c2{wr.b.x - d, wr.b.y};
				bool near = false;
				for (int k = 0; k <= 16 && hovered; ++k) {	// Near the curve: sampled
					const float t = k / 16.f, u = 1.f - t;
					const ImVec2 p{u * u * u * wr.a.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * wr.b.x,
								   u * u * u * wr.a.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * wr.b.y};
					near |= std::hypot(p.x - io.MousePos.x, p.y - io.MousePos.y) < 6.f;
				}
				if (near && hoveredWire < 0) hoveredWire = static_cast<int>(w);
				dl->AddBezierCubic(wr.a, c1, c2, wr.b, near ? IM_COL32(255, 200, 90, 255) : IM_COL32(200, 200, 210, 200), near ? 3.f : 1.8f);
			}
			// Nodes
			std::string overIn, overOut;
			int overInIndex = -1, overOutIndex = -1;
			for (const auto& [path, entry] : where) {
				const ki::Op& op = *entry.first;
				const ImVec2 tl = entry.second;
				if (op.kind == ki::Op::Kind::Variable) {
					const ImVec2 br{tl.x + 60.f * zoom_, tl.y + 24.f * zoom_};
					dl->AddRectFilled(tl, br, IM_COL32(70, 55, 95, 230), 10.f * zoom_);
					if (zoom_ > 0.45f) dl->AddText({tl.x + 6.f * zoom_, tl.y + 5.f * zoom_}, IM_COL32(220, 210, 240, 255), Short(op.cls).c_str());
					continue;
				}
				const float rows = static_cast<float>(std::max<std::size_t>({op.inputs.size(), op.outputs.size(), 1}));
				const ImVec2 br{tl.x + kNodeWidth * zoom_, tl.y + (kHeader + kRow * rows + (op.variables.empty() ? 4.f : 18.f)) * zoom_};
				const ImU32 head = op.kind == ki::Op::Kind::Event ? IM_COL32(150, 60, 50, 255)
								 : op.kind == ki::Op::Kind::Condition ? IM_COL32(60, 110, 60, 255)
								 : op.kind == ki::Op::Kind::Sequence ? IM_COL32(110, 100, 40, 255) : IM_COL32(50, 80, 140, 255);
				dl->AddRectFilled(tl, br, IM_COL32(35, 37, 44, 235), 4.f * zoom_);
				dl->AddRectFilled(tl, {br.x, tl.y + kHeader * zoom_}, head, 4.f * zoom_, ImDrawFlags_RoundCornersTop);
				dl->AddRect(tl, br, op.active ? IM_COL32(255, 220, 80, 255) : IM_COL32(90, 90, 100, 255), 4.f * zoom_, 0, op.active ? 2.5f : 1.f);
				if (zoom_ > 0.45f) {
					dl->AddText({tl.x + 5.f * zoom_, tl.y + 3.f * zoom_}, IM_COL32_WHITE, (op.title.empty() ? Short(op.cls) : op.title).c_str());
					for (std::size_t i = 0; i < op.inputs.size(); ++i)
						dl->AddText({tl.x + 9.f * zoom_, tl.y + (kHeader + kRow * i) * zoom_}, IM_COL32(200, 200, 200, 255), op.inputs[i].c_str());
					for (std::size_t o = 0; o < op.outputs.size(); ++o) {
						const ImVec2 t = ImGui::CalcTextSize(op.outputs[o].desc.c_str());
						dl->AddText({br.x - t.x - 9.f * zoom_, tl.y + (kHeader + kRow * o) * zoom_}, IM_COL32(200, 200, 200, 255), op.outputs[o].desc.c_str());
					}
					for (std::size_t v = 0; v < op.variables.size(); ++v)
						dl->AddText({tl.x + (6.f + 40.f * v) * zoom_, br.y - 15.f * zoom_}, IM_COL32(170, 150, 210, 255), op.variables[v].desc.substr(0, 6).c_str());
				}
				for (std::size_t i = 0; i < op.inputs.size(); ++i) {
					const ImVec2 p = inPin(tl, static_cast<int>(i));
					const bool over = std::hypot(p.x - io.MousePos.x, p.y - io.MousePos.y) < 8.f;
					if (over) { overIn = path; overInIndex = static_cast<int>(i); }
					dl->AddCircleFilled(p, kPin * zoom_ + (over ? 2.f : 0.f), IM_COL32(120, 200, 120, 255));
				}
				for (std::size_t o = 0; o < op.outputs.size(); ++o) {
					const ImVec2 p = outPin(tl, static_cast<int>(o));
					const bool over = std::hypot(p.x - io.MousePos.x, p.y - io.MousePos.y) < 8.f;
					if (over) { overOut = path; overOutIndex = static_cast<int>(o); }
					dl->AddCircleFilled(p, kPin * zoom_ + (over ? 2.f : 0.f), IM_COL32(220, 160, 90, 255));
				}
				// Dragging a node by its header (where it is drawn here only)
				const bool onHeader = io.MousePos.x >= tl.x && io.MousePos.x <= br.x && io.MousePos.y >= tl.y && io.MousePos.y <= tl.y + kHeader * zoom_;
				if (hovered && onHeader && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && overOut.empty()) dragNode_ = path;
				if (onHeader && hovered) ImGui::SetTooltip("%s\n%s%s", op.path.c_str(), op.cls.c_str(), op.comment.empty() ? "" : ("\n" + op.comment).c_str());
			}
			// Linking: press on an output pin, release on an input pin
			if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !overOut.empty()) {
				linkFrom_ = overOut;
				linkOut_ = overOutIndex;
				dragNode_.clear();
			}
			if (!linkFrom_.empty()) {
				if (const auto f = where.find(linkFrom_); f != where.end())
					dl->AddLine(outPin(f->second.second, linkOut_), io.MousePos, IM_COL32(255, 220, 120, 255), 2.f);
				if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
					if (!overIn.empty())
						game::RunOnGameThread([from = linkFrom_, out = linkOut_, to = overIn, in = overInIndex, this] {
							std::string why;
							if (!kismet_editor::Link(from, out, to, in, &why)) SetStatus("Link: " + why);
						});
					linkFrom_.clear();
				}
			}
			if (!dragNode_.empty()) {
				if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
					ImVec2& m = moved_[dragNode_];
					m.x += io.MouseDelta.x / zoom_;
					m.y += io.MouseDelta.y / zoom_;
				} else dragNode_.clear();
			}
			// Unlinking: a right click (not a pan) on a wire
			if (hovered && hoveredWire >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
				std::fabs(ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.f).x) + std::fabs(ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.f).y) < 4.f) {
				const Wire w = wires[static_cast<std::size_t>(hoveredWire)];
				game::RunOnGameThread([w, this] {
					std::string why;
					if (!kismet_editor::Unlink(w.from, w.out, w.to, w.in, &why)) SetStatus("Unlink: " + why);
				});
			}
			dl->PopClipRect();
		}

		void Fit(const ki::Sequence& graph, ImVec2 size)
		{
			float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
			for (const auto& op : graph.ops) {
				minX = std::min(minX, static_cast<float>(op.x));
				minY = std::min(minY, static_cast<float>(op.y));
				maxX = std::max(maxX, static_cast<float>(op.x) + kNodeWidth);
				maxY = std::max(maxY, static_cast<float>(op.y) + 80.f);
			}
			if (graph.ops.empty() || maxX - minX < 1.f) {
				zoom_ = 1.f;
				pan_ = {20.f, 20.f};
				return;
			}
			zoom_ = std::clamp(std::min(size.x / (maxX - minX + 80.f), size.y / (maxY - minY + 80.f)), 0.2f, 1.f);
			pan_ = {-minX * zoom_ + 40.f, -minY * zoom_ + 40.f};
		}

		static std::string Short(const std::string& cls)
		{
			for (const char* p : {"SeqAct_", "SeqEvent_", "SeqCond_", "SeqVar_"})
				if (cls.rfind(p, 0) == 0) return cls.substr(std::strlen(p));
			return cls;
		}

		std::uint64_t ticks_ = 0;
		bool listed_ = false;
		std::string shown_, mod_, status_;
		char class_[128] = "Engine.SeqAct_Log";
		ImVec2 pan_{0.f, 0.f};
		float zoom_ = 1.f;
		std::map<std::string, ImVec2> moved_;
		std::string dragNode_, linkFrom_;
		int linkOut_ = 0;
	};
	MEM_MODULE(KismetEditor)
}

// ---- The editor's functions (game thread) ------------------------------------------------------------------------------
namespace kismet_editor
{
	bool Open(const std::string& path)
	{
		UndoAll();		// Edits belong to the sequence they were made in: another one starts clean
		g_seq = nullptr;
		g_slot = -1;
		for (auto* s : ki::Sequences())
			if (ki::ReadSequence(s, false).path == path) {
				g_seq = s;
				g_slot = s->ObjectInternalInteger;
				break;
			}
		Refresh();
		return g_seq != nullptr;
	}

	ki::Sequence Graph()
	{
		std::lock_guard lock(g_lock);
		return g_graph;
	}

	bool Link(const std::string& from, int output, const std::string& to, int input, std::string* error)
	{
		auto* a = OpOf(from);
		auto* b = OpOf(to);
		if (!a || !b) {
			if (error) *error = "both ends must be ops of the open sequence";
			return false;
		}
		const ki::LinkResult r = ki::AddLink(a, output, b, input, -1, error);
		if (r == ki::LinkResult::Failed) return false;
		if (r == ki::LinkResult::Added) {
			// Linking back what this editor unlinked cancels that edit
			const auto it = std::find_if(g_edits.begin(), g_edits.end(), [&](const Edit& e) {
				return e.kind == Edit::Kind::Unlink && e.from == a && e.to == b && e.output == output && e.input == input;
			});
			if (it != g_edits.end()) g_edits.erase(it);
			else g_edits.push_back({Edit::Kind::Link, a, b, output, input});
		}
		Refresh();
		return true;
	}

	bool Unlink(const std::string& from, int output, const std::string& to, int input, std::string* error)
	{
		auto* a = OpOf(from);
		auto* b = OpOf(to);
		if (!a || !b) {
			if (error) *error = "both ends must be ops of the open sequence";
			return false;
		}
		const int at = ki::RemoveLink(a, output, b, input);
		if (at < 0) {
			if (error) *error = "there is no such link";
			return false;
		}
		const auto it = std::find_if(g_edits.begin(), g_edits.end(), [&](const Edit& e) {
			return e.kind == Edit::Kind::Link && e.from == a && e.to == b && e.output == output && e.input == input;
		});
		if (it != g_edits.end()) g_edits.erase(it);		// Unlinking what this editor linked cancels that edit
		else g_edits.push_back({Edit::Kind::Unlink, a, b, output, input, at});
		Refresh();
		return true;
	}

	std::string Add(const std::string& cls, int x, int y, std::string* error)
	{
		if (!Alive()) {
			if (error) *error = "no sequence open";
			return "";
		}
		std::string full;
		Classes::UClass* c = ClassNamed(cls, full);
		if (!c) {
			if (error) *error = "no class " + cls;
			return "";
		}
		Classes::USequenceOp* op = ki::Construct(g_seq, c, error);
		if (!op) return "";
		op->ObjPosX = x;
		op->ObjPosY = y;
		Edit e;
		e.kind = Edit::Kind::Op;
		e.to = op;
		e.cls = full;
		e.x = x;
		e.y = y;
		g_edits.push_back(e);
		Refresh();
		const std::string seqPath = ki::ReadSequence(g_seq, false).path;
		return seqPath + "." + op->GetName();
	}

	std::vector<std::string> Edits()
	{
		std::vector<std::string> out;
		for (const Edit& e : g_edits) {
			if (e.kind == Edit::Kind::Op) out.push_back("op " + LayerName(e.to) + " (" + e.cls + ")");
			else out.push_back(fmt::format("{} {}[{}] -> {}[{}]", e.kind == Edit::Kind::Link ? "link" : "unlink", RefOf(e.from), e.output,
										   RefOf(e.to), e.input));
		}
		return out;
	}

	void Discard()
	{
		UndoAll();
		Refresh();
	}

	bool Save(const std::string& modId, std::string* error)
	{
		if (g_edits.empty() || !Alive()) {
			if (error) *error = "no edits to save";
			return false;
		}
		std::filesystem::path dir;
		for (const auto& m : mirage::mods::List())
			if (m.id == modId && m.loaded) dir = m.dir;
		if (dir.empty()) {
			if (error) *error = "no loaded mod " + modId;
			return false;
		}
		// The layer's text: ops, then unlinks, then links (the order the layer applies them in)
		std::ostringstream text;
		text << "\n# Saved by the Kismet Editor (" << ki::ReadSequence(g_seq, false).path << ")\n";
		const std::string sequence = g_seq->GetName();
		for (const Edit& e : g_edits)
			if (e.kind == Edit::Kind::Op)
				text << "[[op]]\nname = \"" << LayerName(e.to) << "\"\nclass = \"" << e.cls << "\"\nsequence = \"" << sequence
					 << "\"\n[op.properties]\nObjPosX = " << e.x << "\nObjPosY = " << e.y << "\n\n";
		for (const Edit& e : g_edits)
			if (e.kind != Edit::Kind::Op)
				text << (e.kind == Edit::Kind::Link ? "[[link]]" : "[[unlink]]") << "\nfrom = \"" << RefOf(e.from) << "\"\noutput = " << e.output
					 << "\nto = \"" << RefOf(e.to) << "\"\ninput = " << e.input << "\n\n";
		const std::filesystem::path file = dir / "kismet" / (Lower(game::MapName()) + ".toml");
		std::error_code ec;
		std::filesystem::create_directories(file.parent_path(), ec);
		{
			std::ofstream out(file, std::ios::binary | std::ios::app);
			if (!out) {
				if (error) *error = "can't write " + file.string();
				return false;
			}
			out << text.str();
		}
		spdlog::info("Kismet Editor: {} edit(s) saved into {}", g_edits.size(), file.string());
		UndoAll();		// The layer applies them from now on
		Refresh();
		return true;
	}

	unsigned long long Frames() { return g_frames.load(); }
}
