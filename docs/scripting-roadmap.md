# Mirage scripting roadmap

The goal is to give mod authors a documented way to use every engine feature available in Mirror’s Edge PC. Each
feature gets a working route through reflection, an owned Mirage API, or a content workflow. The roadmap also records
engine features that are unavailable in this build, need an external service, or still need a runtime probe. A name
in a generated header establishes that the type was found in the packages. A working example and a runtime check
establish what a mod can do with it.

This roadmap covers scripting and engine access. [The Mirage plan](mirage-plan.md) tracks the wider loader, editor,
and distribution work. [Field Lab](../examples/mods/tech-demo/README.md) is the integration example to extend as
new families of APIs land.

## Build the coverage inventory

1. Enumerate types, properties, functions, states, and native indices from the game's packages through UnrealLib and
   [the SDK generator](../tools/sdkgen). Include cooked asset kinds, archetypes, and Kismet and UI sequence nodes.
   Inspect the inventory with `uv run mirage sdk-browse --gui` and `uv run mirage assets`. Include driver and
   service packages alongside Core, Engine, and TdGame.
2. Join that list to [the C ABI](../loader/include/mirage/api.h), [Lua bindings](../loader/src/runtime/lua),
   [C# bindings](../managed/Mirage), [generated API reference](api-reference.md), and game-test steps. Record
   the minimum runtime for each verified API.
3. Track one row per capability. Record the engine type or subsystem, supported build, prerequisites, read and
   write routes, thread, lifetime, cleanup, Lua and C# coverage, and a test or example link. Group aliases and
   inherited functions under their shared behavior.
4. Classify each row as **verified**, **partial**, **probe pending**, or **unavailable**. A partial row states
   which operations work. An unavailable row records the evidence and the build or dependency involved.
5. Prioritize missing lifecycle and marshalling work before adding more raw calls. Once a family is covered,
   compare new inventory snapshots in CI so newly discovered capabilities and lost coverage get reviewed.

## Engine coverage and milestones

The existing routes below are starting points. The last column gives a runnable outcome for each family.

| Engine family | Existing route or evidence | Work and acceptance check |
| --- | --- | --- |
| Core reflection, packages, object lifetime | UnrealLib metadata, generated SDK, reflected object handles and calls | Cover nested containers, arrays, names, enum and bitfield values, interfaces, delegates, optional and out parameters, class defaults, subobjects, references, and GC. Round-trip representative values and reject handles after destruction, travel, or slot reuse. |
| UnrealScript and native dispatch | Class compilation, generated classes, function hooks, bytecode patches, injections and mod natives | Inventory opcodes, native indices and signatures. Exercise functions, events, states and latent functions, static/native calls, optional arguments, and rollback of layered patches. Record unsupported compiler constructs and build-specific natives. |
| World, levels, and streaming | Actor lookup, spawn, level layers, custom maps and streaming events | Cover component construction and attachment, transforms and lifetimes, actor factories, archetypes, world and game rules, and streaming control. Spawn, attach, stream, and remove a kit in an isolated map. Verify cleanup through travel and checkpoint reload. |
| Pawn, controller, input, and movement | Movement tuning and slots, rebindable actions, simulation and TAS input, Blink and Grappling Hook | Cover possession, interaction, parkour, inventory, weapons, damage and game modes. Add analog controller and rumble support where the build exposes it. Play a short course with keyboard and gamepad and verify which inputs can replay deterministically. |
| Collision, materials, and physics | Trace, overlap, bodies, impulses, joints, triggers, movers and movement pieces | Cover collision channels and filtering, shape and joint settings, contact events, physical materials, ragdolls and constraints. Verify a triggered rig with contacts, a joint and rollback on unload. |
| Vehicles | Cooked `Vehicle`, `SVehicle`, wheel and Mirror’s Edge vehicle types are inventory candidates | Probe usable archetypes, assets, and the player-driving state. Build an original test car with entry and exit, steering, braking, handbrake, and camera. Prove the pawn is released on restart, unload, and driver death. See [vehicle milestone 50](mirage-plan.md#50-player-drivable-vehicles-p2). |
| AI, navigation, encounters, and combat | Bots can be placed through level layers. SDK and Kismet provide discovery routes | Cover possession, perception, pathing and nav links, goals, movement, encounters, and damage. Spawn a bot, watch it navigate and react to a stimulus, then clear the encounter without retaining controllers or paths. |
| Kismet, missions, routes, and checkpoints | Graph read, edits, layers, execution tracing, objectives, and checkpoints | Cover sequence variables, links and events, latent actions, named graph instances, Matinee, and mission/TAS integration. Play a mission that survives a checkpoint restart and records the intended graph activity. |
| Animation, skeletal meshes, and IK | Playback, custom clips, animation sets, skeletal mesh replacement | Inventory slots, notifies, blend trees, mesh sockets, morphs, IK, root motion, and cloth. Verify a custom character interaction with a socket attachment, animation notify, and return to stock motion. |
| Cameras, cinematic tracks, and time | Camera query, world projection, reflected camera functions | Add scoped control with priority, follow and free cameras, blends, camera effects, cuts, Matinee, and time controls. Show two competing camera owners and verify the original camera returns on release, reload, and travel. |
| Rendering, lighting, VFX, and assets | Overlay and 3D lines, screen shaders, static/skeletal mesh and texture paths | Add owned materials and parameters, decals, particles, lights, shadows, fog, render targets, scene capture, and named render passes. Show scripted changes in a framed test scene. Verify release on mod unload and graphics-device reset. |
| Audio, subtitles, and haptics | Mod WAV playback and generated audio and input types | Probe available mixers, file formats, sound cues, spatial audio, music, subtitles, and controller feedback. Verify listener-relative volume and that voices, loops, and rumble stop on owner removal. |
| Game UI and localization | Mirage overlay widgets and a slider in the game's own options menu | Cover supported `UIScene` and widget operations, focus, navigation, data stores, controller input, HUD, subtitles, localized strings, and menu restoration. Add a test page to the game’s own UI and verify back, cancel, page changes, and unload. |
| Saves, persistence, replays, and timers | Per-mod config, transactions, TAS hooks, and level checkpoints | Define versioned mod save data, deterministic RNG and timers, replay state, checkpoints, stats and score. Test save migration, rewind and reload with a clean game profile. |
| Networking, online services, and platform | Discoverable types in Engine, IpDrv, OnlineSubsystemPC and driver/service packages | Probe which local networking, replication, sessions, achievements, and platform features still work on the supported build. Use an owned loopback test for capabilities that require peers. Mark externally dependent and inactive paths with evidence. |
| Diagnostics, assets, and authoring | SDK browser, asset catalog, profiler, Script Console, level and Kismet tools | Add capability and usage search, class/package deltas, compiler diagnostics, API parity and performance reports, and a reproducible test fixture library. Each public milestone ships with an example and an inventory update. |

## Delivery order

| Phase | Deliverable | Exit check |
| --- | --- | --- |
| 1. Inventory and contracts | Produce the initial coverage report, supported-build matrix, API ownership rules, and test fixture list. Finish common marshalling and handle validation first. | Every inventoried family has an owner, a verified route or a probe to run, and an acceptance check. Existing Lua, C#, and native examples continue to work. |
| 2. Gameplay and world | Expose actor/component lifecycle, camera-independent input and movement, physics, AI, Kismet, and level operations. Build the isolated gameplay fixtures. | A scripted course exercises input, world streaming, contacts, triggers, mission progress, and checkpoint cleanup. |
| 3. Presentation and authoring | Expose animation, camera, audio, UI, lighting, materials, particles, and render resources. Expand Field Lab with a demo for each new route. | A scene reacts to gameplay without leaking resources across reload, map travel, or device reset. Tooling can identify the assets and APIs it uses. |
| 4. Complex gameplay and state | Deliver the vehicle milestone, persistence and replay contracts, and remaining cinematics and encounter features. | Car entry and exit, scripted AI and cinematics, state migration, and replay each have a repeatable in-game check. |
| 5. Build and platform closure | Run the inventory against every supported game build and close gaps in platform/online support, parity, budgets, and documentation. | Every inventory row has an evidenced outcome for each supported build. The coverage report lists any dependencies and unsupported behavior by name. |

Follow the broader plan’s level editor requirement for each implementation stage. Add the related authoring
workflow and verify it with that stage’s example, such as a trigger inspector during physics or a parameter editor
during rendering. Keep the steps small enough to build and game-test independently.

## Step by step: expose an engine feature through the API

The example is a scoped scalar material override for a mod’s mesh. The same sequence applies to a camera owner,
spatial sound, a physics constraint, or another engine capability.

1. **Define what the mod should be able to do.** Write a one-sentence outcome and a pass/fail test before choosing
   signatures. For a scalar override, changing a named material parameter on one mesh should change that mesh’s
   appearance. Unload should restore its original material and leave other users of that material unaffected.
2. **Find and probe the game path.** Use the SDK browser to inspect the type and verify its name, signature, flags,
   arguments and any native index. For this example, inspect `MaterialInstanceConstant::SetScalarParameterValue`
   and the component’s material slot operations in [the generated Engine header](../sdk/include/sdk/Engine.hpp).
   Check a real cooked asset in the asset catalog. Use a small mod in a test level to confirm that the call
   works, what owns the instance, and whether it needs a render update. Record the observed game build.
3. **Choose the public route.** Check for an existing API or a supported reflected call that already satisfies the
   contract. If scripts need ownership, validation, conflict handling, or faster repeated updates, add a dedicated
   operation. A scoped material override can own a private material instance for the component slot and return
   a handle that can be updated and released.
4. **Write the contract in the C ABI.** Append fields to `MirageApi` in
   [api.h](../loader/include/mirage/api.h). Document arguments, units, ownership, valid thread, errors, and
   cleanup there. Use ABI-safe types and size-tagged extensible structs when needed. Preserve previous offsets and
   account for older runtime tables with `MIRAGE_API_HAS`.
5. **Implement through the engine.** Add the typed operation to the appropriate runtime subsystem and wire its
   C ABI entry in [the API table](../loader/src/runtime/mods.cpp). Resolve handles and validate class, slot,
   parameter and finite values before the write. Touch UObjects on the game thread. For functions with a native
   index use [`mirage::call`](../loader/src/runtime/call.h) so parameter storage and native dispatch match the
   engine. Reuse the existing lookup, transaction and cleanup services.
6. **Implement ownership and rollback.** Record the previous state before changing it and release the owned
   resource on explicit release, disable, reload, unload, crash, or relevant level teardown. Test partial failure,
   repeated release, overlapping owners, and stale handles. For the material example, confirm the original slot
   is restored without clearing another mod’s active override.
7. **Expose the bindings through their normal entry points.** Add the Lua binding in
   [the appropriate Lua area](../loader/src/runtime/lua) and register it in that area’s `LuaFunctions` list.
   Regenerate [the managed ABI](../managed/Mirage/Native/Api.g.cs) and add the typed C# wrapper under
   [managed/Mirage](../managed/Mirage). Add a C++ convenience wrapper in
   [mirage.hpp](../loader/include/mirage/mirage.hpp) when the operation needs one. Use consistent units,
   defaults, and errors across the languages.

   ```powershell
   uv run python tools/gen_csharp_api.py
   uv run mirage docs api
   ```

8. **Verify the contract and the example.** Update [the ABI tests](../tests/test_abi.cpp) for new entries.
   Test marshalling, missing capabilities and validation offline. Add a named self-test to
   [selftest.cpp](../loader/src/selftest.cpp) when the engine behavior needs proof. For the material example,
   inspect the live instance or rendered output before and after the override, then verify restoration.
   Wait for a state change. Announce before the game test starts.

   ```powershell
   uv run mirage build
   uv run mirage test
   uv run mirage game-test --no-build --only "Mirage material override"
   ```

   The last command uses the proposed self-test name. Register the step before running it.

9. **Close the inventory entry.** Add the example or Field Lab interaction, link the API reference and exact
   verification command, and record supported builds, languages and limitations. Check the debugger or profiler
   if the operation runs per frame. For presentation APIs, also verify device reset or resource teardown.
