# Mirage Unified Mod Specification v1.0

`.miragemod` contains a 64-byte MUMS header, one standard LZ4 frame carrying the complete [MPAF payload](MPAF_SPEC.md), and an optional 32-byte footer. All integers are unsigned little-endian. Sections are consecutive.

## Header

| Offset | Size | Value |
| --- | --- | --- |
| `0x00` | 8 | `4d 55 4d 53 00 00 00 00` (`MUMS` and four zero bytes) |
| `0x08` | 2 | Major version `1` |
| `0x0a` | 2 | Minor version `0` |
| `0x0c` | 2 | Compression `1` (LZ4 frame) |
| `0x0e` | 2 | Reserved header size, `0` |
| `0x10` | 4 | Flags, `0` |
| `0x14` | 4 | Reserved, `0` |
| `0x18` | 4 | Exact LZ4 frame size in bytes |
| `0x1c` | 4 | Exact decompressed MPAF size in bytes |
| `0x20` | 4 | Reserved, `0` |
| `0x24` | 4 | Footer size, `0` or `32` |
| `0x28` | 24 | Reserved, all zero |

The frame begins with `04 22 4d 18` and consumes exactly the advertised compressed section. Dictionary IDs, additional frames, skippable frames, and trailing data are invalid. The optional frame content size must agree with the envelope. Readers verify the decoded byte count. The alpha implementation caps decompressed data at 256 MiB, matching MPAF's default, and caps the entire envelope at 260 MiB before allocating.

## Footer

| Offset | Size | Value |
| --- | --- | --- |
| `0x00` | 4 | `4d 55 4d 35` (`MUM5`) |
| `0x04` | 2 | Reserved, `0` |
| `0x06` | 2 | Checksum type `1` (CRC-32/ISO-HDLC) |
| `0x08` | 4 | CRC-32 of the complete decompressed MPAF payload |
| `0x0c` | 20 | Reserved, all zero |

Writers include the footer by default. If present, readers verify it before handing bytes to MPAF. CRC-32 detects corruption.

## Manifest and conformance

New packages use a root `mod.toml`. The inner MPAF decoder accepts either that path or `manifest.yaml` and requires exactly one manifest. MUMS treats the payload bytes as opaque.

[`manifest-only.mums`](archive/tests/vectors/manifest-only.mums) wraps the existing `manifest-only.mpaf` payload with an independently constructed LZ4 frame using one uncompressed block and a CRC footer. The C++ tests also exercise the compressor, malformed envelope sections, and missing or extra frame bytes.
