# Unreal Engine 3 SDK

Use `uv run mirage sdk` to regenerate the C++ SDK from the game packages. Use `uv run mirage sdk-browse --gui` to inspect classes and layouts.

`uv run mirage sdk --metadata build/sdk-metadata.json` exports merged layout metadata for MirageAPIGen. The generator uses that metadata for inspection pages and a reviewed exposure policy for runtime bindings.

The documentation build covers the hand-authored SDK bridge by default. Enable `MIRAGE_DOCS_SDK` to include the generated C++ dump, or `MIRAGE_DOCS_LUA_INSPECTION` and `MIRAGE_DOCS_LUA_INPUT` to publish Lua inspection pages from a full metadata export.
