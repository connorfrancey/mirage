#include "crashhandler/window.h"

#include "font.h"
#include "gui.h"
#include "hooks.h"

#include <imgui.h>

#include <Windows.h>
#include <shellapi.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <sstream>

namespace crashhandler::window
{
	namespace
	{
		const ImVec4 kRed{1.f, 0.42f, 0.38f, 1.f}, kAmber{1.f, 0.78f, 0.35f, 1.f}, kDim{0.62f, 0.62f, 0.66f, 1.f},
			kMod{0.55f, 0.8f, 1.f, 1.f};

		// The live window (Run): one crash at a time
		std::atomic<int> g_choice{0};
		std::atomic<bool> g_showing{false};
		std::atomic<unsigned> g_frames{0};
		const Analysis* g_live = nullptr;
		Options g_options;
		std::string g_reportText;

		// The preview: its own copy, drawn by the Crash Reporter module
		std::mutex g_previewLock;
		Analysis g_preview;
		std::wstring g_previewReport;
		std::string g_previewText;
		std::atomic<bool> g_previewOpen{false};
		std::atomic<unsigned> g_previewFrames{0};

		std::string ReadText(const std::wstring& path)
		{
			if (path.empty()) return {};
			std::ifstream in(path, std::ios::binary);
			std::stringstream s;
			s << in.rdbuf();
			return s.str();
		}

		void Open(const std::wstring& path)
		{
			if (!path.empty()) ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}

		void Mono(bool on)
		{
			if (!font::Mono()) return;
			if (on) ImGui::PushFont(font::Mono());
			else ImGui::PopFont();
		}

		void Hex(std::uint32_t v)
		{
			ImGui::Text("%08X", v);
		}

