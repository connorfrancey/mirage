# ======================================================================================================================
# Output
# ======================================================================================================================

# Copy the finished DLL to <repo>/build/ so it's in one fixed place regardless of preset/config
option(MIRAGE_COPY_TO_BUILD_DIR "Copy Mirage.dll to <repo>/build/ after each build" ON)
if(MIRAGE_MINIMAL)   # Don't overwrite the full build's DLL in build/
    set(_mirage_output_default "${PROJECT_SOURCE_DIR}/build/minimal")
else()
    set(_mirage_output_default "${PROJECT_SOURCE_DIR}/build")
endif()
set(MIRAGE_BUILD_OUTPUT_DIR "${_mirage_output_default}" CACHE PATH "Where MIRAGE_COPY_TO_BUILD_DIR copies the DLL")
if(MIRAGE_COPY_TO_BUILD_DIR)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${MIRAGE_BUILD_OUTPUT_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${PROJECT_NAME}> "${MIRAGE_BUILD_OUTPUT_DIR}"
            COMMENT "Copying ${PROJECT_NAME}.dll to ${MIRAGE_BUILD_OUTPUT_DIR}")
endif()
