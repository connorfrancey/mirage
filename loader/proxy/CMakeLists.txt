# mirage loader: a dinput8.dll proxy that sits next to MirrorsEdge.exe and loads mirage\Mirage.dll with the game.
# 32-bit like the game (the root CMakeLists enforces that). Installed by mirage_installer (installer/).

add_library(mirage_loader SHARED src/loader.cpp src/dinput8.def)
set_target_properties(mirage_loader PROPERTIES OUTPUT_NAME dinput8 PREFIX "")
target_compile_definitions(mirage_loader PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX MIRAGE_VERSION="${MIRAGE_VERSION}")

if(MSVC)
  target_compile_options(mirage_loader PRIVATE /W4 /permissive- /utf-8)
else()
  target_compile_options(mirage_loader PRIVATE -Wall -Wextra)
  target_link_options(mirage_loader PRIVATE -static -static-libgcc -static-libstdc++)
endif()
