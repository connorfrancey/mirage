# Mirage Field Lab

Field Lab is a playable tech demo for Mirror's Edge with the current Mirage loader. Copy this whole
`tech-demo` folder into `<game>/Binaries/mirage/mods/`, then enable **Field Lab** in Mirage's Mod Manager.
The converted model is included. Mirage compiles the UnrealScript instrument the first time you deploy.

## Run the demo

1. Load a map, look along a clear stretch of floor, and press **F6**. The lab draws a cyan ring and sets down a
   textured probe alongside it. **Deploy here** in the module panel does the same thing.
2. Walk through the ring. The probe jumps, Faith plays a short arm animation, and a chime and cyan scan sweep
   mark the pulse. Step out and back in to repeat it. The HUD tracks session and lifetime pulses.
3. Open the module panel to see whether the UnrealScript call, its Lua hook, the physics impulse, the Kismet delay,
   the animation, and the sound ran. Adjust launch speed and volume there. Adjust the scan sweep in the game's
   own **Pause → Options → Mod settings → Field Lab pulse intensity**.
4. Press **F7** or select **Pack up** to remove the lab. Disabling the module, dying, restarting a checkpoint, or
   changing maps clears it too. The controls can be rebound in the Mod Manager.

## What to inspect

| Feature | In the demo |
| --- | --- |
| Lua, manifests, settings, actions and storage | `mod.toml` declares the keys and settings. `src/main.lua` handles the input and saves the lifetime counter in the mod's config. |
| Reflection, vectors, traces and physics | The lab reads the player's camera and body, sweeps for room, and launches its own rigid body with a velocity-change impulse. |
| Custom models, material maps and collision | `models/Probe.obj` and its diffuse, normal and specular textures are the source for the rigid-body probe. The converted `.mmesh` is what Mirage loads. |
| UnrealScript compilation and hooks | `classes/MirageFieldInstrument.uc` supplies a reflected function. A Lua hook observes just that function and counts calls in the panel. |
| Kismet and reversible edits | The lab adds a private event and delay to the current map's root sequence when one is available. Its transaction removes the nodes, links, trigger and actors when you pack up. |
| Custom animation, sound and shaders | The animation JSON, PCM chime and HLSL sweep each run from a pulse. |
| Rendering, UI and telemetry | The module draws the ring in the world and projects a HUD marker over it. It exposes the latest state and pulse count through mod shares. |
| Reloading and runtime tools | Pack up, edit a Lua file, and let Mirage reload it. The Script Console can display the lab state with `return field_lab_status().kismet`; the mod profiler can show what the callbacks cost. |

To rebuild the mesh after editing the source, run
`uv run mirage model examples/mods/tech-demo/models/Probe.obj --convert --strict`.

## Other loader demos

Field Lab deploys into the current level. The repository also has dedicated examples for features that need a
different mod entry point, replace shared game assets, or author whole levels.

- [Example mods](../README.md) covers native DLLs, Lua and C# entry points, content overrides, scripting and
  bytecode patches.
- [Model examples](../../../tests/mods/model-test) and [the arms example](../../../tests/mods/arms-test) demonstrate
  game-wide static and skeletal mesh replacement. [Retexture](../retexture) demonstrates a texture replacement.
- [Map examples](../../../tests/mods/map-test) and [the mission example](../../../tests/mods/mission-test) cover
  map and level layers, objectives, checkpoints and Kismet layers.
- [Grappling Hook](../../../mods/mirage-grapple) and [Blink](../../../mods/mirage-blink) demonstrate custom movement.
- Use `uv run mirage assets` to explore cooked assets, `uv run mirage sdk-browse --gui` to inspect reflected types,
  and the in-game editors to inspect objects, levels, Kismet and scripts.
