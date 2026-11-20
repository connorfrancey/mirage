# Exporting models from Blender

A mod's 3D model can be a prop placed in a level, or a replacement for one of the game's static meshes. The model is
made in Blender (or any tool that writes glTF, FBX or OBJ) and converted once by the `mirage` tool to a `.mmesh` file
beside it; the game builds that into a mesh when it loads. This page gives the Blender settings that give the right
result, and what the converter does with what Blender writes.

Skeletal meshes (anything with bones, such as Faith's arms) are not supported yet: only static meshes.

## The short version

1. Model at the world origin, Z up, 1 Blender meter = 50 game units. Keep Scene > Units > Unit Scale at 1.0.
2. Give it UVs, and one material per part that needs its own texture.
3. File > Export > glTF 2.0, format **glTF Binary (.glb)**, with the settings below.
4. Check it: `uv run mirage model models/thing.glb`. Fix every `problem:` line it prints.
5. Convert it: `uv run mirage model models/thing.glb --convert`. Ship both `thing.glb` and `thing.glb.mmesh`.

Convert again whenever the `.glb` changes: the `.mmesh` holds a hash of its source, and a stale one is refused.

## Export settings (glTF 2.0)

glTF is the format to use: it is the one tested end to end (the self-test's Suzanne is a Blender `.glb`), it holds
the textures inside the file, and its axes and units are fixed by the standard.

| Section | Setting | Value | Why |
|---|---|---|---|
| Format | Format | glTF Binary (.glb) | One file with its textures inside |
| Include | Limit to | Selected Objects (if the scene holds more) | Every exported mesh becomes part of one model |
| Transform | +Y Up | On (the default) | The converter expects glTF's Y up and turns it to the game's Z up |
| Data > Mesh | Apply Modifiers | On | The converter sees only the exported triangles |
| Data > Mesh | UVs, Normals | On | Textures need UVs; normals keep your smoothing |
| Data > Mesh | Tangents | Either | The converter makes them when the file has none |
| Data > Material | Materials | Export | Material names and textures come from here |
| Data > Material | Images | Automatic (PNG or JPEG) | The game decodes PNG, JPEG, TGA and BMP, not WebP |
| Data > Shape Keys, Armature, Skinning | | Off | Static meshes don't deform |
| Data > Compression | Draco | **Off** | The converter can't read Draco-compressed meshes |
| Animation | Animation | Off | Not used |

## Size, axes and pivot

- **Scale.** The converter multiplies by 50 (the game's units per meter), so a 2 m tall Blender object is 100 units
  tall in the game. `--scale N` changes the factor; `mirage model <file>` prints the size in the file's own units.
- **Axes.** Blender's X is the game's X, Blender's Z is up, and Blender's Y becomes the game's -Y (the game is
  left-handed; the flip keeps the model from coming out mirrored). An actor faces its +X, so if the prop has a front,
  point it along Blender's +X.
- **Pivot.** Object transforms are baked in, and the mesh's pivot is Blender's **world origin**, not the object's
  origin. Put the point the prop should stand on (usually the middle of its base) at (0, 0, 0).
- **Triangles.** Quads and n-gons are triangulated by the converter. A mesh part holds at most 65535 vertices; split
  bigger ones into separate objects (or materials).

## Materials and textures

Each Blender material becomes one section of the game mesh.

- **The model's own textures.** Build the material from a Principled BSDF:
  - an Image Texture into **Base Color** becomes the diffuse texture;
  - an Image Texture into a **Normal Map** node (Tangent Space) into **Normal** becomes the normal map. Use
    Blender's own convention (OpenGL, green up); the converter flips green for the game. A DirectX-style map (green
    down) must be flipped before use;
  - an Image Texture into **Roughness** (non-color) is exported in glTF's metallic-roughness image, and the game gets
    a grey specular map of 1 - roughness from it. Metallic is ignored.

  The game material used is the characters' lit generic one (`CH_Materials.M_Generic_Diff_Spec_Norm`), so other
  shader nodes (emission, transparency, mixing) are not carried over.
- **A game material.** Name the Blender material after a game material, as `Package.Name` (for example a material
  from the level the prop goes in). The section then uses that material and ignores the model's textures.
- **Neither.** A material with no texture and no game name gets the engine's default (checkered) material.
- **Texture sizes.** Use powers of two (256, 512, 1024, 2048). Other sizes work, but they are resampled to the
  nearest power of two, which blurs them slightly.

## Collision

Nothing to export. The player collides with the model's own triangles, and physics (a prop that falls or is pushed)
uses a convex hull the game makes from the model's outermost points. A concave model (an arch, a cup) gets a hull
that fills the gap, for physics only.

## Using the model

**A prop in a level.** Put the model in the mod's `models/` folder and place it with the in-game Level Editor, or
by hand in a level layer (`levels/<map>.toml` in the mod) on a `DynamicSMActor_Spawnable`:

```toml
[[actor]]
class = "Engine.DynamicSMActor_Spawnable"
location = [1200.0, -340.0, 64.0]
mesh = "models/thing.glb"          # or "<other mod id>:models/thing.glb"
```

**Replacing a game mesh.** A content mod's `content/meshes/<Package.Group.Name>.glb` replaces that StaticMesh
everywhere the game draws it (see `tests/mods/model-test`, which turns the Prologue's garbage bag,
`P_Waste.GarbageBag_01.S_GarbageBag_01`, into Suzanne). The original's materials are kept section by section, in the
order of the model's materials, so give the model as many materials as the original has.

To match the original's size and pivot, start from the original: UnrealLib's extractor writes any static mesh as OBJ,

```
UnrealPackageExtractor <CookedPC>\P_Waste.upk -o out -t meshes --filter GarbageBag
```

Import that OBJ into Blender with the default axes (Forward -Z, Up Y). It comes in in game units (1 Blender meter =
1 unit), already at the right pivot. Model over it, delete it, export, and convert with `--scale 1` so the units stay
as they are.

## FBX and OBJ

Both convert, but check the size and orientation with `mirage model <file>` first; glTF is the reliable path.

- **OBJ** has no embedded textures: keep the images next to the `.obj` and `.mtl` and ship them too. In the `.mtl`,
  `map_Kd` is the diffuse texture, `norm` the normal map and `map_Ks` the specular map (as stored, no conversion).
  Blender's OBJ exporter writes normal maps as `map_Bump`, which the converter doesn't read: change it to `norm`
  by hand.
- **FBX** keeps its specular texture as the specular map. Blender's FBX exporter can store the scene with a unit
  scale other than 1, so a model can come out 100 times too big or too small; if it does, fix it with `--scale`.
