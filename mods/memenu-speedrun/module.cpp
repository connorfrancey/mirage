// Speedrun Timer: a load removed timer with auto splits on chapter ends and personal best comparison, in the spirit of
// softsoundd's Softimer (Mirror's Edge Tweaks). The run logic is live.cpp; this is the HUD and its settings.
//
// A native first-party mod: the public API (mirage/api.h) and the public SDK (mirage/sdk.hpp) only. It shares its run
// (mirage.shared("memenu-speedrun", "run.*")) for Info Lines and the self-test, and takes commands from the menu's own
// tests (mirage.shared("memenu", "speedrun.command"): a counter and the command, "3 reset").
#include "live.h"

#include "mirage/sdk.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

using speedrun::Timer;

float Channel(uint32_t c, int shift) { return static_cast<float>((c >> shift) & 0xFF) / 255.f; }
uint32_t Pack(float r, float g, float b, float a)
{
    const auto u = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    return u(r) | (u(g) << 8) | (u(b) << 16) | (u(a) << 24);
}
uint32_t Alpha(uint32_t c, float times) { return Pack(Channel(c, 0), Channel(c, 8), Channel(c, 16), Channel(c, 24) * times); }
uint32_t Lighter(uint32_t c)
{
    const auto l = [&](int s) { return Channel(c, s) + (1.f - Channel(c, s)) * 0.45f; };
    return Pack(l(0), l(8), l(16), Channel(c, 24));
}

const MirageUi& Ui() { return mirage::Ui(); }
uint32_t TextColor() { return mirage::Api().theme_color(MIRAGE_THEME_TEXT); }
uint32_t DimColor() { return mirage::Api().theme_color(MIRAGE_THEME_TEXT_DIM); }
uint32_t SettingColor(const char* key) { return static_cast<uint32_t>(mirage::Get<int>(key)); }

// Text right-aligned in what's left of the current line / cell
void RightText(uint32_t color, const std::string& s, float size = 0.f)
{
    float w = 0.f, h = 0.f;
    if (size > 0.f) mirage::Api().text_size_sized(s.c_str(), size, &w, &h);
    else Ui().text_size(s.c_str(), &w, &h);
    const float avail = Ui().content_width();
    float x = 0.f, y = 0.f;
    Ui().cursor_screen_pos(&x, &y);
    if (avail > w) x += avail - w;
    if (size > 0.f) {       // Big text: drawn, with its room taken
        Ui().window_draw_text_sized(x, y, color, size, s.c_str());
        Ui().dummy(std::max(avail, w), h);
        return;
    }
    if (avail > w) Ui().dummy(avail - w, 0.f), Ui().same_line(0.f, 0.f);
    Ui().text_colored(color, s.c_str());
}

class SpeedrunTimer final : public mirage::Module {
public:
    SpeedrunTimer() : Module("Speedrun Timer", "Trainer", "Load removed timer with auto splits and personal bests", 75) {}

    void OnTick() override
    {
        speedrun::SetOptions(Options(true));
        RunTestCommand();
        speedrun::Tick();
        Share();
    }

    void OnDisable() override { speedrun::SetOptions(Options(false)); }

    void OnRender() override
    {
        view_ = speedrun::Snapshot();
        const auto& v = view_;
        const auto phase = v.timer.GetPhase();
        const bool menu = mirage::Api().menu_open() != 0;
        if (mirage::Get<bool>("hide_while_running") && phase == Timer::Phase::Running && !menu) return;

        uint32_t flags = MIRAGE_WINDOW_NO_COLLAPSE | MIRAGE_WINDOW_NO_SCROLLBAR | MIRAGE_WINDOW_AUTO_SIZE;
        if (!mirage::Get<bool>("title_bar")) flags |= MIRAGE_WINDOW_NO_TITLE;
        if (!menu) flags |= MIRAGE_WINDOW_NO_INPUTS;      // Only movable while the menu is open
        float sw = 0.f, sh = 0.f;
        mirage::Api().screen_size(&sw, &sh);
        const float width = mirage::Get<float>("width");
        Ui().window_next_pos(sw - width - 30.f, 40.f, 0);
        Ui().window_next_bg_alpha(mirage::Get<float>("opacity"));
        if (Ui().window_begin("Speedrun timer##speedrun", nullptr, flags)) {
            Ui().dummy(width - 16.f, 0.f);           // The window's width (auto size fits the content)
            if (mirage::Get<bool>("show_splits")) DrawSplits(v);
            DrawTimer(v);
            DrawInfo(v);
        }
        Ui().window_end();
    }

