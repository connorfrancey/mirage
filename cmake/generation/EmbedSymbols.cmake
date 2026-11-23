# ======================================================================================================================
# Embed Symbols
# ======================================================================================================================

# The engine's symbols (runtime/symbols.toml), built in as a string: configure again when the file changes
# (MIRAGE_GAME only)
set(_symbols_toml ${CMAKE_SOURCE_DIR}/loader/src/runtime/symbols.toml)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_symbols_toml})
file(READ ${_symbols_toml} _symbols_text)
file(WRITE ${CMAKE_BINARY_DIR}/generated/symbols_toml.inc.tmp "R\"symbols(${_symbols_text})symbols\"
")
file(COPY_FILE ${CMAKE_BINARY_DIR}/generated/symbols_toml.inc.tmp ${CMAKE_BINARY_DIR}/generated/symbols_toml.inc ONLY_IF_DIFFERENT)
file(REMOVE ${CMAKE_BINARY_DIR}/generated/symbols_toml.inc.tmp)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_BINARY_DIR}/generated)
