# Trust and failure boundaries

- Native mod DLLs execute in the game process with its privileges. Guarded calls, fault attribution, and startup safe mode protect availability. Treat native and managed mod code as trusted by the user. [Guard and lifecycle](../loader/src/runtime/mods.cpp), [safe mode design](../docs/infrastructure.md)
- Lua exposes selected libraries and bounds resource use. Keep mod file access on the established API and preserve its path handling. Review the callback and thread contracts of every new binding. [Lua VM](../loader/src/runtime/lua_vm.h), [mod I/O](../loader/src/runtime/mod_io.h)
- Treat manifests, cooked assets, meshes, and archives as untrusted input. Resolve paths against the declared root, validate sizes and offset arithmetic before use, and use the archive extraction rules for destination creation. [Manifest parser](../loader/src/runtime/manifest.cpp), [archive validation](../archive/src/reader.cpp), [format limits](../MPAF_SPEC.md)
- Safe mode prevents unattended retries after a failed startup. Crash logs can include paths, addresses, and stack information, so keep them local unless sharing is explicitly authorized. [Crash reporter](../loader/src/runtime/crash_reporter.h)

See [package format](package-format.md) for the distinction between integrity checks and package authenticity. Installation and executable patches require the user's authorization. [Conventions](conventions.md)
