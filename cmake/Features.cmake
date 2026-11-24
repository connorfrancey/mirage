# Features: which modules go into Mirage.dll
#
# Every module under loader/src/features/ has its own option, MIRAGE_FEATURE_<NAME>, grouped by category
# (MIRAGE_FEATURES_<CATEGORY>) under one master switch (MIRAGE_FEATURES). A feature is built when all three are ON;
# turning a category or the master switch off forces its features off without losing their own setting.
#
#   cmake ... -D MIRAGE_FEATURES=OFF                 just the menu GUI: no modules, no game integration
#   cmake ... -D MIRAGE_FEATURES_DEBUG=OFF           everything except the Debug tab
#   cmake ... -D MIRAGE_FEATURE_NO_CLIP=OFF          everything except No Clip
#
# Features marked GAME need the game integration (loader/src/game, loader/src/render, the engine tick hook and the Mirror's Edge
# SDK). When none of the enabled features needs it, that layer is left out too (MIRAGE_GAME, set below), and the DLL
# is only the D3D9 hook, the menu, themes and the config file.
#
# Staged build (MIRAGE_MINIMAL, `mirage minimal --stage N`): the feature options are ignored and the build is
# everything up to stage MIRAGE_MINIMAL_STAGE, in the order below: least likely to break the game first. Stages 0-2
# are the bare hook (loader/src/minimal/ instead of the menu), 3 is the real menu with no features, and every feature's
# STAGE says when it comes back. The last stage is the whole trainer, every feature on (TAS included).
#
# Sets, for CMakeLists.txt and cmake/Libraries.cmake:
#   MIRAGE_FEATURE_SOURCES   .cpp files of the enabled features (and the game layer when it is needed)
#   MIRAGE_ENABLED_FEATURES  their names
#   MIRAGE_GAME              ON when an enabled feature needs the game layer (compile definition MIRAGE_GAME=0/1)
#   MIRAGE_BARE              ON for the staged build's stages 0-2: hooks.cpp + loader/src/minimal/, no menu
#   MIRAGE_STAGE             the staged build's stage (-1 when not staged)

include(CMakeDependentOption)

# -------------------------------------------------------------------------------------------------
# Stages of the staged build, in order. mirage_stage(<n> "<what it adds>"): `mirage minimal` shows these.
set(MIRAGE_STAGE_LOGGING 1)
set(MIRAGE_STAGE_THEMES 2)
set(MIRAGE_STAGE_CORE 3)
set(MIRAGE_STAGES "")
function(mirage_stage n description)
  list(APPEND MIRAGE_STAGES ${n})
  set(MIRAGE_STAGES "${MIRAGE_STAGES}" PARENT_SCOPE)
endfunction()

mirage_stage(0  "D3D9 hook + ImGui demo window (INSERT)")
mirage_stage(1  "logging: spdlog, mirage.log")
mirage_stage(2  "themes: toml11, the theme files (on the demo window)")
mirage_stage(3  "the real menu: module system, config, keybinds, menu GUI; no features")
mirage_stage(4  "menu-only features that never touch the game, and the not-implemented stubs")
mirage_stage(5  "HUD overlays drawn every frame")
mirage_stage(6  "game integration: SDK, pattern scans, engine tick hook, StretchRect + input blocking; Game Status (read-only)")
mirage_stage(7  "reads the player every frame: Speedometer")
mirage_stage(8  "level actions: loading levels and streaming sublevels")
mirage_stage(9  "writes to the player and the world: God Mode, Game Speed, Save State")
mirage_stage(10 "takes over movement: Flight, No Clip and the trainer tools on top of them")
mirage_stage(11 "Collision View: debug renderer, world geometry, drawing inside the scene")
mirage_stage(12 "TAS / replay: virtual clock, input playback. Everything is on")
list(GET MIRAGE_STAGES -1 MIRAGE_LAST_STAGE)

if(MIRAGE_MINIMAL)
  set(MIRAGE_STAGE ${MIRAGE_MINIMAL_STAGE})
  if(NOT MIRAGE_STAGE MATCHES "^[0-9]+$" OR MIRAGE_STAGE GREATER MIRAGE_LAST_STAGE)
    message(FATAL_ERROR "MIRAGE_MINIMAL_STAGE must be 0-${MIRAGE_LAST_STAGE} (got '${MIRAGE_STAGE}')")
  endif()
  if(MIRAGE_STAGE LESS MIRAGE_STAGE_CORE)
    set(MIRAGE_BARE ON)
  else()
    set(MIRAGE_BARE OFF)
  endif()
else()
  set(MIRAGE_STAGE -1)
  set(MIRAGE_BARE OFF)
endif()

# -------------------------------------------------------------------------------------------------
# Options (not used by the staged build)
if(NOT MIRAGE_MINIMAL)
  option(MIRAGE_FEATURES "Build the menu's features (OFF: only the menu GUI, no in-game functionality)" ON)
  set(MIRAGE_FEATURE_CATEGORIES player trainer tas world visual debug levels misc)
  foreach(category IN LISTS MIRAGE_FEATURE_CATEGORIES)
    string(TOUPPER "${category}" CAT)
    cmake_dependent_option(MIRAGE_FEATURES_${CAT} "Build the ${category} features" ON "MIRAGE_FEATURES" OFF)
  endforeach()
endif()

set(MIRAGE_FEATURE_SOURCES "")
set(MIRAGE_ALL_FEATURE_SOURCES "")   # Every file any feature lists, enabled or not (CMakeLists.txt checks against it)
set(MIRAGE_GAME OFF)
set(MIRAGE_ENABLED_FEATURES "")
set(MIRAGE_DISABLED_FEATURES "")

