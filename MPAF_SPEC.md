# Mirage Payload Archive Format 1.0

MPAF is the uncompressed file archive inside a Mirage `.miragemod` package. The outer MUMS layer owns the 64-byte envelope header, LZ4 compression, and optional 32-byte footer. Its decoder passes only the decompressed payload bytes to MPAF. Its encoder compresses the complete MPAF output and writes the envelope. This specification defines the payload independently of the MUMS wire format.

## Binary layout

All integers are unsigned and little-endian. Offsets are relative to the first byte of the decompressed payload unless stated otherwise. Sections are consecutive with no padding, alignment requirement, or trailing bytes.

```text
64-byte header
64-byte entry × entry_count
UTF-8 path string table
File and manifest data
```

### Header

| Offset | Bytes | Field | Version 1.0 value or meaning |
| --- | ---: | --- | --- |
| 0x00 | 8 | Magic | ASCII `MIRAGEAR`, hex `4d 49 52 41 47 45 41 52` |
| 0x08 | 2 | Major version | 1 |
| 0x0a | 2 | Minor version | 0 |
| 0x0c | 4 | Flags | 0 |
| 0x10 | 4 | Entry count | At least 1 |
| 0x14 | 4 | Entry size | 64 |
| 0x18 | 8 | Entry table offset | 64 |
| 0x20 | 8 | String table offset | Entry table offset plus entry count times 64 |
| 0x28 | 8 | String table size | Total path bytes |
| 0x30 | 8 | Data offset | String table offset plus string table size |
| 0x38 | 8 | Archive size | Data offset plus total file data size, at most 4,294,967,295 |

### Entry

| Offset | Bytes | Field | Version 1.0 value or meaning |
| --- | ---: | --- | --- |
| 0x00 | 4 | Path offset | Relative to the start of the string table |
| 0x04 | 4 | Path length | 1 to 4,096 bytes |
| 0x08 | 8 | Data offset | Absolute MPAF offset of the current data cursor |
| 0x10 | 8 | Data size | Number of data bytes |
| 0x18 | 8 | Original size | Equal to data size |
| 0x20 | 4 | CRC32 | CRC-32/ISO-HDLC over the data |
| 0x24 | 2 | Entry type | 0 file, 1 manifest, 2 directory |
| 0x26 | 2 | Flags | 0 |
| 0x28 | 8 | Content hash | Unseeded, standard XXH3-64 over the data |
| 0x30 | 8 | Reserved | 0 |
| 0x38 | 8 | Reserved | 0 |

Each type-0 or type-1 entry stores its bytes verbatim. CRC-32/ISO-HDLC uses the reflected polynomial `0xedb88320`, initial state `0xffffffff`, and final XOR `0xffffffff`. The XXH3 value is serialized as a little-endian integer. An empty file has CRC `00000000` and XXH3 `2d06800538d394c2`. A directory has zero sizes and zero hashes. Both empty files and directories store the current data cursor as their offset and leave it unchanged.

Exactly one entry has type 1 and path `manifest.yaml`. That exact path is reserved for the manifest. The archive layer treats its contents as opaque bytes. Manifest syntax, schema, and validation belong to the package application.

## Paths and canonical serialization

- Paths are UTF-8 byte strings with no terminating NUL. UTF-8 must encode Unicode scalar values in shortest form. Paths are case-sensitive and retain their original normalization form.
- Path components are separated by ASCII `/`. Components must be nonempty and cannot end with a dot or ASCII space. This excludes both `.` and `..`. Absolute paths, repeated separators, trailing separators, and backslashes are invalid.
- ASCII controls `00` through `1f` and `7f` and the characters `:`, `<`, `>`, `|`, `"`, `?`, and `*` are invalid. For each component, take the part before its first dot and remove trailing ASCII spaces. Compare that stem using ASCII case-folding. Reject `CON`, `PRN`, `AUX`, `NUL`, `CONIN$`, `CONOUT$`, and `COM` or `LPT` followed by a single ASCII digit or by `¹`, `²`, or `³`.
- Sort entries lexicographically by unsigned UTF-8 bytes, with the shorter path first when one is a prefix of the other. Duplicates and file ancestors are invalid. A file ancestor is an entry of type 0 or 1 whose path followed by `/` prefixes another path.
- Start the path cursor at zero. Write each path at the cursor and advance by its length. Start the data cursor at the header’s data offset. Write each file or manifest at that cursor and advance by its size. Directory entries are optional and may represent empty directories. Parent directories for nested entries can be inferred.
- Identical path, type, and byte sets produce identical payloads regardless of input order. File timestamps, ownership, permissions, and host OS metadata are omitted.

