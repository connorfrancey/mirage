// Save State, Load State, Save States (named states per level) and Refill Reaction Time.
//
// A native first-party mod: the public API (mirage/api.h) and SDK (mirage/sdk.hpp) only; the player's state itself is
// the runtime's (MirageApi::player_state_capture / _apply). Shares has_save for the self-test, which drives the
// library through mirage.shared("mirage", "save_states.command") ("<n> <command> [index]") and points it at a folder
// of its own (mirage/save_states.folder).
#include "library.h"

#include "mirage/sdk.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace {

void Notify(const std::string& text) { mirage::Api().notify(mirage::Mod(), text.c_str()); }

// ---- Game-thread work from the menu's thread (on_trigger, buttons): run in the next tick event -------------------------------
std::mutex g_queueLock;
std::vector<std::function<void()>> g_queue;
void Later(std::function<void()> fn)
{
    std::lock_guard lock(g_queueLock);
    g_queue.push_back(std::move(fn));
}
void RunQueue()
{
    std::vector<std::function<void()>> queue;
    {
        std::lock_guard lock(g_queueLock);
        queue.swap(g_queue);
    }
    for (auto& fn : queue) fn();
}

// Strang and Beamer armed when a state loads would fire right away in it (the trainer drops them too). They are the
// Trainer mod's: switched off by name.
void DropTools()
{
    for (const char* tool : {"Strang", "Beamer"}) mirage::Api().module_set_state(tool, 0);
}

std::string Shared(const char* key)
{
    char v[256]{};
    mirage::Api().share_get("mirage", key, v, sizeof v);
    return v;
}

void LoadEntry(int index)
{
    Later([index] {
        DropTools();
        std::string error;
        const auto list = save_library::List();
        if (save_library::Load(index, &error)) {
            if (index >= 0 && index < static_cast<int>(list.size())) Notify("Loaded " + list[static_cast<std::size_t>(index)].name);
        } else {
            Notify("Load: " + error);
        }
    });
}

std::string SelectedName()
{
    const auto list = save_library::List();
    const int i = save_library::Selected();
    return i >= 0 && i < static_cast<int>(list.size()) ? "State: " + list[static_cast<std::size_t>(i)].name : "No save states here";
}

// The queue, the self-test's folder and commands, and has_save: from the tick event (every tick in a level)
std::string g_lastCommand;
void Tick()
{
    save_library::SetFolder(Shared("save_states.folder"));
    const std::string cmd = Shared("save_states.command");
    if (!cmd.empty() && cmd != g_lastCommand) {
        g_lastCommand = cmd;
        char what[32]{};
        int index = 0;
        sscanf_s(cmd.c_str(), "%*d %31s %d", what, static_cast<unsigned>(sizeof what), &index);
        const std::string w = what;
        std::string error;
        if (w == "load") save_library::Load(index, &error);
        else if (w == "save_new") save_library::SaveNew(&error);
        else if (w == "clear") save_library::ClearCurrent();
        mirage::Api().share_set(mirage::Mod(), "command.error", error.c_str());
        mirage::Api().share_set(mirage::Mod(), "command.done", cmd.c_str());
    }
    RunQueue();
    mirage::Api().share_set(mirage::Mod(), "has_save", save_library::HasCurrent() ? "1" : "0");
    const auto list = save_library::List(mirage::sdk::MapName());
    mirage::Api().share_set(mirage::Mod(), "states", std::to_string(list.size()).c_str());
    mirage::Api().share_set(mirage::Mod(), "map", save_library::Map().c_str());
}

class SaveState final : public mirage::Module {
public:
    SaveState() : Module("Save State", "Trainer", "Remember where you are, how fast you move and what you are doing", 50, '4',
                         MIRAGE_MODULE_ACTION)
    {
        mirage::On(MIRAGE_EVENT_TICK, [](const MirageEvent&) { Tick(); });
    }

    void OnTrigger() override
    {
        Later([] {
            std::string error;
            if (!mirage::sdk::Pawn()) Notify("Save State: no player");
            else if (!save_library::SaveCurrent(&error)) Notify("Save State: " + error);
            else Notify("State saved");
        });
    }

    void OnPanel() override
    {
        if (mirage::Ui().button("Clear saved state", mirage::Ui().content_width(), 0.f)) Later([] { save_library::ClearCurrent(); });
    }
};
MIRAGE_MODULE(SaveState)

