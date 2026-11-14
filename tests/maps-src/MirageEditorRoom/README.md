# MirageEditorRoom: a map made with the game's 2008 editor

`tests/mods/map-test/maps/MirageEditorRoom.me1` is a room built in Mirror's Edge's own UnrealEd (`MirrorsEdge.exe
editor`, with the `wxRC` and `wxRES` folders from Unreal Tournament 3 next to the exe). Step 13's milestone asks for
a map made with that editor, and the self-test step "Mirage editor map" plays it. It holds only what is made here
(two brushes, a player start, a point light) and the engine's default classes: no game content.

How it was built, all through the editor's command box (`editor_commands.ps1` types commands into it and closes the
startup dialogs; the menu was switched off for the editor with `mirage\disabled`):

1. `python brushes.py` writes `outer.t3d` and `inner.t3d`: cubes in the format and vertex order the editor's own cube
   builder exports (`BRUSH EXPORT`), sized for a 2048 x 2048 room, 800 high, with its floor 96 units below the origin.
2. In the editor:

   ```
   BRUSH IMPORT FILE="<here>\outer.t3d" MERGE=0 FLAGS=0
   BRUSH ADD
   BRUSH IMPORT FILE="<here>\inner.t3d" MERGE=0 FLAGS=0
   BRUSH SUBTRACT
   ACTOR ADD CLASS=PlayerStart
   ACTOR ADD CLASS=PointLight
   MAP REBUILD
   ```

   `ACTOR ADD` places at the origin, inside the room. `BRUSH LOAD` reads brush packages, not T3D: use `BRUSH IMPORT`.
3. File > Save As (menu command 6013), saved as `MirageEditorRoom.umap` (the editor's map extension, `MapExt=umap`),
   then renamed to `.me1`. `OBJ SAVEPACKAGE` and `MAP SAVE` from the command box saved nothing, and the editor ignores
   `-EXEC=` at startup.

The editor's log stays nearly empty with this game's config (most categories are suppressed), so each step is checked
by its files: `BRUSH EXPORT` after an import shows what the builder brush holds.