		void SlotsTable(const char* id, const std::vector<Slot>& slots, bool onlyNoted)
		{
			Mono(true);
			if (ImGui::BeginTable(id, 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) {
				ImGui::TableSetupColumn("Where");
				ImGui::TableSetupColumn("Value");
				ImGui::TableSetupColumn("Looks like", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableHeadersRow();
				for (const Slot& s : slots) {
					if (onlyNoted && s.note.empty()) continue;
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(s.label.c_str());
					ImGui::TableNextColumn();
					Hex(s.value);
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(s.note.c_str());
				}
				ImGui::EndTable();
			}
			Mono(false);
		}

		void PlacesTable(const std::vector<Place>& frames)
		{
			Mono(true);
			if (ImGui::BeginTable("frames", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) {
				ImGui::TableSetupColumn("#");
				ImGui::TableSetupColumn("Where");
				ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("Source / mod");
				ImGui::TableSetupScrollFreeze(0, 1);
				ImGui::TableHeadersRow();
				for (std::size_t i = 0; i < frames.size(); ++i) {
					const Place& f = frames[i];
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::Text("%zu", i);
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(f.Short().c_str());
					ImGui::TableNextColumn();
					ImGui::TextUnformatted(f.symbol.empty() ? "?" : f.symbol.c_str());
					ImGui::TableNextColumn();
					if (!f.mod.empty()) ImGui::TextColored(kMod, "mod %s", f.mod.c_str());
					else ImGui::TextDisabled("%s", f.source.c_str());
				}
				ImGui::EndTable();
			}
			Mono(false);
		}

		// The window's contents. `options` null: the preview (no actions on the game)
		Action Body(const Analysis& a, const std::wstring& report, const std::string& reportText, const Options* options)
		{
			Action chosen = Action::None;
			ImGui::PushTextWrapPos(0.f);
			ImGui::TextColored(kRed, "%s", options ? "Mirror's Edge crashed" : ("Crash report: " + a.title).c_str());
			ImGui::Text("%s in %s", a.what.c_str(), a.whose.c_str());
			ImGui::TextColored(kDim, "at %s", a.where.Full().c_str());
			if (a.accessKnown) ImGui::TextColored(kDim, "%s 0x%08X, on %s", a.accessKind.c_str(), a.accessAddress, a.threadName.c_str());
			else ImGui::TextColored(kDim, "on %s", a.threadName.c_str());
			ImGui::PopTextWrapPos();

			ImGui::Spacing();
			if (ImGui::Button("Copy the report")) ImGui::SetClipboardText(reportText.empty() ? "(the report couldn't be read)" : reportText.c_str());
			ImGui::SameLine();
			if (ImGui::Button("Open the report")) Open(report);
			ImGui::SameLine();
			if (ImGui::Button("Open the folder")) {
				const std::size_t slash = report.find_last_of(L"\\/");
				Open(options ? options->folder : slash == std::wstring::npos ? std::wstring() : report.substr(0, slash));
			}
			if (options) {
				ImGui::SameLine();
				ImGui::TextDisabled("|");
				ImGui::SameLine();
				if (ImGui::Button("Restart the game")) chosen = Action::Restart;
				ImGui::SameLine();
				if (ImGui::Button("Close the game")) chosen = Action::Close;
				if (options->canContinue) {
					ImGui::SameLine();
					if (ImGui::Button("Continue")) chosen = Action::Continue;
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("Go on past it (a breakpoint is skipped); the game may crash again");
				}
				if (options->canEndThread) {
					ImGui::SameLine();
					if (ImGui::Button("End this thread")) chosen = Action::EndThread;
					if (ImGui::IsItemHovered()) ImGui::SetTooltip("The crashed thread stops; the game goes on without it (it may "
																  "miss what that thread did)");
				}
			}

			if (!a.findings.empty()) {
				ImGui::SeparatorText("Possible causes");
				ImGui::PushTextWrapPos(0.f);
				for (const Finding& f : a.findings) {
					ImGui::Bullet();
					ImGui::TextColored(kAmber, "%s", f.title.c_str());
					ImGui::Indent();
					ImGui::TextUnformatted(f.detail.c_str());
					ImGui::Unindent();
				}
				ImGui::PopTextWrapPos();
			}

			ImGui::Spacing();
			if (ImGui::BeginTabBar("crash")) {
				if (ImGui::BeginTabItem("Call stack")) {
					if (a.framesFromScan) ImGui::TextDisabled("From code addresses on the stack (no frame pointers): some may be stale");
					PlacesTable(a.frames);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("UnrealScript")) {
					if (a.script.empty()) ImGui::TextDisabled("No UnrealScript was running on this thread");
					Mono(true);
					for (const ScriptFrame& f : a.script) {
						ImGui::Text("%s +0x%X", f.function.c_str(), f.offset);
						ImGui::SameLine();
						ImGui::TextDisabled("on %s", f.object.c_str());
						if (!f.changedBy.empty()) {
							ImGui::SameLine();
							ImGui::TextColored(kMod, "[bytecode changed by %s]", f.changedBy.c_str());
						}
					}
					Mono(false);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Registers")) {
					SlotsTable("registers", a.registers, false);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Stack")) {
					static bool all = false;
					ImGui::Checkbox("Every slot (not only the ones that look like something)", &all);
					SlotsTable("stack", a.stack, !all);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Code")) {
					if (a.disasm.empty()) ImGui::TextDisabled("The code at the crash couldn't be read");
					Mono(true);
					for (const DisasmLine& l : a.disasm) {
						if (l.current) ImGui::PushStyleColor(ImGuiCol_Text, kRed);
						ImGui::Text("%s %08X  %-24s %s", l.current ? "=>" : "  ", l.address, l.bytes.c_str(), l.text.c_str());
						if (l.current) ImGui::PopStyleColor();
					}
					Mono(false);
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Mods")) {
					if (a.mods.empty()) ImGui::TextDisabled("No mods were loaded");
					for (const std::string& m : a.mods) ImGui::BulletText("%s", m.c_str());
					ImGui::EndTabItem();
				}
				if (ImGui::BeginTabItem("Log")) {
					Mono(true);
					ImGui::BeginChild("log", {0.f, 0.f}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
					for (const std::string& l : a.log) ImGui::TextUnformatted(l.c_str());
					if (ImGui::GetScrollMaxY() > 0.f && ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(1.f);
					ImGui::EndChild();
					Mono(false);
					ImGui::EndTabItem();
				}
				ImGui::EndTabBar();
			}
			return chosen;
		}

		void Center(float fraction)
		{
			const ImGuiViewport* vp = ImGui::GetMainViewport();
			ImGui::SetNextWindowPos({vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f},
									ImGuiCond_Appearing, {0.5f, 0.5f});
			ImGui::SetNextWindowSize({vp->WorkSize.x * fraction, vp->WorkSize.y * fraction}, ImGuiCond_Appearing);
		}

		// hooks::SetExclusiveDraw: the only thing drawn once the game has crashed
		void DrawLive()
		{
			if (!g_live) return;
			g_frames.fetch_add(1);
			Center(0.86f);
			ImGui::SetNextWindowFocus();
			if (ImGui::Begin("Mirage: the game crashed", nullptr, ImGuiWindowFlags_NoCollapse)) {
				const Action a = Body(*g_live, g_options.report, g_reportText, &g_options);
				if (a != Action::None) g_choice = static_cast<int>(a);
			}
			ImGui::End();
		}
	}

	Action Run(const Analysis& analysis, const Options& options, double noFrameSeconds)
	{
		if (!ImGui::GetCurrentContext()) return Action::None;
		g_live = &analysis;
		g_options = options;
		g_reportText = ReadText(options.report);
		g_choice = 0;
		g_frames = 0;
		const bool wasVisible = gui::visible;
		gui::visible = true;		// The window's input reaches ImGui (hooks.cpp passes it on while the menu is visible)
		hooks::SetExclusiveDraw(&DrawLive);
		g_showing = true;

		const auto hwnd = static_cast<HWND>(hooks::GameWindow());
		const bool owns = hwnd && GetWindowThreadProcessId(hwnd, nullptr) == GetCurrentThreadId();
		const ULONGLONG start = GetTickCount64();
		while (g_choice.load() == 0) {
			if (owns) {
				// This thread is the window's: nobody else pumps its messages now
				MSG m;
				while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
					if (m.message == WM_QUIT) g_choice = static_cast<int>(Action::Close);
					TranslateMessage(&m);
					DispatchMessageW(&m);
				}
			}
			hooks::DrawWhileStalled();		// When the renderer stands still (it waits for this thread): drawn from here
			if (g_frames.load() == 0 && GetTickCount64() - start > static_cast<ULONGLONG>(noFrameSeconds * 1000.0)) break;
			Sleep(10);
		}
		g_showing = false;
		hooks::SetExclusiveDraw(nullptr);
		gui::visible = wasVisible;
		const auto chosen = static_cast<Action>(g_choice.load());
		g_live = nullptr;
		return chosen;
	}

	bool Showing() { return g_showing.load(); }
	unsigned FramesDrawn() { return g_frames.load(); }
	void Choose(Action action) { g_choice = static_cast<int>(action); }

	void Preview(const Analysis& analysis, const std::wstring& report)
	{
		std::lock_guard lock(g_previewLock);
		g_preview = analysis;
		g_previewReport = report;
		g_previewText = ReadText(report);
		g_previewFrames = 0;
		g_previewOpen = true;
	}
	bool PreviewOpen() { return g_previewOpen.load(); }
	void ClosePreview() { g_previewOpen = false; }
	unsigned PreviewFrames() { return g_previewFrames.load(); }

	void DrawPreview()
	{
		if (!g_previewOpen.load()) return;
		std::lock_guard lock(g_previewLock);
		bool open = true;
		Center(0.7f);
		if (ImGui::Begin("Crash report", &open, ImGuiWindowFlags_NoCollapse)) {
			g_previewFrames.fetch_add(1);
			Body(g_preview, g_previewReport, g_previewText, nullptr);
		}
		ImGui::End();
		if (!open) g_previewOpen = false;
	}
}
