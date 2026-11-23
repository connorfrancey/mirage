# ======================================================================================================================
# Compiler Options
# ======================================================================================================================

# The settings every Mirage target compiles with (the DLL and each mirage_library): warnings, the source include root,
# and the definitions the code tests (MIRAGE_GAME, MIRAGE_FEATURE_<NAME>, ...). One function, so a library's objects are
# built exactly like the DLL's own and the two can't drift apart.
function(mirage_target_settings target)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/loader/src)
    if(MIRAGE_GAME)   # mirage/api.h (the mod ABI) and what cmake/generation/ writes (symbols_toml.inc)
        target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/loader/include ${CMAKE_BINARY_DIR}/generated)
    endif()
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
            MIRAGE_GAME=$<BOOL:${MIRAGE_GAME}>
            MIRAGE_OPTIMIZATIONS=$<BOOL:${MIRAGE_OPTIMIZATIONS}>
            MIRAGE_DEV_MODE=$<BOOL:${MIRAGE_DEV_MODE}>
            MIRAGE_TRACY=$<AND:$<BOOL:${MIRAGE_TRACY}>,$<NOT:$<BOOL:${MIRAGE_BARE}>>>   # Staged stages 0-2 never link it
            MIRAGE_MINIMAL=$<BOOL:${MIRAGE_BARE}>     # The code's "minimal": no menu, loader/src/minimal/ instead (stages 0-2)
            MIRAGE_MINIMAL_STAGE=$<IF:$<BOOL:${MIRAGE_BARE}>,${MIRAGE_STAGE},0>)
    foreach(feature IN LISTS MIRAGE_ENABLED_FEATURES)    # MIRAGE_FEATURE_<NAME>=0/1 in the code
        string(TOUPPER "${feature}" _name)
        target_compile_definitions(${target} PRIVATE MIRAGE_FEATURE_${_name}=1)
    endforeach()
    foreach(feature IN LISTS MIRAGE_DISABLED_FEATURES)
        string(TOUPPER "${feature}" _name)
        target_compile_definitions(${target} PRIVATE MIRAGE_FEATURE_${_name}=0)
    endforeach()

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        # Frame pointers kept in optimized builds too (x86 /O2 drops them): the crash window's and the memory manager's
        # call stacks walk them, so a release crash report has its real call stack. About 1% of speed.
        target_compile_options(${target} PRIVATE $<$<NOT:$<CONFIG:Debug>>:/Oy->)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

mirage_target_settings(${PROJECT_NAME})
if(NOT MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE -static -static-libgcc -static-libstdc++)
endif()
