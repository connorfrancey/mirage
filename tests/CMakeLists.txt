# Unit tests (GoogleTest) for the code that runs without the game: math, mesh building, TAS replays and macros, ...
# Run with `uv run mirage test` or `ctest --test-dir cmake-build-x86-debug --output-on-failure`.

add_executable(mirage_tests
        test_math.cpp
        test_float_util.cpp
        test_cache_system.cpp
        test_mesh.cpp
        test_geometry.cpp
        test_static_scene.cpp
        test_settings.cpp
        test_keys.cpp
        test_cache.cpp
        test_cache2.cpp
        test_shell.cpp
        test_semver.cpp
        test_manifest.cpp
        test_touch.cpp
        test_abi.cpp
        test_resolve.cpp
        test_mod_profile.cpp
        test_zep.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/mod_profile.cpp
        test_detours.cpp
        test_crashhandler.cpp
        test_symbols.cpp
        test_memory.cpp
        # The memory manager's parts without the game (runtime/mem): the test exe keeps the CRT's own operator new
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/mem/analysis.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/mem/pool.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/mem/arena.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/mem/heap.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/mem/sampler.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/mem/process.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/mem/allocators.cpp
        test_caches_more.cpp
        test_infra.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/events.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/tasks.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/resource_cache.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/config_cache.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/detours.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/detour_guard.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/symbols.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/hooking.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/crashhandler/analysis_text.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/crashhandler/memory_text.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/crashhandler/analyzer/builtin.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/resolve.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/semver.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/manifest.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/touch.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/render/mesh.cpp
        ${CMAKE_SOURCE_DIR}/mods/mirage-collision/render/static_scene.cpp   # A native first-party mod's pure part
        # The settings framework and what it needs (test_settings.cpp): no game, no window
        ${CMAKE_SOURCE_DIR}/loader/src/core/setting.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/module.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/notify.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/keybinds.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/bindings.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/keys.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/scheduler.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/clock.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/cache.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/cache_evict.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/cache_sched.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/memory_cache.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/signature_cache.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/compressed_file.cpp     # logs/.old and crashlogs/.old (test_infra.cpp)
        ${CMAKE_SOURCE_DIR}/loader/src/core/address_cache.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/string_cache.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/hardware.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/core/shell.cpp)
target_include_directories(mirage_tests PRIVATE ${CMAKE_SOURCE_DIR}/loader/src ${CMAKE_SOURCE_DIR}/loader/include ${CMAKE_SOURCE_DIR}/mods
        ${CMAKE_BINARY_DIR}/generated)   # symbols_toml.inc
target_compile_definitions(mirage_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
        MIRAGE_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples")
target_link_libraries(mirage_tests PRIVATE GTest::gtest_main zstd_compress nlohmann_json::nlohmann_json imgui zep spdlog::spdlog toml11::toml11 MirageHook::MirageHook Zydis magic_enum::magic_enum unordered_dense::unordered_dense xxhash::xxhash FastFloat::fast_float)
if(MSVC)
  target_compile_options(mirage_tests PRIVATE /W4 /permissive- /utf-8)
endif()

# One CTest test per TEST(); listed when ctest runs (not at build time, so a build never runs the exe)
include(GoogleTest)
gtest_discover_tests(mirage_tests DISCOVERY_MODE PRE_TEST)

# docs/api-reference.md is generated from loader/include/mirage/api.h (`mirage docs api`): out of date fails here
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME ApiReference.UpToDate COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/gen_api_docs.py --check)
endif()

# The installer's work on a game folder, on a fake one (tests/installer/test_install.cpp): stand-in DLLs embedded the
# way the real installer embeds the built ones, as RCDATA resources (written with absolute paths for the resource
# compiler).
set(MIRAGE_TEST_PAYLOAD_RC ${CMAKE_CURRENT_BINARY_DIR}/installer_payload.rc)
file(WRITE ${MIRAGE_TEST_PAYLOAD_RC}
"#include \"${CMAKE_SOURCE_DIR}/installer/src/resource.h\"
IDR_LOADER_DLL RCDATA \"${CMAKE_CURRENT_SOURCE_DIR}/installer/fake_loader.bin\"
IDR_MENU_DLL   RCDATA \"${CMAKE_CURRENT_SOURCE_DIR}/installer/fake_menu.bin\"
")
add_executable(installer_tests
        installer/test_install.cpp
        ${CMAKE_SOURCE_DIR}/installer/src/install.cpp
        ${CMAKE_SOURCE_DIR}/launcher/src/detect.cpp
        ${MIRAGE_TEST_PAYLOAD_RC})
target_include_directories(installer_tests PRIVATE ${CMAKE_SOURCE_DIR}/installer/src ${CMAKE_SOURCE_DIR}/launcher/src)
target_compile_definitions(installer_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE MIRAGE_VERSION="test"
        MIRAGE_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/installer")
target_link_libraries(installer_tests PRIVATE GTest::gtest_main shell32 advapi32)
if(MSVC)
  target_compile_options(installer_tests PRIVATE /W4 /permissive- /utf-8)
endif()
gtest_discover_tests(installer_tests DISCOVERY_MODE PRE_TEST)

# UnrealLib (cmake/Libraries.cmake): its own exe because the library is C++20 and mirage_tests builds as C++17.
# Reading the game's packages uses the launcher's install detection (or UNREALLIB_MIRRORSEDGE_DIR); skipped without it.
if(MIRAGE_UNREALLIB)
  add_executable(unreallib_tests
          test_unreallib.cpp
          test_textures.cpp
          ${CMAKE_SOURCE_DIR}/loader/src/runtime/texture_files.cpp
          ${CMAKE_SOURCE_DIR}/loader/src/runtime/stb_impl.cpp
          ${CMAKE_SOURCE_DIR}/launcher/src/detect.cpp)
  target_include_directories(unreallib_tests PRIVATE ${CMAKE_SOURCE_DIR}/launcher/src ${CMAKE_SOURCE_DIR}/loader/src)
  target_compile_definitions(unreallib_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(unreallib_tests PRIVATE GTest::gtest_main UnrealLib::UnrealLib stb_headers shell32 advapi32)
  if(MSVC)
    target_compile_options(unreallib_tests PRIVATE /W4 /permissive- /utf-8)
  endif()
  gtest_discover_tests(unreallib_tests DISCOVERY_MODE PRE_TEST)

  # Status check (`mirage unreallib`): one check per module against the game's packages, logged with spdlog
  add_executable(unreallib_status
          unreallib_status.cpp
          ${CMAKE_SOURCE_DIR}/launcher/src/detect.cpp)
  target_include_directories(unreallib_status PRIVATE ${CMAKE_SOURCE_DIR}/launcher/src)
  target_compile_definitions(unreallib_status PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(unreallib_status PRIVATE UnrealLib::UnrealLib spdlog::spdlog shell32 advapi32)
  set_target_properties(unreallib_status PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})   # Where mirage looks
  if(MSVC)
    target_compile_options(unreallib_status PRIVATE /W4 /permissive- /utf-8)
  endif()
endif()
