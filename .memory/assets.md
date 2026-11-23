# Asset and world data pipelines

## Cooked asset replacements

`content::Load` collects default-property changes, replacement assets, and custom animation clips. `texture_overlays` indexes package tables and plans replacements by object path. `BuildOverlay` serializes a derived package on demand and `file_redirect` steers incoming opens to it. The original game files stay untouched. A package already opened keeps its selected file for the session. [Content](../loader/src/runtime/content.cpp), [overlay planner](../loader/src/runtime/texture_overlays.cpp), [writer](../loader/src/runtime/texture_files.cpp), [redirector](../loader/src/runtime/file_redirect.h)

- Textures use image decoding and game-native mips and formats. Models use the Assimp-based `mirage model --convert` tool and source-checked `.mmesh` intermediates. `models` also owns skeletal replacement for already-loaded meshes. [Mesh format](../loader/src/runtime/mesh_file.h), [model runtime](../loader/src/runtime/models.cpp), [author docs](../docs/models.md)
- `content/sounds/<SoundNodeWave path>.ogg` replaces a cooked PC wave while the game's cue owns playback. `audio` is a separate WAV playback service backed by XAudio2. [Wave overlay](../loader/src/runtime/texture_files.cpp), [script audio](../loader/src/runtime/audio.h)

## Other asset entry points

Level-layer TOML goes through `layers`; `maps`, `routes`, and `kismet_layers` own their respective data layouts. Author edits through those services and their in-game editor surfaces. [Layers](../loader/src/runtime/layers.h), [maps](../loader/src/runtime/maps.h), [routes](../loader/src/runtime/routes.h), [Kismet layers](../loader/src/runtime/kismet_layers.h)

Keep authored fixtures separate from original game content. Asset tests and previews read the player's CookedPC installation. [Testing](testing.md), [tooling](tooling.md)
