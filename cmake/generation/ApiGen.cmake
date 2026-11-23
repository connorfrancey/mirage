# Optional MirageAPIGen registrations. Python is only used while building.
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
file(GLOB _apigen_python CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/mirage_apigen/*.py")
set(_apigen_dir "${CMAKE_BINARY_DIR}/generated/apigen")
set(_apigen_cpp "${_apigen_dir}/cpp/package-ALAudio.cpp")
set(_apigen_stamp "${_apigen_dir}/complete.stamp")
add_custom_command(
    OUTPUT "${_apigen_stamp}"
    BYPRODUCTS "${_apigen_cpp}" "${_apigen_dir}/api-manifest.json" "${_apigen_dir}/generation.json"
               "${_apigen_dir}/diagnostics.json" "${_apigen_dir}/coverage.json"
    COMMAND "${Python3_EXECUTABLE}" -m mirage_apigen.build_cpp "${_apigen_dir}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_apigen_stamp}"
    DEPENDS ${_apigen_python} "${CMAKE_SOURCE_DIR}/tools/sdkgen/engine-layouts.json"
            "${CMAKE_SOURCE_DIR}/tools/apigen/config/api.toml"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools"
    VERBATIM)
add_custom_target(mirage_apigen_generate DEPENDS "${_apigen_stamp}")
add_library(mirage_apigen_bindings OBJECT "${_apigen_cpp}")
add_dependencies(mirage_apigen_bindings mirage_apigen_generate)
target_include_directories(mirage_apigen_bindings PRIVATE "${CMAKE_SOURCE_DIR}/loader/src" "${CMAKE_SOURCE_DIR}/loader/include")
target_link_libraries(mirage_apigen_bindings PRIVATE lua::lua)
target_sources(${PROJECT_NAME} PRIVATE $<TARGET_OBJECTS:mirage_apigen_bindings>)