    void OnPanel() override
    {
        const bool run = mirage::Get<bool>("section_run"), autos = mirage::Get<bool>("section_auto");
        const bool show = mirage::Get<bool>("section_show"), look = mirage::Get<bool>("section_look");
        const bool keys = mirage::Get<bool>("section_keys"), records = mirage::Get<bool>("section_records");
        mirage::SetVisible("category", run);
        for (const char* k : {"auto_start", "auto_split", "auto_finish", "auto_reset", "remove_loads", "pause_in_menus"})
            mirage::SetVisible(k, autos);
        for (const char* k : {"show_splits", "show_segment", "show_last_split", "show_rta", "show_pb", "show_sob", "simple",
                              "decimals", "hide_while_running"})
            mirage::SetVisible(k, show);
        mirage::SetVisible("split_rows", show && mirage::Get<bool>("show_splits"));
        for (const char* k : {"width", "timer_size", "opacity", "title_bar", "ahead_color", "behind_color", "gold_color"})
            mirage::SetVisible(k, look);
        for (const char* k : {"key_split", "key_reset", "key_undo", "key_skip"}) mirage::SetVisible(k, keys);
        if (run) DrawControls();
        if (records) DrawPbInfo();
    }

private:
    speedrun::Options Options(bool enabled) const
    {
        speedrun::Options o;
        o.enabled = enabled;
        o.category = static_cast<speedrun::Category>(mirage::Get<int>("category"));
        o.autoStart = mirage::Get<bool>("auto_start");
        o.autoSplit = mirage::Get<bool>("auto_split");
        o.autoFinish = mirage::Get<bool>("auto_finish");
        o.autoReset = mirage::Get<bool>("auto_reset");
        o.removeLoads = mirage::Get<bool>("remove_loads");
        o.pauseInMenus = mirage::Get<bool>("pause_in_menus");
        o.keySplit = mirage::Get<int>("key_split");
        o.keyReset = mirage::Get<int>("key_reset");
        o.keyUndo = mirage::Get<int>("key_undo");
        o.keySkip = mirage::Get<int>("key_skip");
        return o;
    }

    // The run, for Info Lines ({runtime}, {runrta}) and the self-test
    void Share()
    {
        const speedrun::View v = speedrun::Snapshot();
        const auto phase = v.timer.GetPhase();
        const auto put = [](const char* key, const std::string& value) { mirage::Api().share_set(mirage::Mod(), key, value.c_str()); };
        char n[64];
        put("run.phase", phase == Timer::Phase::Idle ? "idle" : phase == Timer::Phase::Running ? "running" : "finished");
        std::snprintf(n, sizeof n, "%.6f", phase == Timer::Phase::Finished ? v.timer.FinalLrt() : v.timer.Lrt(v.now));
        put("run.lrt", n);
        std::snprintf(n, sizeof n, "%.6f", v.timer.Rta(v.now));
        put("run.rta", n);
        if (shareTick_++ % 30 != 0 && !forceShare_) return;      // The rest twice a second (a command: at once)
        forceShare_ = false;
        put("run.map", v.map);
        put("run.event", v.lastEvent);
        put("run.hooked", v.hooked ? "1" : "0");
        put("run.new_pb", v.newPb ? "1" : "0");
        put("run.completed", std::to_string(v.comparison.completed));
        std::snprintf(n, sizeof n, "%.6f", v.comparison.PbTime());
        put("run.pb", n);
        put("run.file", "speedrun/" + speedrun::FileName(v.categoryLabel) + ".json");
    }

