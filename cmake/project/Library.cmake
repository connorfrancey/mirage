# ======================================================================================================================
# Mirage Libraries
# ======================================================================================================================

# mirage_library(): one part of the loader built as its own library (MirageCore, MirageUI, MirageScript, ...) and linked
# into Mirage.dll. A library owns a folder of loader/src/; its files leave the DLL's own source list, so each file is
# compiled once, by its library.
#
#   mirage_library(MirageCore
#           FOLDERS core util                    # loader/src/<folder>/, globbed recursively (.c .cpp .h .hpp)
#           SOURCES loader/src/log.cpp           # Single files outside those folders (relative to the repository)
#           DEPENDS MirageCore                   # Other Mirage libraries it uses (by target name)
#           LIBS spdlog::spdlog glaze::glaze)    # Third-party targets it uses (from cmake/Libraries.cmake)
#
#   Target MirageCore, alias Mirage::Core. Code includes as it does now ("core/hash.h"): the include root stays loader/src.
#
# OBJECT libraries, not static ones: modules register themselves from static initializers (MEM_MODULE), and the linker
# leaves out every object file of a static .lib that nothing references by name, so a module in a static library would
# silently never register. An object library's objects all go into the DLL.
#
# Each library compiles with mirage_target_settings (the DLL's warnings, definitions and feature switches), so moving a
# file into a library changes nothing about how it is built. Call it after cmake/project/Target.cmake (the DLL must
# exist to give up the files) and before nothing else: third-party targets are resolved when CMake generates.

function(mirage_library name)
    cmake_parse_arguments(PARSE_ARGV 1 L "" "" "FOLDERS;SOURCES;DEPENDS;LIBS")
    if(L_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "mirage_library(${name}): unknown arguments ${L_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT name MATCHES "^Mirage[A-Z][A-Za-z0-9]*$")
        message(FATAL_ERROR "mirage_library(${name}): the name is Mirage<Part> (MirageCore, MirageUI, ...)")
    endif()
    string(REGEX REPLACE "^Mirage" "" _part "${name}")

    set(_files "")
    foreach(folder IN LISTS L_FOLDERS)
        set(_dir ${PROJECT_SOURCE_DIR}/loader/src/${folder})
        if(NOT IS_DIRECTORY ${_dir})
            message(FATAL_ERROR "mirage_library(${name}): no folder loader/src/${folder}")
        endif()
        file(GLOB_RECURSE _found CONFIGURE_DEPENDS ${_dir}/*.c ${_dir}/*.cpp ${_dir}/*.h ${_dir}/*.hpp)
        list(APPEND _files ${_found})
    endforeach()
    foreach(src IN LISTS L_SOURCES)
        cmake_path(ABSOLUTE_PATH src BASE_DIRECTORY ${PROJECT_SOURCE_DIR} NORMALIZE OUTPUT_VARIABLE _abs)
        if(NOT EXISTS ${_abs})
            message(FATAL_ERROR "mirage_library(${name}): no file ${src}")
        endif()
        list(APPEND _files ${_abs})
    endforeach()
    list(REMOVE_DUPLICATES _files)

    # Only what the DLL would build: a feature switched off (cmake/Features.cmake) or a staged build's missing layer
    # stays out of the library too
    get_target_property(_dll_sources ${PROJECT_NAME} SOURCES)
    set(_owned "")
    foreach(file IN LISTS _files)
        if(file IN_LIST _dll_sources OR file MATCHES "\\.(h|hpp)$")
            list(APPEND _owned ${file})
        endif()
    endforeach()
    set(_compiled ${_owned})
    list(FILTER _compiled INCLUDE REGEX "\\.(c|cpp)$")
    if(NOT _compiled)
        message(STATUS "mirage_library(${name}): nothing to build in this configuration")
        return()
    endif()

    # The DLL gives the files up: compiled once, here
    list(REMOVE_ITEM _dll_sources ${_owned})
    set_property(TARGET ${PROJECT_NAME} PROPERTY SOURCES ${_dll_sources})

    add_library(${name} OBJECT ${_owned})
    add_library(Mirage::${_part} ALIAS ${name})
    mirage_target_settings(${name})
    target_link_libraries(${name} PUBLIC ${L_DEPENDS} ${L_LIBS})
    source_group(TREE ${PROJECT_SOURCE_DIR}/loader/src FILES ${_owned})
    set_target_properties(${name} PROPERTIES FOLDER "Mirage")

    target_link_libraries(${PROJECT_NAME} PRIVATE ${name})
    set_property(GLOBAL APPEND PROPERTY MIRAGE_LIBRARIES ${name})   # For the tests and tools that want them all
endfunction()
