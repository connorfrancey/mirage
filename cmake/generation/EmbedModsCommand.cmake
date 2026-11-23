# ======================================================================================================================
# Embed Mods Command
# ======================================================================================================================

# Embeds every first-party mod's files (and the native mod DLLs) into firstparty_mods.cpp (MIRAGE_GAME only).
# The script it runs is cmake/EmbedMods.cmake.
get_property(_native_mods GLOBAL PROPERTY MIRAGE_NATIVE_MODS)
set(_native_dlls "")
foreach(_id IN LISTS _native_mods)
    list(APPEND _native_dlls ${MIRAGE_NATIVE_MODS_DIR}/${_id}/${_id}.dll)
endforeach()
string(REPLACE ";" "," _bins_arg "${_firstparty_bins}")
add_custom_command(OUTPUT ${_firstparty_cpp}
        COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR}/mods -DOUT=${_firstparty_cpp} "-DBINS=${_bins_arg}"
        -P ${CMAKE_SOURCE_DIR}/cmake/EmbedMods.cmake
        DEPENDS ${_firstparty_files} ${CMAKE_SOURCE_DIR}/cmake/EmbedMods.cmake
        COMMENT "Embedding the first-party mods (mods/)")