    // The menu's self-test drives the timer: "<counter> <command>" (start_or_split, reset, undo, skip)
    void RunTestCommand()
    {
        char buf[64]{};
        mirage::Api().share_get("memenu", "speedrun.command", buf, sizeof buf);
        const std::string cmd = buf;
        if (cmd.empty() || cmd == lastCommand_) return;
        lastCommand_ = cmd;
        forceShare_ = true;
        const std::string what = cmd.substr(cmd.find(' ') + 1);
        if (what == "start_or_split") speedrun::StartOrSplit();
        else if (what == "reset") speedrun::Reset();
        else if (what == "undo") speedrun::Undo();
        else if (what == "skip") speedrun::SkipSplit();
    }

    double Lrt(const speedrun::View& v) const { return v.timer.Lrt(speedrun::Now()); }

    // Where the run stands against the personal best: < 0 ahead, > 0 behind, NaN without a comparison
    double LiveDelta(const speedrun::View& v) const
    {
        const auto& segs = v.comparison.segments;
        const std::size_t i = v.timer.Splits().size();
        if (v.timer.GetPhase() == Timer::Phase::Finished) {
            const double pb = v.newPb ? -1.0 : v.comparison.PbTime();
            return pb >= 0.0 ? v.timer.FinalLrt() - pb : (v.newPb ? -1.0 : std::nan(""));
        }
        if (i >= segs.size() || segs[i].pb < 0.0) return std::nan("");
        return Lrt(v) - segs[i].pb;
    }

    uint32_t DeltaColor(double delta, double previous) const
    {
        const bool ahead = delta < 0.0;
        // Losing time while ahead / gaining while behind: the lighter shade (like LiveSplit)
        const bool lighter = !std::isnan(previous) && (ahead ? delta > previous : delta < previous);
        const uint32_t c = SettingColor(ahead ? "ahead_color" : "behind_color");
        return lighter ? Lighter(c) : c;
    }

    void DrawSplits(const speedrun::View& v)
    {
        const auto& segs = v.comparison.segments;
        const auto& run = v.timer.Splits();
        if (segs.empty()) return;
        const int d = mirage::Get<int>("decimals");
        const bool running = v.timer.Running();
        const int current = running ? static_cast<int>(run.size()) : -1;

        // A window of rows that keeps the current split in view, with the last split always at the bottom
        const int total = static_cast<int>(segs.size());
        const int rows = std::min(total, std::max(2, mirage::Get<int>("split_rows")));
        const int focus = current >= 0 ? current : static_cast<int>(run.size()) - 1;
        const int first = std::clamp(focus - rows + 3, 0, total - rows);

        Ui().text_disabled(v.categoryLabel.c_str());
        Ui().same_line(0.f, -1.f);
        RightText(DimColor(), std::to_string(v.comparison.attempts));

        float deltaW = 0.f, timeW = 0.f, h = 0.f;
        Ui().text_size("-0:00.00", &deltaW, &h);
        Ui().text_size("0:00:00.00", &timeW, &h);
        if (!Ui().table_begin("##splits", 3, MIRAGE_TABLE_SIZING_FIXED, 0.f, 0.f)) return;
        Ui().table_column("name", MIRAGE_COLUMN_STRETCH, 0.f);
        Ui().table_column("delta", MIRAGE_COLUMN_FIXED, deltaW);
        Ui().table_column("time", MIRAGE_COLUMN_FIXED, timeW);

        double previousDelta = std::nan("");
        for (int i = 0; i < total; ++i) {
            const bool shown = (i >= first && i < first + rows - 1) || i == total - 1;
            const speedrun::Segment& s = segs[static_cast<std::size_t>(i)];
            double delta = std::nan("");
            if (i < static_cast<int>(run.size()) && run[static_cast<std::size_t>(i)].time >= 0.0 && s.pb >= 0.0)
                delta = run[static_cast<std::size_t>(i)].time - s.pb;
            if (shown) DrawSplitRow(v, i, current, delta, previousDelta, d);
            if (!std::isnan(delta)) previousDelta = delta;
        }
        Ui().table_end();
        Ui().separator();
    }

