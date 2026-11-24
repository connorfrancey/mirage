# ======================================================================================================================
# Platform
# ======================================================================================================================

# Mirror's Edge is a 32-bit Windows process
if(NOT WIN32)
    message(FATAL_ERROR "${PROJECT_NAME} is Windows-only.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Mirror's Edge is 32-bit. Use an x86 toolchain/preset (cmake -A Win32 / x86-debug).")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>") # static CRT
