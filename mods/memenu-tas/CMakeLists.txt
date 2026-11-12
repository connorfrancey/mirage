# TAS / Macro: a native first-party mod (cmake/FirstPartyMods.cmake). The engine stays in the runtime (src/tas: memory
# snapshots, the game thread's clocks and input); this is its UI, driving it through MirageApi tas_*. tas/ holds its own
# copies of the runtime's pure replay code (replay, timeline, history, splits), compat/ the menu's module and controls on
# the public API. The timeline and the editors need more than MirageUi has: they draw with the menu's ImGui
# (MirageApi imgui_context, api/mirage/imgui.hpp), built from the same ImGui version.
mirage_native_mod(memenu-tas
        SOURCES ui/tas_module.cpp ui/timeline_view.cpp ui/replay_editor.cpp ui/macro_library.cpp
                tas/tas_client.cpp tas/replay.cpp tas/timeline.cpp tas/history.cpp tas/splits.cpp
        LIBS nlohmann_json::nlohmann_json mirage::sdk magic_enum::magic_enum imgui imgui_filebrowser)
