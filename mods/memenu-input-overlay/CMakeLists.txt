# Input Overlay: a native first-party mod (cmake/FirstPartyMods.cmake), built against api/ only
mirage_native_mod(memenu-input-overlay SOURCES overlay.cpp input_layout.cpp LIBS nlohmann_json::nlohmann_json)
