# mirage_sdkgen: writes the Mirage SDK (C++ headers, LuaLS stubs, decompiled script) from the game's packages and the
# engine-only layouts the in-game self-test saved (Mirage, step 3). Built with -DMIRAGE_BUILD_SDKGEN=ON; run with
# `mirage sdk` or `mirage_sdkgen --config tools/sdkgen/sdkgen.toml`.

add_executable(mirage_sdkgen
        main.cpp
        config.cpp
        model.cpp
        emit_cpp.cpp
        emit_lua.cpp
        emit_script.cpp
        compare.cpp
        # The layout code the self-test proves against the engine: the generator uses the same, not a copy
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/computed_layout.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/runtime/layout_json.cpp)

target_include_directories(mirage_sdkgen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/loader/src)
target_link_libraries(mirage_sdkgen PRIVATE
        UnrealLib::Linker UnrealLib::Decompiler fmt::fmt glaze::glaze toml11::toml11)
target_compile_features(mirage_sdkgen PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(mirage_sdkgen PRIVATE /W4 /utf-8 /bigobj)
endif()
set_target_properties(mirage_sdkgen PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The generated SDK, once written (the menu links it from the top-level CMakeLists when it moves off the old SDK):
# building it compiles every layout check, so `cmake --build --target mirage_sdk` proves a fresh generation
if(EXISTS ${CMAKE_SOURCE_DIR}/sdk/CMakeLists.txt AND NOT TARGET mirage_sdk)
  add_subdirectory(${CMAKE_SOURCE_DIR}/sdk ${CMAKE_BINARY_DIR}/sdk)
endif()

# mirage_index: every export of every cooked package as TSV (`mirage index` loads it into SQLite to search)
add_executable(mirage_index index.cpp)
target_link_libraries(mirage_index PRIVATE UnrealLib::Linker)
if(MSVC)
  target_compile_options(mirage_index PRIVATE /W4 /utf-8)
endif()
set_target_properties(mirage_index PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# mirage_assets: the textures, sounds, meshes, animations and materials with their details, and previews of them
# (`mirage assets` builds the catalog and browses it)
add_executable(mirage_assets assets.cpp)
target_link_libraries(mirage_assets PRIVATE UnrealLib::Linker UnrealLib::Assets)
target_compile_features(mirage_assets PRIVATE cxx_std_20)
if(MSVC)
  target_compile_options(mirage_assets PRIVATE /W4 /utf-8)
endif()
set_target_properties(mirage_assets PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