class LoadState final : public mirage::Module {
public:
    LoadState() : Module("Load State", "Trainer", "Go back to the saved state", 51, '5', MIRAGE_MODULE_ACTION) {}

    void OnTrigger() override
    {
        Later([] {
            if (!save_library::HasCurrent()) {
                Notify("Load State: nothing saved yet");
                return;
            }
            auto* pawn = mirage::sdk::Pawn();
            if (!pawn || pawn->Health <= 0) return;
            DropTools();
            std::string error;
            if (!save_library::LoadCurrent(&error)) Notify("Load State: not right now");
        });
    }
};
MIRAGE_MODULE(LoadState)

class SaveStates final : public mirage::Module {
public:
    SaveStates()
        : Module("Save States", "Trainer", "Named save states for each level, kept between sessions. Run: save a new one", 52,
                 0, MIRAGE_MODULE_ACTION | MIRAGE_MODULE_PANEL_OPEN)
    {}

    void OnTrigger() override
    {
        Later([] {
            std::string error;
            if (save_library::SaveNew(&error)) {
                const auto list = save_library::List();
                if (const int i = save_library::Selected(); i >= 0 && i < static_cast<int>(list.size()))
                    Notify("Saved " + list[static_cast<std::size_t>(i)].name);
            } else {
                Notify("Save: " + error);
            }
        });
    }

    void OnPanel() override
    {
        const MirageUi& ui = mirage::Ui();
        const float width = ui.content_width();
        DrawList(ui, width);
        if (ui.button("Save new", width, 0.f)) OnTrigger();
        if (ui.item_hovered()) ui.tooltip("Save where you are as a new state");
        if (ui.button("Open save states folder", width, 0.f)) mirage::Api().file_open_folder(mirage::Mod(), save_library::Folder().c_str());
    }

private:
    void DrawList(const MirageUi& ui, float width)
    {
        const std::string map = mirage::sdk::Ready() ? save_library::Map() : std::string();
        const auto list = save_library::List();
        const int selected = save_library::Selected();
        if (map.empty() && list.empty()) {
            ui.text_disabled("Open a level to see its states");
            return;
        }
        char line[160];
        std::snprintf(line, sizeof line, "%zu state%s in %s", list.size(), list.size() == 1 ? "" : "s", map.c_str());
        ui.text_disabled(line);
        if (list.empty()) {
            ui.text_disabled("None yet: run this or press Save new");
            return;
        }

        const float height = std::min(static_cast<float>(list.size() + 1), 10.f) * ui.frame_height() + 6.f;
        if (ui.table_begin("##states", 2, MIRAGE_TABLE_ROW_BG | MIRAGE_TABLE_SCROLL_Y, width, height)) {
            ui.table_column("State", MIRAGE_COLUMN_STRETCH, 0.f);
            ui.table_column("Speed", MIRAGE_COLUMN_FIXED, 50.f);
            ui.table_headers();
            for (int i = 0; i < static_cast<int>(list.size()); ++i) {
                const auto& e = list[static_cast<std::size_t>(i)];
                ui.id_push_int(i);
                ui.table_next_row();
                ui.table_next_column();
                if (ui.selectable(e.name.c_str(), i == selected, 0.f, 0.f)) save_library::Select(i);
                if (ui.item_double_clicked()) LoadEntry(i);
                if (ui.item_hovered()) {
                    std::snprintf(line, sizeof line, "%s\nAt %.0f, %.0f, %.0f\nSaved %s\nDouble-click to load", e.name.c_str(),
                                  e.x, e.y, e.z, e.created.c_str());
                    ui.tooltip(line);
                }
                ui.table_next_column();
                std::snprintf(line, sizeof line, "%.0f", e.speed);
                ui.text_disabled(line);
                ui.id_pop();
            }
            ui.table_end();
        }

        if (selected < 0 || selected >= static_cast<int>(list.size())) return;
        const auto& sel = list[static_cast<std::size_t>(selected)];

        // Rename: the box shows the selected state's name; leaving the box keeps what was typed
        if (renameFor_ != selected || (!renaming_ && renameName_ != sel.name)) {
            std::snprintf(rename_, sizeof rename_, "%s", sel.name.c_str());
            renameFor_ = selected;
            renameName_ = sel.name;
        }
        ui.next_width(width);
        ui.input_text_hint("##name", "name", rename_, sizeof rename_, 0);
        renaming_ = ui.item_active() != 0;
        if (ui.item_hovered()) ui.tooltip("Name of the selected state");
        if (ui.item_deactivated() && rename_[0]) {
            save_library::Rename(selected, rename_);
            renameName_ = rename_;
        }

        const float third = (width - 16.f) / 3.f;
        if (ui.button("Load", third, 0.f)) LoadEntry(selected);
        if (ui.item_hovered()) ui.tooltip("Load it (and make it the Save State: the Load State key comes back here)");
        ui.same_line(0.f, -1.f);
        const float now = ui.time();
        const bool pending = now - confirmAt_ < 3.f;
        if (ui.button(confirm_ == 1 && pending ? "Sure?##o" : "Overwrite", third, 0.f)) {
            if (confirm_ == 1 && pending) {
                confirm_ = 0;
                Later([selected] {
                    std::string error;
                    Notify(save_library::Overwrite(selected, &error) ? "State overwritten" : "Overwrite: " + error);
                });
            } else {
                confirm_ = 1;
                confirmAt_ = now;
            }
        }
        if (ui.item_hovered()) ui.tooltip("Replace it with where you are now");
        ui.same_line(0.f, -1.f);
        if (ui.button(confirm_ == 2 && pending ? "Sure?##d" : "Delete", third, 0.f)) {
            if (confirm_ == 2 && pending) {
                confirm_ = 0;
                save_library::Remove(selected);
            } else {
                confirm_ = 2;
                confirmAt_ = now;
            }
        }

        const float half = (width - 8.f) / 2.f;
        ui.disabled_begin(selected == 0);
        if (ui.button("Move up", half, 0.f)) save_library::MoveBy(selected, -1);
        ui.disabled_end();
        ui.same_line(0.f, -1.f);
        ui.disabled_begin(selected + 1 >= static_cast<int>(list.size()));
        if (ui.button("Move down", half, 0.f)) save_library::MoveBy(selected, 1);
        ui.disabled_end();
    }

