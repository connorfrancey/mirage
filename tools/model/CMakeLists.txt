# mirage_model: reads a 3D model (glTF, FBX, OBJ) with assimp and reports what the game would get from it: meshes,
# vertices, triangles, UV channels, materials and their textures, the size, and what the game's static meshes can't
# take. A tool run at development time (`mirage model <file>`), never loaded into the game: assimp stays out of
# Mirage.dll and the game's 32-bit address space. Writing the game's packages from it is the plan's step 31.
#
# Its own CMake project (built in cmake-build-x86-model by the CLI): assimp builds its own zlib, whose target name
# would clash with the zlib UnrealLib brings into the main build.
cmake_minimum_required(VERSION 3.24)
project(mirage_model LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/CPM.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/Versions.cmake)

# assimp: only the importers mods need (glTF, FBX, OBJ), no exporters, tools or tests
CPMAddPackage(
        NAME assimp
        GITHUB_REPOSITORY assimp/assimp
        GIT_TAG ${ASSIMP_VERSION}
        OPTIONS "BUILD_SHARED_LIBS OFF" "ASSIMP_BUILD_TESTS OFF" "ASSIMP_BUILD_ASSIMP_TOOLS OFF" "ASSIMP_BUILD_SAMPLES OFF"
                "ASSIMP_INSTALL OFF" "ASSIMP_NO_EXPORT ON" "ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT OFF"
                "ASSIMP_BUILD_GLTF_IMPORTER ON" "ASSIMP_BUILD_FBX_IMPORTER ON" "ASSIMP_BUILD_OBJ_IMPORTER ON"
                "ASSIMP_WARNINGS_AS_ERRORS OFF" "ASSIMP_BUILD_ZLIB ON" "ASSIMP_INJECT_DEBUG_POSTFIX OFF"
)

add_executable(mirage_model src/main.cpp src/model_report.cpp)
target_link_libraries(mirage_model PRIVATE assimp::assimp)
if(MSVC)
  target_compile_options(mirage_model PRIVATE /W4 /permissive- /utf-8)
  target_link_options(mirage_model PRIVATE /LARGEADDRESSAWARE)   # A 32-bit tool: big models get 4 GB on 64-bit Windows
endif()

# Models read back: `ctest` in this build folder. An OBJ cube (12 triangles, one UV channel) and a glTF triangle with
# no UVs, which the report must name as a problem (and --strict fail on)
enable_testing()
add_test(NAME ModelCubeObj COMMAND mirage_model --json ${CMAKE_CURRENT_LIST_DIR}/test/cube.obj)
set_tests_properties(ModelCubeObj PROPERTIES PASS_REGULAR_EXPRESSION "\"triangles\": 12,")
add_test(NAME ModelTriangleGltf COMMAND mirage_model --json ${CMAKE_CURRENT_LIST_DIR}/test/triangle.gltf)
set_tests_properties(ModelTriangleGltf PROPERTIES PASS_REGULAR_EXPRESSION "\"triangles\": 1,.*has no UVs")
add_test(NAME ModelTriangleGltfStrict COMMAND mirage_model --strict ${CMAKE_CURRENT_LIST_DIR}/test/triangle.gltf)
set_tests_properties(ModelTriangleGltfStrict PROPERTIES WILL_FAIL TRUE)
