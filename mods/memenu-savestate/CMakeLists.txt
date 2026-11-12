# Save State, Load State, Save States, Refill Reaction Time: a native first-party mod (cmake/FirstPartyMods.cmake),
# built against api/ and the public SDK only
mirage_native_mod(memenu-savestate SOURCES module.cpp library.cpp LIBS nlohmann_json::nlohmann_json mirage::sdk)
