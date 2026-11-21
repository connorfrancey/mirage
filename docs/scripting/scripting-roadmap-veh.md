# Drivable vehicles for Mirage

This plan expands [Mirage milestone 50](../mirage-plan.md#50-player-drivable-vehicles-p2) into a playable car, truck,
and helicopter mod. In a purpose-built test area, Faith walks up to a usable driver’s door, presses a rebindable
interaction key, enters the vehicle, takes control, and gets a third-person chase camera. She can operate the
vehicle’s lights and other systems, then get out at a clear position. Mods and level authors can place, tune,
observe, and remove vehicles through the same runtime.

The handling target is a GTA V-style balance of weight and responsiveness. A car should turn in readily and break
traction progressively. A loaded truck should accelerate and stop more slowly, lean in turns, and need more room
to maneuver. A helicopter should climb, bank, drift, and settle on its skids, with assists that make those controls
usable on a keyboard. Use original assets, animation, sounds, and HUD designs.

## What the engine gives us

| Candidate | Local evidence | Probe before committing to it |
| --- | --- | --- |
| Possession and seats | `Vehicle.DriverEnter`, `DriverLeave`, `SetInputs`, `DrivenVehicle`, and controller driving methods in the Engine SDK | Spawn and possess a vehicle in the retail game. Observe which controller and pawn own input, what Mirror’s Edge’s movement and checkpoint systems see, and whether exit restores Faith. |
| Ground physics | `SVehicle`, `SVehicleSimCar`, and `SVehicleWheel` expose simulation, wheels, steering, suspension and handbrake data | Verify where wheel shapes are created, which vehicles and collision assets can be loaded, and whether a four-wheel body moves and collides under native simulation. Test support for multiple powered and steering axles separately. |
| Game-specific vehicle types | `TdVehicle`, `TdVehicleSeatPawn`, and `TdVehicle_Helicopter` exist in the TdGame SDK | Classify usable archetypes and dependence on campaign scripts. Verify whether the helicopter exposes player-driven forces, or whether it only provides a scripted campaign body or seat. |
| Mirage building blocks | Reflected calls, class compilation, actor handles, level layers, traces, rigid-body operations, action events, camera queries, and model conversion | Find the smallest missing APIs for owned input capture, camera overrides, physics-step updates, and vehicle resources. Test which asset paths can create a standalone skeletal vehicle with bones, socket transforms, and a physics body. |

Local references. [Generated Engine SDK](../../sdk/include/sdk/Engine.hpp), [generated TdGame SDK](../../sdk/include/sdk/TdGame.hpp),
[Mirage API reference](../api-reference.md), and [model import workflow](../models.md).

### Decide the backend from runtime probes

1. Inventory cooked vehicle classes, physics assets, skeletons, wheel bones, seat sockets, lights, and sound cues
   with `mirage assets` and `mirage sdk-browse`. Record whether each asset can be reused at runtime or needs a
   mod-owned equivalent. Verify the compiler can create any component templates the selected class needs.
2. In an isolated level, drive one native `SVehicle` using scripted inputs. Record the actual physics update
   point, driver/controller ownership, wheel contact, and behavior on exit, death, and checkpoint restart. Probe
   the same state transitions for the helicopter without depending on a campaign sequence.
3. Prefer the native ground simulation if it passes. If the wheel path is unavailable, prototype suspension-ray
   contacts and tire forces on a PhysX rigid body. Keep collision and integration in the engine and apply forces
   at the contact points. Use the flight backend described below if the shipped helicopter depends on a mission.
4. Decide possession separately from simulation. Prefer native driver entry if the campaign controller, checkpoint
   logic, and HUD work with it. Otherwise, leave Faith’s controller assigned to her pawn and use a scoped seated
   state with vehicle input and camera ownership. In that fallback, verify that the pawn can be anchored, that its
   movement and collision can be suspended and restored, and that campaign triggers skip the seated pawn.
5. Document the results by game build and backend. Keep a four-wheel prototype and a direct enter/exit test before
   expanding the public ABI. A failed probe should narrow the backend choice or produce a specific engine blocker.

## Doors, driver state, and handover

Each drivable seat declares an approach point, door hinge or animation, entry pose, seated pose, and exit candidates.
For the first release, the nearest usable door leads to the driver or pilot seat. A truck can have a higher step-in
pose and a helicopter can have a wider clearance check. This leaves room to add passenger seats without changing
how driver ownership works.

1. **Find the door.** Query nearby registered vehicles at a bounded rate and transform their approach points into
   world space. Score by distance and facing, then test line of sight and body clearance. Require a free seat, a
   valid pawn, and a parked vehicle or one below its entry speed limit. Show the prompt at the chosen door.
2. **Reserve the seat.** Recheck on interaction press and claim the seat once. Double presses, another request for
   the same seat, or actor removal during the transition must leave at most one driver. Record the actor, pawn,
   controller, prior view target, and any movement, collision, visibility, and input state the handover will change.
3. **Enter.** Open the door and align Faith to the approach pose within a short swept path. Play an original
   entry clip and perform the engine handover at its seat marker. Use a timeout to roll back an interrupted
   animation. Start the camera blend, close the door, and accept driving input after the seat is confirmed.
4. **Drive.** Keep a vehicle input context active while seated. The on-foot UI and camera restore on release.
   Audit places where Mirage or the game expects its pawn lookup to return Faith. Expose separate driver and
   controlled vehicle queries so movement, damage, checkpoint, TAS, and mission systems can choose the right actor.
5. **Exit.** Require low ground speed, or skids on the ground and low speed for a helicopter. Sweep Faith’s
   standing capsule to each candidate from the relevant door, check support and headroom, and reject occupied or
   steep landing points. Try the opposite door if available. If no point is safe, retain the seat and explain why.
6. **Recover.** On death, restart, map travel, mod disable, crash, or vehicle destruction, cancel transitions,
   neutralize inputs, and release camera and seat claims once. Use a verified safe exit if the world still exists.
   If the pawn or world is being destroyed, hand recovery to the game’s respawn or checkpoint path. Never restore
   handles from the old world into the new pawn.

Treat each transition as a transaction. Leases belong to the vehicle session and can be released repeatedly.
Normal shutdown tests must cover disable at the door, during entry, while driving, and while exiting.

## Driving controls and handling

| Action | Suggested keyboard defaults | Suggested controller defaults |
| --- | --- | --- |
| Enter or exit | `E` near a usable door or while driving on the ground, `F` to exit a landed helicopter | Context action button |
| Car and truck drive | `W` throttle, `S` brake or reverse, `A/D` steer, `Space` handbrake | Triggers throttle and brake, left stick steer, face button handbrake |
| Helicopter flight | `W/S` pitch, `A/D` roll, `Q/E` yaw, `Space/Ctrl` climb and descend | Left stick cyclic, shoulder buttons yaw, triggers collective |
| View | Mouse look, re-center key, look-behind key | Right stick look and re-center button |
| Systems | Keys for ignition, headlights, horn and auxiliary functions | Rebindable buttons or a compact command menu |

Resolve overlapping defaults by context. In flight, `E` is yaw and the exit command uses a separate binding.
Give camera look an independent input map so turning the camera does not steer or yaw the vehicle. Clear held
inputs on focus loss, opening menus, controller disconnect, seat exit, or transition cancellation. Use ramped
digital controls, analog dead zones and response curves, and an adjustable steering assist. Consume interaction
input only when the mod has a valid target or active seat.

### Cars and trucks

- Define mass, center of mass, inertia, axle positions, wheel radius, suspension travel, spring and damper rates,
  driven and steerable axles, engine torque curve, gear ratios, shift behavior, differential and brake balance per
  vehicle. Keep tuning data in SI units and convert through a single engine-units boundary.
- Use wheel contacts and surface data to apply load-dependent longitudinal and lateral grip, combined slip, rolling
  resistance, and braking. If the native backend supplies those operations, tune its curves and measure behavior
  with the same fixtures as the fallback. On the fallback, apply equal and opposite dynamic contact forces where
  supported, and use a fixed simulation step with bounded catch-up.
- Limit steering angle with speed, smooth both throttle and brake, give wheels progressive slip near the grip limit,
  and treat handbrake as rear or designated axle braking. At speed, reverse input brakes first. Gear changes and
  suspension movement should show up in the instruments, body motion, and sound.
- Start with a compact car and a rigid two-axle truck. Give the truck its own mass, wheelbase, gearing, axle load,
  turn radius, and cargo mass range. Verify that heavier load changes acceleration, braking, and cornering.
  Once two axles are stable, probe more axles and trailers as extensions.
- Build a track with marked distances, a slalom, a narrow reverse bay, incline, curb, and low-grip patch. Log
  acceleration, stopping distance, turn radius, wheel contact, slip, and body roll. Define target envelopes after
  the first baseline. The car should beat the loaded truck to the same speed and stop in a shorter distance.
  Repeat at 30 and 62 FPS and investigate measurable frame-dependent drift.

### Helicopters

- Model rotor thrust through the aircraft’s up axis, cyclic pitch and roll, collective lift, yaw torque, and
  translational and angular damping. Tie available lift to rotor RPM and spool-up. Tune gravity compensation,
  vertical acceleration limits, and low-speed stability for an approachable flight model. Use the real control
  vocabulary to keep bindings and HUD terminology clear. [FAA Helicopter Flying Handbook, chapters 2–3](https://www.faa.gov/regulations_policies/handbooks_manuals/aviation/helicopter_flying_handbook).
- Apply thrust and torque to the engine rigid body at the physics step. Give skids or landing gear correct contact
  points and collision response. Verify that gust-like disturbances, hard contact, slopes, and altitude changes
  do not introduce unbounded angular velocity. Choose supported simulation and collision behavior before adding
  effects such as ground wash, rotor disk checks, or degraded power.
- Supply auto-level and optional hover assist with bounded corrections and smooth handoff to pilot input. Keep raw
  cyclic, yaw, and collective independent. Add a reduced-assist preset once the default works on keyboard and
  controller. Display rotor readiness, height above ground when measurable, and vertical speed.
- Test takeoff from a pad, a stationary hover, yaw without translation commands, forward and lateral travel,
  controlled descent, landing, spool-down, and exit. Also test blocked rotor clearance, an obstructed landing
  pad, world travel while airborne, and cleanup when the aircraft is removed. Put the first flight area in a
  purpose-built level with verified streaming and collision.

## Third-person chase camera

- Acquire a scoped camera override when the seat commits. Blend from Faith’s view into a target near the vehicle’s
  center and above its body. Give the car a close chase view, the truck a farther and higher framing, and the
  helicopter a view that keeps the tail and landing area visible. Release the override on exit or cleanup so
  the underlying engine camera or another owner resumes.
- Separate the smoothed target, desired orbit, and camera boom. Use velocity and steering for look-ahead, damp
  target translation, and limit how much body roll and pitch transfer to the horizon. Re-center after an
  adjustable idle delay. Support free orbit, look behind, and at least two follow distances.
- Sweep the boom from the target to the desired camera position using a small volume and explicit vehicle and
  occupant exclusions. Shorten quickly on obstruction and lengthen smoothly when clear. Verify the near plane,
  corners, tunnels, low ceilings, reversing beside walls, and rotorcraft between buildings. Snap interpolation
  on teleport or a world change.
- Update control and collision decisions from game-thread snapshots. Publish completed transforms for rendering.
  Check seat movement and camera behavior at different frame rates. Keep speed-based FOV changes and camera shake
  adjustable, with an option to disable them.

## Vehicle systems and assets

| System | Player-facing behavior and implementation |
| --- | --- |
| Lighting | Toggle headlights and high beams, left and right indicators, hazards, and optional auxiliary lamps. Derive brake and reverse lamps from actual controls and gear. Use emissive materials for lamp state and bounded lights for beams. Give the helicopter navigation lights, beacon, and a controllable landing light or searchlight. |
| Engine and exhaust | Allow ignition and shutdown. Use RPM and load for engine sound, tailpipe particles, and exhaust intensity. Let a service or demo setting enable or suppress exhaust effects for vehicles that have them. Use a rotor or turbine outlet and engine-state effects for the helicopter. |
| Audio and effects | Use spatial engine or rotor loops, startup and shutdown cues, tire skid, suspension impacts, horn, and door sounds. Tie dust, rotor wash, and impact effects to contact or measured speed. Release loops and pooled effects when the vehicle stops or unloads. |
| Animated parts | Drive steering wheel, suspension, wheel rotation, doors, rotor blades, and optional mirrors from the simulated state. Use separately named bones or sockets and swap rotor visuals at high RPM. Keep visual updates separate from collision state. |
| Driver instruments | Show speed, gear, handbrake, active lights, and contextual door or exit feedback. For helicopters show rotor readiness, altitude or height above ground, climb rate, and assist state. Expose the state to scripts so other mods can present their own HUDs. |
| Assets and authoring | Ship one original vehicle for each family. Validate dimensions, pivots, units, wheels or skids, door and seat sockets, camera anchors, lamp sockets, exhaust sockets, collision, material slots, and audio/effect references. Add an editor preview for door fit, exit clearances, wheel or skid contacts, center of mass, and chase camera framing. |

Use a validated versioned vehicle definition, referenced by a level layer. Separate shared identity, body, seats,
camera, and effects from the ground axle data or rotorcraft flight data. Allow optional effects to be absent and
report missing required sockets or invalid tuning during import. Keep built or cached packages keyed by their
source assets, content version, and compatible game profile.

## Mirage implementation and scripting API

| Layer | Responsibility |
| --- | --- |
| Engine adapter | Locate and verify engine functions per supported build, manage possession or seated state, and run movement before the relevant physics step. Route native-index calls through `mirage::call`. Use game-thread handles and the existing pattern-based hook infrastructure. |
| Vehicle runtime | Own vehicle and driver lifetimes, physics backend selection, scope input and camera control, and publish durable events and read-only telemetry. Keep vehicle motion independent of the HUD, scripts, sound, and renderer. |
| Lua and C# | Provide authoring and gameplay control through spawn, query, enter/exit request, system toggles, and events such as seat changed or vehicle removed. Reuse settings, actions, and transactions. Keep physics substeps out of per-frame script callbacks. |
| Public ABI | Use validated opaque handles and versioned data or snapshots. Append narrow, proven operations once the car and helicopter agree on the shared contract. Mirror them in Lua and C# and cover older runtime tables, missing capabilities, and stale handles. |
| Diagnostics and editor | Report seat ownership, input context, physics backend, contacts, RPM, camera obstruction, and resource leaks. Provide a preview and validate each definition when a level author places a vehicle. |

API sequence. First prove spawn, seat, control, and cleanup internally. Add public capabilities for vehicle
discovery and telemetry, then requests for entry, exit, and systems, then authoring and event subscriptions. Use the
[API implementation guide](scripting-roadmap.md#how-to-expose-an-engine-feature-through-the-api) for each addition.

## Milestones and completion checks

| Milestone | Deliverable | Gate |
| --- | --- | --- |
| 0. Retail probes | Class and asset inventory, native ground and helicopter test, controller and physics timeline | Evidence for the selected ground backend, flight backend, and possession strategy, with a safe fallback or task-specific blocker for each. |
| 1. Car vertical slice | One greybox car, one door, driver handover, keyboard input, provisional chase camera | Walk to the actual door, enter, accelerate, turn, brake, reverse, exit, and restart. Check state and contact outcomes in the real game. |
| 2. Stable shared vehicle runtime | Camera and control ownership, input settings, vehicle events, editor socket preview, selected systems | Verify wall and tunnel camera behavior, focus-loss cleanup, alternate exit, rollback during entry, and resource release on disable. |
| 3. Cars and trucks | Original low-poly car and rigid truck assets, per-axle tuning, truck loads, lamps, exhaust, sound, and instruments | Run timed ground tests on both. Demonstrate heavier handling with cargo and reliable steering, parking, lights, and smoke across repeated spawns. |
| 4. Helicopter | Flight model, pilot door interaction, original rotorcraft, flight camera, assists, airfield, and systems | Taxi or spool on the pad, take off, hover, turn, fly a route, descend, land, exit, and remove the aircraft while flight and camera systems are active. |
| 5. Complete showcase | An installable mod, vehicle authoring docs, Lua and C# examples, editor placement, and full integration coverage | The player can drive all three vehicle types end to end using keyboard or controller. Each supported build passes the lifecycle, handling, camera, and systems checks. |

Use unit tests for definition validation, input shaping, state transitions, camera damping, and the math in any
fallback solver. Add named game tests such as `Mirage vehicle seats`, `Mirage vehicle ground`,
`Mirage vehicle flight`, and `Mirage vehicle systems`. For a driving or flying step, wait on telemetry,
contacts, or checkpoints rather than elapsed time. A physical effect passes when it changes the underlying state
and the corresponding visible or audible feedback.

Run failure tests for blocked doors, seat contention, moving vehicles, unloaded components, driver death, mod
reload, device reset, level streaming, pause, and checkpoint restart. Record input, seat and vehicle state with
TAS when available. Treat physics replay determinism as a measured property and label replays accordingly.

Profile a single active vehicle and a parked fleet on the game's 32-bit build. Measure simulation, script
callbacks, camera queries, effects, and memory separately. Keep wheels, lights, particles, and audio dormant when
appropriate. Use the first greybox measurements to set active-vehicle, parked-fleet, and frame-time budgets. The
release gate includes repeated spawn, drive, exit, and teardown cycles with no growing resource counts.

The first implementation task is a tiny car probe on a clean test map. Resolve its body, four wheels, driver
seat, controller and input handover before building a public API or investing in final vehicle assets.