    void DrawSplitRow(const speedrun::View& v, int i, int current, double delta, double previousDelta, int d)
    {
        const auto& s = v.comparison.segments[static_cast<std::size_t>(i)];
        const auto& run = v.timer.Splits();
        const bool done = i < static_cast<int>(run.size());

        Ui().table_next_row();
        Ui().table_next_column();
        if (i == current) {     // The current row, highlighted
            float x1, y1, x2, y2;
            Ui().cursor_screen_pos(&x1, &y1);
            Ui().window_rect(nullptr, nullptr, &x2, &y2);
            Ui().window_draw_rect(x1 - 4.f, y1, x2 - 4.f, y1 + Ui().line_height(), Alpha(mirage::Api().theme_color(MIRAGE_THEME_ACCENT), 0.25f), 0.f, 0.f);
        }
        Ui().text_colored(i == current || done ? TextColor() : DimColor(), s.name.c_str());

        Ui().table_next_column();
        if (done) {
            const double seg = speedrun::Comparison::SegmentTime(run, static_cast<std::size_t>(i));
            const bool gold = seg >= 0.0 && s.best >= 0.0 && seg < s.best;
            if (!std::isnan(delta))
                RightText(gold ? SettingColor("gold_color") : DeltaColor(delta, previousDelta), speedrun::FormatDelta(delta, std::min(d, 2)));
        } else if (i == current && s.pb >= 0.0) {
            const double live = Lrt(v) - s.pb;   // Live: only once behind the PB split
            if (live > 0.0) RightText(SettingColor("behind_color"), speedrun::FormatDelta(live, std::min(d, 1)));
        }

        Ui().table_next_column();
        if (done) RightText(TextColor(), run[static_cast<std::size_t>(i)].time >= 0.0 ? speedrun::Format(run[static_cast<std::size_t>(i)].time, std::min(d, 2)) : "-");
        else RightText(DimColor(), s.pb >= 0.0 ? speedrun::Format(s.pb, std::min(d, 2)) : "-");
    }

    void DrawTimer(const speedrun::View& v)
    {
        const auto phase = v.timer.GetPhase();
        const double delta = LiveDelta(v);
        uint32_t color = TextColor();
        if (phase == Timer::Phase::Idle) color = DimColor();
        else if (phase == Timer::Phase::Finished && v.newPb) color = SettingColor("gold_color");
        else if (!std::isnan(delta)) color = SettingColor(delta < 0.0 ? "ahead_color" : "behind_color");
        if (v.loading && phase == Timer::Phase::Running) color = DimColor();

        const std::string text = speedrun::Format(Lrt(v), mirage::Get<int>("decimals"));
        const float size = mirage::Get<float>("timer_size");
        RightText(color, text, size);
        if (!mirage::Get<bool>("simple")) {      // The label in front of the big digits, on their baseline
            float x1, y1, x2, y2, bigW, bigH, lw, lh;
            Ui().item_rect(&x1, &y1, &x2, &y2);
            mirage::Api().text_size_sized(text.c_str(), size, &bigW, &bigH);
            const char* label = v.loading && phase == Timer::Phase::Running ? "LOAD" : "LRT";
            Ui().text_size(label, &lw, &lh);
            Ui().window_draw_text(x2 - bigW - lw - 8.f, y2 - lh - 4.f, DimColor(), label);
        }
    }

    void Line(const char* label, const std::string& value, uint32_t color)
    {
        if (!mirage::Get<bool>("simple")) {
            Ui().text_disabled(label);
            Ui().same_line(0.f, -1.f);
        }
        RightText(color, value);
    }

