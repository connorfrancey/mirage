// The Memory module: the settings of the runtime's memory manager (runtime/mem/manager.h), kept in the menu's config like
// any module's, and what it sees: pressure, the address space, the menu's heap, caches, pools, arenas, leak suspects.
// Switched off, the manager only watches (it never gives memory back or changes the cache budget).
#include "core/module.h"
#include "runtime/mem/manager.h"

#include <imgui.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>

namespace {

class Memory final : public Module {
public:
    Memory() : Module("Memory", "Debug", "The memory manager: pressure, leaks, caches, pools and arenas (the menu's own memory only)", 6)
    {
        enabledByDefault = true;
        schedule.interval = 0.5f;           // Settings changes reach the manager within half a second
        schedule.priority = sched::Priority::Background;
        const auto in = [](ctl::Section& section, std::initializer_list<ctl::Setting*> controls) {
            for (ctl::Setting* c : controls) c->VisibleIf([&section] { return section.Get(); });
        };
        in(manageSection, {&act, &cacheMin, &cacheMax, &menuBudget, &trimNow});
        in(trackSection, {&tracking, &sampleKb, &largeMb, &logFindings, &mark, &since});
        sampleKb.VisibleIf([this] { return trackSection && tracking.Get() == 1; });
        Push(false);    // Until the saved state is known: watch only
    }

    void OnEnable() override { Push(true); }
    void OnDisable() override { Push(false); }
    void OnTick() override { Push(true); }

