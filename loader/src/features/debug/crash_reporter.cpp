// The Crash Reporter module: the settings of the crash reporter and the hang detector (runtime/crash_reporter.h,
// runtime/hang.h), the reports they wrote, and "the game crashed last time" at the next start (like Geode).
#include "core/module.h"
#include "gui.h"
#include "crashhandler/window.h"
#include "runtime/crash_reporter.h"
#include "runtime/hang.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <Windows.h>
#include <shellapi.h>

#include <chrono>
#include <fstream>
#include <string>

namespace {

void OpenPath(const std::filesystem::path& p)
{
    ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// A report in Notepad (.miragecrashlog has no program of its own); an archived one (.zst) as a plain copy in %TEMP%
void OpenReport(const std::filesystem::path& p)
{
    std::filesystem::path file = p;
    if (p.extension() == ".zst") {
        file = std::filesystem::temp_directory_path() / p.stem();      // "crash-....miragecrashlog"
        std::ofstream(file, std::ios::binary | std::ios::trunc) << mirage::crash::ReadReport(p, 16 * 1024 * 1024);
    }
    const std::wstring args = L"\"" + file.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"notepad.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

class CrashReporter final : public Module {
public:
    CrashReporter() : Module("Crash Reporter", "Debug", "Handles game crashes with a report (and minidump), detects hangs", 7)
    {
        enabledByDefault = true;
        schedule.interval = 1.f;
        schedule.priority = sched::Priority::Background;
        const auto in = [](ctl::Section& section, std::initializer_list<ctl::Setting*> controls) {
            for (ctl::Setting* c : controls) c->VisibleIf([&section] { return section.Get(); });
        };
        in(crashSection, {&handle, &window, &openReport, &showLast, &openFolder});
        in(hangSection, {&hangOn, &gameSeconds, &renderSeconds, &sampleStack});
    }

    void OnEnable() override { Apply(); }
    void OnDisable() override
    {
        // Off: the game handles its own crashes again; hangs aren't watched
        mirage::crash::SetMode(mirage::crash::Mode::Game);
        auto h = mirage::hang::Configured();
        h.enabled = false;
        mirage::hang::Configure(h);
    }
    void OnTick() override { Apply(); }

    void OnRender() override
    {
        mirage::hang::BeatRender();         // The render thread's heartbeat (while this module is on)
        if (!checkedLast_) {
            checkedLast_ = true;
            if (showLast.Get() && mirage::crash::TakeUnseen(last_)) {
                lastText_ = mirage::crash::ReadReport(last_.path, 24 * 1024);
                showLastWindow_ = true;
            }
        }
        if (showLastWindow_) DrawLastCrash();
        crashhandler::window::DrawPreview();
    }

    void DrawPanel(float width) override
    {
        const auto hs = mirage::hang::GetStatus();
        ImGui::Text("Hangs: %llu%s", static_cast<unsigned long long>(hs.hangs), hs.hungNow ? " (one now)" : "");
        if (!hs.last.empty()) ImGui::TextWrapped("Last: %s", hs.last.c_str());
        ImGui::TextDisabled("Since the last tick %.1f s, frame %.1f s", hs.sinceGameBeat, hs.sinceRenderBeat);
        const auto now = std::chrono::steady_clock::now();
        if (now - listed_ > std::chrono::seconds(2)) {
            reports_ = mirage::crash::List(8);
            listed_ = now;
        }
        if (reports_.empty()) {
            ImGui::TextDisabled("No crash reports");
            return;
        }
        if (ImGui::TreeNode("Crash reports")) {
            for (const auto& r : reports_) {
                ImGui::PushID(r.path.string().c_str());
                if (ImGui::SmallButton("Open")) OpenReport(r.path);
                ImGui::SameLine();
                ImGui::TextUnformatted(r.path.filename().string().c_str());
                ImGui::Indent();
                ImGui::TextDisabled("%s", r.summary.c_str());
                ImGui::Unindent();
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        (void)width;
    }

private:
    void Apply()
    {
        mirage::crash::SetMode(handle.Get() == 0 ? mirage::crash::Mode::Mirage : mirage::crash::Mode::Game);
        mirage::crash::SetOpenReport(openReport.Get());
        mirage::crash::SetWindow(window.Get());
        mirage::hang::Settings h;
        h.enabled = hangOn.Get();
        h.gameSeconds = gameSeconds.Get();
        h.renderSeconds = renderSeconds.Get();
        h.sampleStack = sampleStack.Get();
        mirage::hang::Configure(h);
    }

    void DrawLastCrash()
    {
        ImGui::SetNextWindowSize({620.f, 420.f}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("The game crashed last time", &showLastWindow_)) {
            ImGui::TextWrapped("Mirror's Edge crashed in the last session. The report says:");
            ImGui::Separator();
            ImGui::TextUnformatted(last_.summary.c_str());
            if (ImGui::Button("Open the report")) OpenReport(last_.path);
            ImGui::SameLine();
            if (ImGui::Button("Open the folder")) OpenPath(mirage::crash::Folder());
            ImGui::SameLine();
            if (ImGui::Button("Copy")) ImGui::SetClipboardText(lastText_.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Close")) showLastWindow_ = false;
            ImGui::BeginChild("report", {0.f, 0.f}, true, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(lastText_.c_str());
            ImGui::EndChild();
        }
        ImGui::End();
    }

    ctl::Section crashSection{this, "Crashes", true};
    ctl::Combo handle{this, "Handle crashes", {"Mirage (report + minidump)", "The game's own"}, 0,
                      "Mirage: a fatal crash writes a report and a minidump to mirage\\crashlogs, opens the report once the game is "
                      "gone, and the next start shows it. The game's own: its crash dialog, as without the menu"};
    ctl::Checkbox window{this, "Show the crash window", true,
                         "When the game crashes, a window over the game shows what happened (where, likely causes, both call "
                         "stacks, registers, the code), and lets you copy the report, restart or close the game"};
    ctl::Checkbox openReport{this, "Open the report after a crash", true, "When the crash window can't be shown"};
    ctl::Checkbox showLast{this, "Show the last crash at start", true};
    ctl::Button openFolder{this, "Open the crash reports folder", [] { OpenPath(mirage::crash::Folder()); }};

    ctl::Section hangSection{this, "Hangs", true};
    ctl::Checkbox hangOn{this, "Detect hangs", true,
                         "Notices when the game or the menu stops responding, says where (in the log and a report) and how to quit "
                         "(Ctrl+Alt+Shift+F12). Watches only: it never changes the game"};
    ctl::Slider gameSeconds{this, "Game thread after", 10.f, 3.f, 60.f, "%.0f s", 1.f, "No engine tick for this long (not counting level loads)"};
    ctl::Slider renderSeconds{this, "Render thread after", 10.f, 3.f, 60.f, "%.0f s", 1.f, "No frame for this long while the game is in front"};
    ctl::Checkbox sampleStack{this, "Find where it is stuck", true,
                              "Pauses the stuck thread for a moment to read its stack (only once per hang)"};

    bool checkedLast_ = false, showLastWindow_ = false;
    mirage::crash::Report last_;
    std::string lastText_;
    std::vector<mirage::crash::Report> reports_;
    std::chrono::steady_clock::time_point listed_{};
};

MEM_MODULE(CrashReporter)

} // namespace
