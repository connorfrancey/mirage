# ======================================================================================================================
# Settings (before the dependencies, so they inherit them)
# ======================================================================================================================

# C++
set(CMAKE_CXX_STANDARD 23)   # glaze needs C++23
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# CMake 4 dropped support for cmake_minimum_required < 3.5; this lets older dependencies configure
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
