# Speedrun Timer: a native first-party mod (cmake/FirstPartyMods.cmake), built against api/ and the public SDK only
mirage_native_mod(memenu-speedrun SOURCES module.cpp live.cpp timer.cpp LIBS nlohmann_json::nlohmann_json mirage::sdk)
