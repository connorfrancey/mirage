# medge launcher: starts (or waits for) Mirror's Edge and injects Mirage.dll into that
# Must be 32-bit like the game and the DLL (the root CMakeLists already enforces that).

# Finding files (e.g. file(GLOB_RECUSE ...)) isn't necessary for this, so just add them manually
add_executable(mirage_launcher src/main.cpp src/inject.cpp src/inject.h src/detect.cpp src/detect.h)

target_link_libraries(mirage_launcher PRIVATE glaze::glaze shell32 advapi32)
target_compile_definitions(mirage_launcher PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

if (MSVC)
    target_compile_options(mirage_launcher PRIVATE /W4 /permissive- /utf-8)
    # Always run as administrator (UAC prompt), like the installer: injecting into the game needs the rights to open
    # its process, which a normal launcher does not have when the game itself runs elevated.
    target_link_options(mirage_launcher PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
else()
    target_compile_options(mirage_launcher PRIVATE -Wall -Wextra)
    target_link_options(mirage_launcher PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()

# Build the DLL first and keep both side by side: the launcher looks for Mirage.dll next to itself.
set_target_properties(mirage_launcher PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
add_dependencies(mirage_launcher Mirage)
add_custom_command(TARGET mirage_launcher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:Mirage> $<TARGET_FILE_DIR:mirage_launcher>
    COMMENT "Copying Mirage.dll next to the launcher")