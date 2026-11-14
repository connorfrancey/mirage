# mirage_testhost: loads Mirage.dll into a small D3D9 app (no game needed) and drives it the way the game would.
# Used by `mirage verbose-build` to test every stage of the staged build; see src/main.cpp for what it does.
add_executable(mirage_testhost src/main.cpp)
target_link_libraries(mirage_testhost PRIVATE d3d9 dbghelp)
target_compile_definitions(mirage_testhost PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
if(MSVC)
  target_compile_options(mirage_testhost PRIVATE /W4 /permissive- /utf-8)
else()
  target_link_options(mirage_testhost PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
# Next to the DLL in the build folder (mirage_tools looks for it there)
set_target_properties(mirage_testhost PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
