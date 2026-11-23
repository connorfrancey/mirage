# Packages and archives

| Boundary | Source |
| --- | --- |
| Installed mod | Directory recognized by `FindInstalledMods` and loaded through `mod.toml`. [Runtime manifest](../loader/src/runtime/manifest.cpp) |
| Author packaging | `mirage mod package` stages the mod and builds its distributable. The committed baseline produces ZIP. [CLI implementation](../tools/mirage_tools/modtools.py) |
| MPAF payload | `ArchiveWriter` and `ArchiveReader` encode and verify a deterministic, uncompressed file archive. [C++ API](../archive/include/mirage/mpaf.h), [wire specification](../MPAF_SPEC.md) |
| MUMS envelope | The specification assigns outer framing and LZ4 compression to this layer. No implementation was found in configured targets during this audit. [Wire specification](../MPAF_SPEC.md) |

MPAF uses checked offsets, sorted UTF-8 paths, an indexed file table, and CRC-32 plus XXH3-64. Bounds and path rules precede access, and content hashes are checked on read or extraction. The hashes detect corruption. Extract into a fresh destination with trusted parents. Follow the spec for byte layout and golden vectors. [Reader](../archive/src/reader.cpp), [format implementation](../archive/src/format.cpp), [tests](../archive/tests/test_mpaf.cpp)

The committed V1 spec and golden vectors use the reserved `manifest.yaml` path for opaque payload bytes. The active TOML worktree accepts `mod.toml` and has a packer that writes raw MPAF with a `.miragemod` extension. The MUMS envelope and runtime archive installation remain separate gaps. Reconcile this distinction before changing compatibility. [Baseline session](sessions/2026-11-22-baseline.md)

The [identity specification](../docs/specifications/mirrors-edge-mod-identity.md) concerns identity metadata across tools. UE3 cooked files are covered in [assets](assets.md).
