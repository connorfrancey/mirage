# 0004 Separate the payload from its distribution envelope

- **Date and status.** 2026-11-22. Specified in MPAF V1. The payload reader and writer landed in `482f093`.
- **Context.** Distribution compression and payload indexing have different concerns. Tools need a deterministic representation of the files and their checksums.
- **Decision.** Define MPAF as the uncompressed, indexed file payload. The spec assigns outer framing and LZ4 compression to MUMS.
- **Reasoning.** Payload parsing, path validation, and deterministic vectors can be tested independently of distribution compression.
- **Alternatives.** Retrospective comparison. A single compressed archive layer would bind payload readers and vectors to the distribution codec.
- **Consequences.** Keep the wire spec and vectors synchronized with reader and writer changes. See the package note for open questions about the CLI, TOML, and the envelope.

[MPAF V1 specification](../../MPAF_SPEC.md), [payload API](../../archive/include/mirage/mpaf.h), [package status](../package-format.md).