    // Render thread
    char rename_[64] = "";
    int renameFor_ = -1;
    std::string renameName_;
    bool renaming_ = false;
    int confirm_ = 0;           // 1 overwrite, 2 delete
    float confirmAt_ = -10.f;
};
MIRAGE_MODULE(SaveStates)

// What were buttons with keys on Save States: actions now, so they keep their keys
class LoadSelectedState final : public mirage::Module {
public:
    LoadSelectedState() : Module("Load Selected State", "Trainer", "Load the state selected in Save States", 53, 0, MIRAGE_MODULE_ACTION) {}
    void OnTrigger() override { LoadEntry(save_library::Selected()); }
};
MIRAGE_MODULE(LoadSelectedState)

class NextState final : public mirage::Module {
public:
    NextState() : Module("Next Save State", "Trainer", "Select the next state in Save States", 54, 0, MIRAGE_MODULE_ACTION) {}
    void OnTrigger() override
    {
        save_library::SelectNext(1);
        Notify(SelectedName());
    }
};
MIRAGE_MODULE(NextState)

class PreviousState final : public mirage::Module {
public:
    PreviousState() : Module("Previous Save State", "Trainer", "Select the previous state in Save States", 55, 0, MIRAGE_MODULE_ACTION) {}
    void OnTrigger() override
    {
        save_library::SelectNext(-1);
        Notify(SelectedName());
    }
};
MIRAGE_MODULE(PreviousState)

class RefillReactionTime final : public mirage::Module {
public:
    RefillReactionTime() : Module("Refill Reaction Time", "Trainer", "Fill the reaction time meter", 60, 0, MIRAGE_MODULE_ACTION) {}

    void OnTrigger() override
    {
        Later([] {
            auto* pawn = mirage::sdk::Pawn();
            auto* controller = mirage::sdk::Controller();
            auto* world = mirage::sdk::World();
            if (!pawn || !controller || !world) return;
            // Same conditions as the trainer's button: not in time trials, not already full or active, not falling to death
            if (world->Game && mirage::sdk::IsA(world->Game, "TdSPTimeTrialGame")) {
                Notify("Reaction time: not in time trials");
                return;
            }
            if (controller->ReactionTimeEnergy >= 100.f || controller->bReactionTime ||
                pawn->MovementState == Classes::EMovement::MOVE_FallingUncontrolled)
                return;
            controller->ReactionTimeEnergy = 100.f;
            if (auto* hud = static_cast<Classes::ATdHUD*>(controller->myHUD); hud && hud->EffectManager)
                hud->EffectManager->ActivateReactionTimeTeaser();
        });
    }
};
MIRAGE_MODULE(RefillReactionTime)

} // namespace
