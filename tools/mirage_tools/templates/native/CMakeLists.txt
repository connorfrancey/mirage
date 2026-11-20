# {{name}}: a native Mirage mod, its own CMake project. Built against the mod API headers only (MIRAGE_API_DIR), so any
# compiler that targets 32-bit Windows works (MSVC, clang-cl). Its sources are in src/. The built mod (mod.toml,
# {{id}}.dll and content/) goes to
# <build>/mod/{{id}}: `mirage mod install` copies it into the game's mods folder.
cmake_minimum_required(VERSION 3.20)
project({{ident}}_mod LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Mirror's Edge is 32-bit: build the mod for x86 (an x86 environment, or clang-cl -m32)")
endif()

set(MIRAGE_API_DIR "{{api_dir}}" CACHE PATH "Folder holding mirage/api.h")

add_library({{id}} SHARED src/{{id}}.cpp)
target_include_directories({{id}} PRIVATE ${MIRAGE_API_DIR})
target_compile_features({{id}} PRIVATE cxx_std_17)
set_target_properties({{id}} PROPERTIES
        PREFIX ""
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"   # Static CRT: nothing to install next to it
        LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mod/{{id}}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/mod/{{id}})
if(MSVC)
  target_compile_options({{id}} PRIVATE /W4 /utf-8)
endif()
# mod.toml and content/ next to the DLL, on every build
add_custom_target({{ident}}_manifest ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_CURRENT_SOURCE_DIR}/mod.toml ${CMAKE_BINARY_DIR}/mod/{{id}}/mod.toml
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/content ${CMAKE_BINARY_DIR}/mod/{{id}}/content
        DEPENDS {{id}})
