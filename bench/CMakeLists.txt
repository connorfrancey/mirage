# Benchmarks (Google Benchmark) for the code that runs without the game: replay recording, hashing, saving and loading,
# macros, math and mesh building. Run with `uv run mirage bench` (a release build: debug timings mean little).

add_executable(mirage_bench
        bench_main.cpp
        bench_render.cpp
        bench_hash.cpp
        ${CMAKE_SOURCE_DIR}/loader/src/render/mesh.cpp)
target_include_directories(mirage_bench PRIVATE ${CMAKE_SOURCE_DIR}/loader/src)
target_compile_definitions(mirage_bench PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
        MIRAGE_EXAMPLES_DIR="${CMAKE_SOURCE_DIR}/examples")
target_link_libraries(mirage_bench PRIVATE benchmark::benchmark nlohmann_json::nlohmann_json magic_enum::magic_enum unordered_dense::unordered_dense xxhash::xxhash)
set_target_properties(mirage_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
if(MSVC)
  target_compile_options(mirage_bench PRIVATE /W4 /permissive- /utf-8)
endif()

# Texture replacement's encoder needs UnrealLib (its package model): only when the build has it
if(TARGET UnrealLib::UnrealLib)
  target_sources(mirage_bench PRIVATE bench_textures.cpp
          ${CMAKE_SOURCE_DIR}/loader/src/runtime/texture_files.cpp
          ${CMAKE_SOURCE_DIR}/loader/src/runtime/stb_impl.cpp)
  target_compile_definitions(mirage_bench PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(mirage_bench PRIVATE UnrealLib::UnrealLib stb_headers)
endif()
