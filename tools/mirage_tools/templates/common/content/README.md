# content/

Data the game uses, no code (mod.toml's `content = ["content"]`). An empty folder changes nothing.

- `defaults.toml`: property overrides, one table per object:

  ```toml
  ["TdGame.Default__TdPawn"]
  MaxStepHeight = 60.0
  ```

- `textures/<Package.Group.Name>.png`: a replacement texture, named by the texture's full path (PNG, TGA, BMP, JPG,
  or DDS used as stored).
- `meshes/<Package.Group.Name>.glb`: a replacement mesh (glTF, FBX or OBJ), converted first with
  `uv run mirage model <file> --convert` (docs/models.md).
- `sounds/<full SoundNodeWave path>.ogg` replaces a game's sound wave with mono or stereo Ogg Vorbis.
  Packages already opened keep their current version until the game restarts.
- `animations/<Name>.json`: animations for the player's meshes.
