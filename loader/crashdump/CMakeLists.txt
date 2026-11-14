# mirage_crashdump: writes the game's minidumps from outside it (src/main.cpp says why). 32-bit like the game, so the
# dump has the game's own stacks. Embedded in Mirage.dll as resource 900 (the root CMakeLists), which writes it next to
# itself at startup: nothing more to install.

add_executable(mirage_crashdump src/main.cpp)
target_compile_definitions(mirage_crashdump PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(mirage_crashdump PRIVATE dbghelp)
# A known place: the resource that embeds it names the file
set_target_properties(mirage_crashdump PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/crashdump)

if(MSVC)
  target_compile_options(mirage_crashdump PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(mirage_crashdump PRIVATE -Wall -Wextra)
  target_link_options(mirage_crashdump PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
