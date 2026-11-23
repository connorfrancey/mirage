# Rendering and in-game UI

The live backend is Direct3D 9 with ImGui Win32/DX9. `hooks.cpp` owns EndScene, Reset, window/input integration, frame dispatch, and device-loss callbacks. `gui.cpp` presents the module UI and controls. Render work and ImGui go through the render thread. [Hooks](../loader/src/hooks.cpp), [GUI](../loader/src/gui.cpp), [thread ownership](architecture.md)

- `debug_renderer` consumes snapshots of world geometry. Snapshot construction can happen off the render thread, while engine reads obey game-thread ownership. Reuse existing depth and overlay paths. [Debug renderer](../loader/src/render/debug_renderer.h), [mesh helpers](../loader/src/render/mesh.h)
- `postfx` draws ordered fullscreen D3D9 effects in EndScene before the menu. Follow its state preservation and reset hooks when adding device resources. [Post effects](../loader/src/render/postfx.h)
- The `MirageUi` API supplies stable rendering controls to mods. Native mods using the ImGui wrapper depend on context and version compatibility. [Public API](../loader/include/mirage/api.h), [ImGui wrapper](../loader/include/mirage/imgui.hpp)

OpenGL references in the asset pipeline describe source normal-map conventions. The inspected runtime and test-host targets link D3D9. [Texture conventions](../loader/src/runtime/texture_files.h), [linked dependencies](../cmake/Libraries.cmake), [test-host build](../tools/testhost/CMakeLists.txt)
