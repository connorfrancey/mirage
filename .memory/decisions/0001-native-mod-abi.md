# 0001 Preserve the native mod ABI

- **Date and status.** 2026-11-22. Accepted, recorded retrospectively.
- **Context.** Mods and the loader build independently. Windows x86 calling conventions and the layout of exported structs are part of the contract.
- **Decision.** Expose a versioned C function table. Extend `MirageApi` at its tail and preserve offsets for existing fields. Mirror exposed changes in Lua and managed bindings.
- **Reasoning.** Existing native binaries retain their field offsets. The C ABI limits dependencies on loader internals and compiler-specific C++ interfaces.
- **Alternatives.** Retrospective comparison. Exporting C++ objects or inserting members into the existing table would couple independently built binaries to the loader's current layout.
- **Consequences.** Use the ABI offset tests and review version checks in clients. Implement new runtime behavior behind the public table.

[Public ABI](../../loader/include/mirage/api.h), [ABI tests](../../tests/test_abi.cpp), [scripting and bindings](../scripting.md).
