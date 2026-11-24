# ======================================================================================================================
# Target
# ======================================================================================================================

# Injected mod => DLL, not an executable
add_library(${PROJECT_NAME} SHARED ${HEADER_FILES} ${SOURCE_FILES})
if(MIRAGE_GAME)
    target_sources(${PROJECT_NAME} PRIVATE ${_firstparty_cpp})
endif()
set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")

# build_info.cpp: a new build identifier (UUID, commit, build time) on every build, logged at startup
include(cmake/BuildInfo.cmake)
mirage_add_build_info(${PROJECT_NAME})

source_group(TREE ${PROJECT_SOURCE_DIR}/loader/src FILES ${HEADER_FILES} ${SOURCE_FILES})

# Include root, definitions and warnings: mirage_target_settings (cmake/project/CompilerOptions.cmake)
