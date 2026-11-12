# xxHash (https://github.com/Cyan4973/xxHash): XXH3, for hashing data in bulk where FNV-1a's byte loop was the cost:
# the resource cache's content keys (core/resource_cache.h) and mods' file fingerprints. Measured against FNV-1a in
# benchmarks/bench_hash.cpp. Header only (XXH_INLINE_ALL in core/hash.h). File formats that already store FNV-1a
# (.merp checksums) keep it: changing them would break saved files.
# The version is pinned in cmake/Versions.cmake (XXHASH_VERSION); the fallback here only covers a Versions.cmake
# without it.
if(NOT XXHASH_VERSION)
  set(XXHASH_VERSION v0.8.3)
endif()
CPMAddPackage(
        NAME xxhash
        GITHUB_REPOSITORY Cyan4973/xxHash
        GIT_TAG ${XXHASH_VERSION}
        DOWNLOAD_ONLY YES
)
if(xxhash_ADDED AND NOT TARGET xxhash::xxhash)
    add_library(xxhash INTERFACE)
    target_include_directories(xxhash SYSTEM INTERFACE ${xxhash_SOURCE_DIR})
    target_compile_definitions(xxhash INTERFACE XXH_INLINE_ALL)
    add_library(xxhash::xxhash ALIAS xxhash)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE xxhash::xxhash)
