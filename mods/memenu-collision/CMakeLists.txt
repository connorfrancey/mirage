# Collision View: a native first-party mod (cmake/FirstPartyMods.cmake), built against api/ and the public SDK only.
# render/math.h, geometry.h and mesh.* are its own copies of the runtime's pure geometry code.
mirage_native_mod(memenu-collision SOURCES collision_view.cpp debug_world.cpp static_world.cpp render/mesh.cpp
                  render/static_scene.cpp LIBS nlohmann_json::nlohmann_json mirage::sdk fmt::fmt unordered_dense)
