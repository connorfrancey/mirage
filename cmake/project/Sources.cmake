# ======================================================================================================================
# Sources
# ======================================================================================================================

# Find sources and headers
file(GLOB_RECURSE SOURCE_FILES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/loader/src/*.c
        ${CMAKE_SOURCE_DIR}/loader/src/*.cpp)
file(GLOB_RECURSE HEADER_FILES CONFIGURE_DEPENDS
        ${CMAKE_SOURCE_DIR}/loader/src/*.h
        ${CMAKE_SOURCE_DIR}/loader/src/*.hpp)

# Feature modules (loader/src/features/<category>/), the TAS engine and the game layer come from cmake/Features.cmake.
# Only the always-built files stay from the glob: core, menu, hooks, config, themes and features/tabs.cpp.
set(_feature_files ${SOURCE_FILES})
list(FILTER _feature_files INCLUDE REGEX "/src/features/[^/]+/")
list(FILTER SOURCE_FILES EXCLUDE REGEX "/src/(features/[^/]+|tas|game|render|speedrun|runtime|crashhandler)/")
foreach(file IN LISTS _feature_files)   # A new module file has to be listed there, or it would silently not build
    if(NOT file IN_LIST MIRAGE_ALL_FEATURE_SOURCES)
        message(FATAL_ERROR "${file} is not listed in cmake/Features.cmake: add a mirage_feature() for it")
    endif()
endforeach()
list(APPEND SOURCE_FILES ${MIRAGE_FEATURE_SOURCES})

if(MIRAGE_GAME)   # The Mirage runtime (loader/src/runtime/): UnrealLib and Lua, linked with the game layer
    file(GLOB_RECURSE _runtime_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/loader/src/runtime/*.cpp)
    list(APPEND SOURCE_FILES ${_runtime_sources})
    # The crash handler (loader/src/crashhandler/): the crash window, the report's analysis and its analyzers
    file(GLOB_RECURSE _crashhandler_sources CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/loader/src/crashhandler/*.cpp)
    list(APPEND SOURCE_FILES ${_crashhandler_sources})
    # The first-party mods (mods/): embedded in the DLL, written to the mods folder at startup (runtime/firstparty.h)
    file(GLOB_RECURSE _firstparty_files CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/mods/*)
    set(_firstparty_cpp ${CMAKE_BINARY_DIR}/generated/firstparty_mods.cpp)   # Its command: cmake/generation/EmbedModsCommand.cmake
endif()
if(NOT MIRAGE_GAME)   # Keep the IDE tree to what is built
    list(FILTER HEADER_FILES EXCLUDE REGEX "/src/(tas|game|runtime|crashhandler)/")
endif()

if(MIRAGE_BARE)    # Staged build before the menu: hooks.cpp + loader/src/minimal/ (its own DllMain and gui::)
    set(SOURCE_FILES ${PROJECT_SOURCE_DIR}/loader/src/hooks.cpp ${PROJECT_SOURCE_DIR}/loader/src/core/hooking.cpp
            ${PROJECT_SOURCE_DIR}/loader/src/core/clock.cpp
            ${PROJECT_SOURCE_DIR}/loader/src/minimal/minimal.cpp)
    set(HEADER_FILES ${PROJECT_SOURCE_DIR}/loader/src/hooks.h ${PROJECT_SOURCE_DIR}/loader/src/core/hooking.h ${PROJECT_SOURCE_DIR}/loader/src/gui.h ${PROJECT_SOURCE_DIR}/loader/src/paths.h)
    if(MIRAGE_STAGE GREATER_EQUAL MIRAGE_STAGE_LOGGING)
        list(APPEND SOURCE_FILES ${PROJECT_SOURCE_DIR}/loader/src/log.cpp ${PROJECT_SOURCE_DIR}/loader/src/core/compressed_file.cpp)
        list(APPEND HEADER_FILES ${PROJECT_SOURCE_DIR}/loader/src/log.h ${PROJECT_SOURCE_DIR}/loader/src/core/compressed_file.h)
    endif()
    if(MIRAGE_STAGE GREATER_EQUAL MIRAGE_STAGE_THEMES)
        list(APPEND SOURCE_FILES ${PROJECT_SOURCE_DIR}/loader/src/theme.cpp)
        list(APPEND HEADER_FILES ${PROJECT_SOURCE_DIR}/loader/src/theme.h)
    endif()
else()
    list(FILTER SOURCE_FILES EXCLUDE REGEX "/src/minimal/")
endif()