# mirage_feature(<name> <category> [GAME] [OFF] STAGE <n> FILES <files relative to src/>...)
#   GAME   the feature needs the game integration
#   OFF    the feature defaults to off (normal build)
#   STAGE  when the staged build adds it back (see mirage_stage above): rank new features by how likely they are
#          to break the game
function(mirage_feature name category)
  cmake_parse_arguments(PARSE_ARGV 2 F "GAME;OFF" "STAGE" "FILES")
  string(TOUPPER "${name}" NAME)
  string(TOUPPER "${category}" CAT)
  if(NOT DEFINED F_STAGE OR NOT F_STAGE IN_LIST MIRAGE_STAGES OR F_STAGE LESS MIRAGE_STAGE_CORE)
    message(FATAL_ERROR "mirage_feature(${name}): needs STAGE <n>, one of the mirage_stage() numbers from "
                        "${MIRAGE_STAGE_CORE} up (cmake/Features.cmake)")
  endif()

  if(MIRAGE_MINIMAL)
    if(F_STAGE LESS_EQUAL MIRAGE_STAGE)
      set(enabled ON)
    else()
      set(enabled OFF)
    endif()
  else()
    set(default ON)
    if(F_OFF)
      set(default OFF)
    endif()
    cmake_dependent_option(MIRAGE_FEATURE_${NAME} "Build the ${name} feature (${category})" ${default}
      "MIRAGE_FEATURES;MIRAGE_FEATURES_${CAT}" OFF)
    set(enabled ${MIRAGE_FEATURE_${NAME}})
  endif()

  foreach(file IN LISTS F_FILES)
    list(APPEND MIRAGE_ALL_FEATURE_SOURCES "${PROJECT_SOURCE_DIR}/loader/src/${file}")
  endforeach()
  set(MIRAGE_ALL_FEATURE_SOURCES "${MIRAGE_ALL_FEATURE_SOURCES}" PARENT_SCOPE)

  if(enabled)
    foreach(file IN LISTS F_FILES)
      list(APPEND MIRAGE_FEATURE_SOURCES "${PROJECT_SOURCE_DIR}/loader/src/${file}")
    endforeach()
    list(REMOVE_DUPLICATES MIRAGE_FEATURE_SOURCES)   # Helpers shared by several features (fly_module.cpp)
    list(APPEND MIRAGE_ENABLED_FEATURES ${name})
    if(F_GAME)
      set(MIRAGE_GAME ON PARENT_SCOPE)
    endif()
  else()
    list(APPEND MIRAGE_DISABLED_FEATURES ${name})
  endif()
  set(MIRAGE_FEATURE_SOURCES "${MIRAGE_FEATURE_SOURCES}" PARENT_SCOPE)
  set(MIRAGE_ENABLED_FEATURES "${MIRAGE_ENABLED_FEATURES}" PARENT_SCOPE)
  set(MIRAGE_DISABLED_FEATURES "${MIRAGE_DISABLED_FEATURES}" PARENT_SCOPE)
endfunction()

# -------------------------------------------------------------------------------------------------
# Player

# Trainer

# TAS / replay
mirage_feature(input             tas GAME STAGE 12 FILES tas/engine.cpp tas/input_monitor.cpp)

# World

# Visual

# Debug
mirage_feature(crash_reporter    debug GAME STAGE 5  FILES features/debug/crash_reporter.cpp)
mirage_feature(self_test         debug      STAGE 4  FILES features/debug/self_test.cpp)   # Shown in developer mode only
mirage_feature(memory_profiler   debug GAME STAGE 6  FILES features/debug/memory_profiler.cpp)

# Levels

# Misc
mirage_feature(mod_manager       misc GAME STAGE 9 FILES features/misc/mod_manager.cpp)
mirage_feature(console           misc GAME STAGE 9 FILES features/misc/console.cpp)
mirage_feature(kismet_editor     misc GAME STAGE 9 FILES features/misc/kismet_editor.cpp)
mirage_feature(script_editor     misc GAME STAGE 9 FILES features/misc/script_editor.cpp)
mirage_feature(level_editor      misc GAME STAGE 9 FILES features/misc/level_editor.cpp)
mirage_feature(animation_browser misc GAME STAGE 9 FILES features/misc/animation_browser.cpp)
mirage_feature(object_inspector  misc GAME STAGE 9 FILES features/misc/object_inspector.cpp)

# -------------------------------------------------------------------------------------------------
# The game integration layer, only when something uses it
if(MIRAGE_GAME)
  file(GLOB MIRAGE_GAME_SOURCES CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/loader/src/game/*.cpp
    ${PROJECT_SOURCE_DIR}/loader/src/render/*.cpp)
  list(APPEND MIRAGE_FEATURE_SOURCES ${MIRAGE_GAME_SOURCES})
endif()

list(LENGTH MIRAGE_ENABLED_FEATURES _n_on)
list(LENGTH MIRAGE_DISABLED_FEATURES _n_off)
if(MIRAGE_MINIMAL)
  message(STATUS "mirage: staged build, stage ${MIRAGE_STAGE} of ${MIRAGE_LAST_STAGE}")
endif()
message(STATUS "mirage: ${_n_on} feature(s) enabled, ${_n_off} disabled, game integration ${MIRAGE_GAME}")
if(MIRAGE_DISABLED_FEATURES)
  string(REPLACE ";" ", " _off "${MIRAGE_DISABLED_FEATURES}")
  message(STATUS "mirage: disabled: ${_off}")
endif()
