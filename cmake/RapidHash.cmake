# rapidhash (https://github.com/Nicoshev/rapidhash): the 64-bit hash behind core/hash.h, for hashing data in bulk where
# FNV-1a's byte loop was the cost: the resource cache's content keys (core/resource_cache.h) and mods' file
# fingerprints. Measured against FNV-1a in bench/bench_hash.cpp. Header only. File formats that already store FNV-1a
# (.merp checksums) keep it: changing them would break saved files.
# The version is pinned in cmake/Versions.cmake (RAPIDHASH_VERSION); the fallback here only covers a Versions.cmake
# without it.
if(NOT RAPIDHASH_VERSION)
  set(RAPIDHASH_VERSION rapidhash_v3)
endif()
CPMAddPackage(
        NAME rapidhash
        GITHUB_REPOSITORY Nicoshev/rapidhash
        GIT_TAG ${RAPIDHASH_VERSION}
        DOWNLOAD_ONLY YES
)
if(rapidhash_ADDED AND NOT TARGET rapidhash::rapidhash)
    add_library(rapidhash INTERFACE)
    target_include_directories(rapidhash SYSTEM INTERFACE ${rapidhash_SOURCE_DIR})
    add_library(rapidhash::rapidhash ALIAS rapidhash)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE rapidhash::rapidhash)
