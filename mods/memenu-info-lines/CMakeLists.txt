# Info Lines: a native first-party mod (cmake/FirstPartyMods.cmake), built against api/ and the public SDK only
mirage_native_mod(memenu-info-lines SOURCES info_lines.cpp template.cpp LIBS mirage::sdk fmt::fmt)
