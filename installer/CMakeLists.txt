# mirage_installer: a single exe with the loader (dinput8.dll) and the menu (Mirage.dll) embedded, which installs
# them into the game's Binaries folder. This is the file to hand to players.

# The payload: the built DLLs as RCDATA resources. The template is generated (so it can name the actual output files);
# the .rc that gets compiled is copied from it by a custom command that depends on both DLLs. That is what makes Ninja
# wait for them to be linked (and re-embed them whenever they change) before the resource compiler runs.
set(MIRAGE_PAYLOAD_TEMPLATE ${CMAKE_CURRENT_BINARY_DIR}/payload_$<CONFIG>.rc.in)
set(MIRAGE_PAYLOAD_RC ${CMAKE_CURRENT_BINARY_DIR}/payload_$<CONFIG>.rc)
file(GENERATE OUTPUT ${MIRAGE_PAYLOAD_TEMPLATE} CONTENT
"#include \"${CMAKE_CURRENT_SOURCE_DIR}/src/resource.h\"
IDR_LOADER_DLL RCDATA \"$<TARGET_FILE:mirage_loader>\"
IDR_MENU_DLL   RCDATA \"$<TARGET_FILE:Mirage>\"
")
add_custom_command(
        OUTPUT ${MIRAGE_PAYLOAD_RC}
        COMMAND ${CMAKE_COMMAND} -E copy ${MIRAGE_PAYLOAD_TEMPLATE} ${MIRAGE_PAYLOAD_RC}
        DEPENDS ${MIRAGE_PAYLOAD_TEMPLATE} mirage_loader Mirage
        COMMENT "Embedding dinput8.dll and Mirage.dll into the installer"
        VERBATIM)

add_executable(mirage_installer WIN32
        src/main.cpp src/install.cpp src/install.h src/resource.h
        ${CMAKE_SOURCE_DIR}/launcher/src/detect.cpp ${CMAKE_SOURCE_DIR}/launcher/src/detect.h
        ${MIRAGE_PAYLOAD_RC})
target_include_directories(mirage_installer PRIVATE ${CMAKE_SOURCE_DIR}/launcher/src ${CMAKE_SOURCE_DIR}/loader/src)
target_compile_definitions(mirage_installer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        MIRAGE_VERSION="${MIRAGE_VERSION}")
target_link_libraries(mirage_installer PRIVATE comctl32 comdlg32 shell32 advapi32)

add_dependencies(mirage_installer mirage_loader Mirage)

set_target_properties(mirage_installer PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if(MSVC)
  target_compile_options(mirage_installer PRIVATE /W4 /permissive- /utf-8)
  # The game lives in Program Files: writing there needs admin rights, so ask for them up front.
  target_link_options(mirage_installer PRIVATE "/MANIFESTUAC:level='requireAdministrator' uiAccess='false'")
else()
  target_compile_options(mirage_installer PRIVATE -Wall -Wextra)
  target_link_options(mirage_installer PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
