# Packages and archives

| Boundary | Source |
| --- | --- |
| Installed mod | Directory recognized by `FindInstalledMods` and loaded through `mod.toml`. [Runtime manifest](../loader/src/runtime/manifest.cpp) |
| Author packaging | `mirage mod package` stages the mod and builds its distributable. ZIP is the default. The `miragemod` format wraps MPAF in MUMS. [CLI implementation](../tools/mirage_tools/modtools.py) |
| MPAF payload | `ArchiveWriter` and `ArchiveReader` encode and verify a deterministic, uncompressed file archive. [C++ API](../archive/include/mirage/mpaf.h), [wire specification](../MPAF_SPEC.md) |
| MUMS envelope | The envelope uses a single LZ4 frame and an optional CRC-32 footer. [Wire specification](../MUMS_SPEC.md) |

MPAF uses checked offsets, sorted UTF-8 paths, an indexed file table, and CRC-32 plus XXH3-64. Bounds and path rules precede access, and content hashes are checked on read or extraction. The hashes detect corruption. Extract into a fresh destination with trusted parents. Follow the spec for byte layout and golden vectors. [Reader](../archive/src/reader.cpp), [format implementation](../archive/src/format.cpp), [tests](../archive/tests/test_mpaf.cpp)

The MPAF reader accepts either supported manifest path. New `.miragemod` packages carry `mod.toml`. The packer can verify MUMS and the inner file hashes with `mirage_pack --verify <file>`. Installed mods are discovered as directories.

Release notices use the license files from the configured dependency sources. If UnrealLib is configured, include its notice and zlib's. Use the separate fmt notice when that source is configured. [Packaging](../tools/mirage_tools/cli.py)

The [identity specification](../docs/specifications/mirrors-edge-mod-identity.md) concerns identity metadata across tools. UE3 cooked files are covered in [assets](assets.md).