    void DrawInfo(const speedrun::View& v)
    {
        const int d = std::min(mirage::Get<int>("decimals"), 2);
        const bool simple = mirage::Get<bool>("simple");
        const auto& run = v.timer.Splits();
        const auto& segs = v.comparison.segments;
        const auto phase = v.timer.GetPhase();

        if (mirage::Get<bool>("show_segment") && phase == Timer::Phase::Running) {
            const std::size_t i = run.size();
            const double seg = Lrt(v) - v.timer.SegmentStart();
            const bool slowerThanBest = i < segs.size() && segs[i].best >= 0.0 && seg > segs[i].best;
            Line(simple ? "" : (i < segs.size() ? segs[i].name.c_str() : "Segment"), speedrun::Format(seg, d),
                 slowerThanBest ? Alpha(SettingColor("behind_color"), 0.85f) : TextColor());
        }
        if (mirage::Get<bool>("show_last_split") && !run.empty()) {
            const std::size_t i = run.size() - 1;
            const double seg = speedrun::Comparison::SegmentTime(run, i);
            const std::string label = simple ? std::string() : "Last: " + run[i].name;
            Line(label.c_str(), seg >= 0.0 ? speedrun::Format(seg, d) : "-", TextColor());
        }
        if (mirage::Get<bool>("show_rta") && phase != Timer::Phase::Idle)
            Line("RTA", speedrun::Format(v.timer.Rta(speedrun::Now()), d), TextColor());
        if (mirage::Get<bool>("show_pb")) {
            const double pb = v.comparison.PbTime();
            Line("Personal best", pb >= 0.0 ? speedrun::Format(pb, d) : "-", TextColor());
        }
        if (mirage::Get<bool>("show_sob")) {
            const double sob = v.comparison.SumOfBest();
            Line("Sum of best", sob >= 0.0 ? speedrun::Format(sob, d) : "-", TextColor());
        }
        if (mirage::Api().menu_open() && !v.hooked && mirage::Get<bool>("auto_split"))
            Ui().text_disabled("auto splits not available (see the log)");
    }

    // Status and buttons, in the module's panel
    void DrawControls()
    {
        const auto& v = view_;
        const auto phase = v.timer.GetPhase();
        const char* state = phase == Timer::Phase::Idle ? "ready" : phase == Timer::Phase::Finished
                                ? (v.newPb ? "finished: personal best!" : "finished") : (v.loading ? "running (load)" : "running");
        Ui().text_disabled((v.categoryLabel + ", " + state).c_str());
        if (!v.lastEvent.empty()) Ui().text_disabled(("last: " + v.lastEvent).c_str());

        const float bw = (Ui().content_width() - 8.f) / 2.f;
        if (Ui().button(phase == Timer::Phase::Running ? "Split" : "Start", bw, 0.f)) speedrun::StartOrSplit();
        Ui().same_line(0.f, -1.f);
        if (Ui().button("Reset", bw, 0.f)) speedrun::Reset();
        Ui().disabled_begin(phase == Timer::Phase::Idle);
        if (Ui().button("Undo split", bw, 0.f)) speedrun::Undo();
        Ui().same_line(0.f, -1.f);
        if (Ui().button("Skip split", bw, 0.f)) speedrun::SkipSplit();
        Ui().disabled_end();
        if (!v.hooked && Enabled())
            Ui().text_colored(Pack(1.f, 0.7f, 0.3f, 1.f), "Auto splits are off: the game's\nlevel completed event wasn't found.");
    }

    void DrawPbInfo()
    {
        const auto& v = view_;
        const auto& c = v.comparison;
        const double pb = c.PbTime(), sob = c.SumOfBest();
        char line[160];
        std::snprintf(line, sizeof line, "%s: %d attempts, %d finished", v.categoryLabel.c_str(), c.attempts, c.completed);
        Ui().text(line);
        Ui().text(("Personal best  " + (pb >= 0.0 ? speedrun::Format(pb, 3) : std::string("none"))).c_str());
        Ui().text(("Sum of best    " + (sob >= 0.0 ? speedrun::Format(sob, 3) : std::string("-"))).c_str());
        Ui().text_disabled("Saved in this mod's speedrun\\ folder");
        const float now = Ui().time();
        const bool confirm = now < confirmUntil_;
        if (Ui().button(confirm ? "Sure? Click again" : "Forget personal best", Ui().content_width(), 0.f)) {
            if (confirm) {
                speedrun::ForgetPb();
                confirmUntil_ = 0.f;
            } else confirmUntil_ = now + 3.f;
        }
        if (Ui().item_hovered())
            Ui().tooltip(("Clears the personal best and best segments of " + v.categoryLabel + " (not the attempt count)").c_str());
    }

    speedrun::View view_;           // Render thread: the last snapshot
    float confirmUntil_ = 0.f;
    std::string lastCommand_;       // Game thread
    unsigned shareTick_ = 0;
    bool forceShare_ = false;
};
MIRAGE_MODULE(SpeedrunTimer)

} // namespace

MIRAGE_MOD()