    void DrawPanel(float width) override
    {
        Push(Enabled());
        const auto now = std::chrono::steady_clock::now();
        if (now - lastStatus_ > std::chrono::milliseconds(500)) {
            status_ = mirage::mem::GetStatus();
            lastStatus_ = now;
        }
        const auto& st = status_;
        const auto mb = [](double b) { return b / (1024.0 * 1024.0); };
        static const ImVec4 kLevel[] = {{0.5f, 0.85f, 0.5f, 1.f}, {0.95f, 0.8f, 0.3f, 1.f}, {1.f, 0.55f, 0.2f, 1.f}, {1.f, 0.3f, 0.3f, 1.f}};
        ImGui::TextColored(kLevel[static_cast<int>(st.level)], "Pressure: %s", mirage::mem::LevelName(st.level));
        if (!st.reason.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", st.reason.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", st.acting ? "(acting)" : "(watching only)");
        if (st.address.complete)
            ImGui::Text("Address space: %.0f MB free, largest block %.0f MB, fragmentation %.2f", mb(static_cast<double>(st.address.free.totalFree)),
                        mb(static_cast<double>(st.address.free.largestFree)), st.address.free.Index());
        ImGui::Text("Process: %.0f MB private, %.0f MB working set", mb(static_cast<double>(st.process.privateBytes)),
                    mb(static_cast<double>(st.process.workingSet)));
        ImGui::Text("Menu heap: %.1f MB live (peak %.1f), %.0f allocations/s", mb(static_cast<double>(st.heap.live)),
                    mb(static_cast<double>(st.heap.peakLive)), st.allocRate);
        ImGui::Text("Caches: %.1f of %.1f MB", mb(static_cast<double>(st.cacheBytes)), mb(static_cast<double>(st.cacheBudget)));
        if (st.secondsToExhaustion)
            ImGui::TextColored(kLevel[2], "Largest free block critical in %.0f s at this rate", *st.secondsToExhaustion);
        if (!st.lastAction.empty()) ImGui::TextDisabled("Last action: %s", st.lastAction.c_str());
        ImGui::TextDisabled("Manager: %.3f ms a step (max %.3f)", st.stepMs, st.maxStepMs);

        if (!st.recent.empty() && ImGui::TreeNode("Recent findings")) {
            for (auto it = st.recent.rbegin(); it != st.recent.rend(); ++it)
                ImGui::TextWrapped("%s%s: %s", it->severity == mirage::mem::Finding::Severity::Warning ? "! " : "", it->subject.c_str(),
                                   it->text.c_str());
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Full report")) {
            if (now - lastReport_ > std::chrono::seconds(2) || report_.empty()) {
                report_ = mirage::mem::Report(false);
                lastReport_ = now;
            }
            if (ImGui::SmallButton("Copy (with call stacks)")) ImGui::SetClipboardText(mirage::mem::Report(true).c_str());
            ImGui::BeginChild("report", {width, 260.f}, true, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(report_.c_str());
            ImGui::EndChild();
            ImGui::TreePop();
        }
    }

private:
    void Push(bool on)
    {
        mirage::mem::Config c;
        c.act = on && act.Get();
        c.tracking = static_cast<mirage::mem::sampler::Mode>(tracking.Get());
        c.sampleInterval = static_cast<std::size_t>(sampleKb.Get()) * 1024;
        c.cacheMin = static_cast<std::size_t>(cacheMin.Get()) << 20;
        c.cacheMax = static_cast<std::size_t>(std::max(cacheMax.Get(), cacheMin.Get())) << 20;
        c.menuBudget = static_cast<std::size_t>(menuBudget.Get()) << 20;
        c.largeAllocation = static_cast<std::size_t>(largeMb.Get()) << 20;
        c.logFindings = logFindings.Get();
        mirage::mem::Configure(c);
    }

    void LogSinceMark()
    {
        const auto rep = mirage::mem::sampler::Snapshot(mark_, 10);
        spdlog::info("Memory: live since the mark (estimated): {:.2f} MB in {} blocks, {} stacks", static_cast<double>(rep.liveBytes) / 1048576.0,
                     rep.liveBlocks, rep.sites.size());
        for (const auto& site : rep.sites) {
            std::string frames;
            for (std::size_t i = 0; i < site.frames.size() && i < 6; ++i)
                frames += (i ? " < " : "") + mirage::mem::sampler::Describe(site.frames[i]);
            spdlog::info("Memory:   {:.1f} KB in {} blocks [{}] {}", static_cast<double>(site.liveBytes) / 1024.0, site.liveBlocks,
                         mirage::mem::TagName(site.tag), frames);
        }
    }

    ctl::Section manageSection{this, "Management", true};
    ctl::Checkbox act{this, "Act on memory pressure", true,
                      "Give memory back when the process runs short (the menu's caches, pools and arenas; mods are told) and adapt "
                      "the cache budget. Off: only watch and report. The game's memory is never touched either way"};
    ctl::SliderInt cacheMin{this, "Cache budget min", 4, 1, 64, "%d MB", 1, "The least the menu's caches keep, whatever the pressure"};
    ctl::SliderInt cacheMax{this, "Cache budget max", 64, 8, 512, "%d MB", 8, "The most the caches may grow to while they are used and memory is plentiful"};
    ctl::SliderInt menuBudget{this, "Menu heap budget", 256, 64, 1024, "%d MB", 16,
                              "The menu's own heap over 1.5 times this counts as memory pressure"};
    ctl::Button trimNow{this, "Give memory back now", [] { mirage::mem::RequestTrim(); },
                        "Caches down to their budget, spare pool slabs and arena chunks freed"};

    ctl::Section trackSection{this, "Tracking", false};
    ctl::Combo tracking{this, "Allocation tracking", {"Off", "Sampled", "Full (slow)"}, 1,
                        "Sampled: about one allocation per sample interval is recorded with its call stack (leak suspects, live memory by "
                        "tag and stack, lifetimes) at almost no cost. Full: every allocation (exact, slow: for finding a leak)"};
    ctl::SliderInt sampleKb{this, "Sample interval", 256, 16, 4096, "%d KB", 16, "Smaller: more detail, a little more cost"};
    ctl::SliderInt largeMb{this, "Log allocations over", 8, 0, 64, "%d MB", 1, "With their call stack. 0: off"};
    ctl::Checkbox logFindings{this, "Log findings", true, "Anomalies, leak suspects and large allocations go to the log (always in the report)"};
    ctl::Button mark{this, "Mark", [this] { mark_ = mirage::mem::sampler::Generation(); spdlog::info("Memory: mark set"); },
                     "Remember this moment, then use \"Log live since mark\" to see what was allocated after it and is still there"};
    ctl::Button since{this, "Log live since mark", [this] { LogSinceMark(); }, "What is still allocated of what was allocated after the mark, by call stack"};

    std::uint32_t mark_ = 0;
    mirage::mem::Status status_;
    std::string report_;
    std::chrono::steady_clock::time_point lastStatus_{}, lastReport_{};
};

MEM_MODULE(Memory)

} // namespace