## Validation and resource limits

1. Check the header length, magic, version, flags, entry width, declared archive size, and configured limits. Version 1.0 readers reject other major or minor versions.
2. Derive the section boundaries with checked arithmetic. Require the declared layout to equal the derived layout, all sections to fit the actual payload, and the actual size to match the declared size exactly.
3. Traverse entries in order. Check the record fields and require path and data offsets to match their respective cursors. Bound both advances before using the referenced ranges. Validate every path, type, flag, reserved field, and size. Require both cursors to end at their exact section boundaries.
4. Verify that the table is strictly sorted, contains exactly one root manifest, and has no file ancestors.
5. Verify both hashes before returning or extracting each file’s contents. A full archive verification checks every file and the manifest. These checks detect corruption and provide no authenticity guarantee.

Default C++ limits are 256 MiB of decompressed payload, 100,000 entries, and 16 MiB of path strings. Callers may impose smaller budgets or raise them within the wire and host address-space bounds. The outer codec should enforce a decompression budget before allocating its output and validate the actual output size. MPAF uses no per-file compression.

The C++ extraction API creates a fresh destination directory or an exclusive new file under a trusted destination parent. It rejects existing targets, symlinks and Windows reparse points in the supplied parent chain, and archive paths that collide on the destination filesystem. Full extraction defaults to a limit of 100,000 additional directories, including implicit parents. A destination parent and its ancestors must be protected against concurrent replacement by untrusted local users. Output names that exceed local filesystem limits can cause extraction to fail even when the input is a valid MPAF payload.

## Golden vectors

The [manifest-only vector](archive/tests/vectors/manifest-only.mpaf) is a 141-byte archive with one empty manifest. The [checksum vector](archive/tests/vectors/check.mpaf) is a 223-byte archive with `check.txt` holding ASCII `123456789` and an empty manifest. These are decompressed MPAF payloads.

For the checksum vector the complete hexadecimal encoding is

```text
0000  4d 49 52 41 47 45 41 52 01 00 00 00 00 00 00 00
0010  02 00 00 00 40 00 00 00 40 00 00 00 00 00 00 00
0020  c0 00 00 00 00 00 00 00 16 00 00 00 00 00 00 00
0030  d6 00 00 00 00 00 00 00 df 00 00 00 00 00 00 00
0040  00 00 00 00 09 00 00 00 d6 00 00 00 00 00 00 00
0050  09 00 00 00 00 00 00 00 09 00 00 00 00 00 00 00
0060  26 39 f4 cb 00 00 00 00 ff 7d a1 67 8b b1 dc 72
0070  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0080  09 00 00 00 0d 00 00 00 df 00 00 00 00 00 00 00
0090  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00a0  00 00 00 00 01 00 00 00 c2 94 d3 38 05 80 06 2d
00b0  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
00c0  63 68 65 63 6b 2e 74 78 74 6d 61 6e 69 66 65 73
00d0  74 2e 79 61 6d 6c 31 32 33 34 35 36 37 38 39
```

## C++ use

Link `mirage::mpaf` and include `<mirage/mpaf.h>`. The reader can borrow an immutable payload or take ownership of a decompressed byte vector. Filesystem and stream readers buffer the uncompressed payload for random access. Filesystem sources are streamed in bounded chunks during writing. Sources should remain stable while the archive is written, and a writer checks that the data matches the hashes it serialized.

```cpp
#include <mirage/mpaf.h>

using namespace mirage::mpaf;

auto writer = ArchiveWriter::FromDirectory("my-mod");
auto payload = writer.Serialize(); // Input to the outer package encoder.
auto archive = ArchiveReader::Open(std::move(payload));
auto manifest = archive.Manifest(); // A verified view into the owned payload.
archive.Verify();
archive.ExtractAll("unpacked-mod");
```

The main project runs the MPAF GoogleTest cases through `uv run mirage test -R Mpaf`. The `archive` directory is also a standalone CMake project. Enable `MIRAGE_BUILD_BENCHMARKS` to build `mirage_mpaf_bench`, which measures parsing, serialization, hashing, lookup, and verified extraction.
