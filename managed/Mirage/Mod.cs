using System;
using System.Collections.Generic;
using Mirage.Native;

namespace Mirage
{
    /// <summary>
    /// A C# mod: derive one class from this in the mod's assembly (mod.toml: <c>dotnet = "MyMod.dll"</c>). The runtime
    /// makes it, calls <see cref="OnLoad"/> (register modules and events there) and, before unloading it (a hot reload,
    /// the menu unloading), <see cref="OnUnload"/>.
    /// </summary>
    /// <example>
    /// public sealed class Hello : Mirage.Mod
    /// {
    ///     public override void OnLoad() => new HelloModule(this);
    /// }
    /// </example>
    public abstract unsafe class Mod
    {
        internal void* Handle;
        internal readonly List<IDisposable> Owned = new();      // Modules, subscriptions, hooks: released at unload

        public string Id { get; internal set; } = "";
        public string Version { get; internal set; } = "";
        public string Folder { get; internal set; } = "";
        public Logger Log { get; internal set; } = null!;

        public virtual void OnLoad() { }
        public virtual void OnUnload() { }

        /// <summary>A value in the mod's config.toml (fallback when missing).</summary>
        public string Config(string key, string fallback = "")
        {
            var api = Bridge.Api;
            var buf = stackalloc byte[1024];
            uint n = api->config_get(Handle, Utf8.Intern(key), Utf8.Intern(fallback), buf, 1024);
            return Utf8.Read(buf, (int)Math.Min(n, 1023u));
        }

        public void SetConfig(string key, string value) =>
            Utf8.With(value, v => { Bridge.Api->config_set(Handle, Utf8.Intern(key), (byte*)v); return 0; });

        /// <summary>Calls <paramref name="handler"/> on an event (MIRAGE_EVENT_*); released when the mod unloads.</summary>
        public Subscription On(GameEvent type, Action<EventArgs> handler) => Subscription.Create(this, type, handler);

        /// <summary>Hooks a game function ("Engine.Actor.SetLocation"); released when the mod unloads.</summary>
        public Hook Hook(string function, HookWhen when, Action<Call> handler, int priority = 0) =>
            Mirage.Hook.Create(this, function, when, priority, handler);

        /// <summary>
        /// Calls <paramref name="handler"/> from inside <paramref name="function"/>'s bytecode at <paramref name="point"/>:
        /// "head", "return", "before:Name[#n]", "after:Name[#n]" (a statement calling Name), "token:i". The call reads and
        /// writes parameters and locals by name; <c>call.Skip()</c> returns from the function there (with "ReturnValue").
        /// </summary>
        public Hook Inject(string function, string point, Action<Call> handler, int priority = 0) =>
            Mirage.Hook.CreateInjection(this, function, point, priority, handler);

        /// <summary>
        /// Registers <paramref name="fn"/> as a native the game's bytecode can call (a script patch's NativeFunction
        /// token with the returned index). <paramref name="signature"/>: "float(float, int)"; arguments arrive as int,
        /// float, bool, GameObject or string (a name); the return value is converted to the declared type.
        /// </summary>
        public int Native(string name, string signature, Func<object?[], object?> fn) =>
            Mirage.ModNative.Register(this, name, signature, fn);

        /// <summary>
        /// The class in the UnrealScript file <paramref name="path"/> (relative to the mod's folder, "classes/Pad.uc"),
        /// compiled and loaded by the engine; it spawns like a cooked class. Throws with the compiler's errors when it
        /// doesn't compile. Game thread.
        /// </summary>
        public GameObject LoadClass(string path)
        {
            var buf = stackalloc byte[1024];
            void* cls = Bridge.Api->class_load(Handle, Utf8.Intern(path), buf, 1024);
            if (cls == null) throw new InvalidOperationException($"{path}: {Utf8.Read(buf)}");
            return new GameObject(cls);
        }

        /// <summary>
        /// A class described instead of written: the runtime writes the UnrealScript, compiles and loads it. A function in
        /// <see cref="ClassSpec.Functions"/> is an override of the parent's (by name) or a new one (by declaration,
        /// "function int Count()"); its code is the handler, run at the head of its empty body (read and write the
        /// parameters with <c>call.Get</c>/<c>call.Set</c>, the object is <c>call.Object</c>; set "ReturnValue" and
        /// <c>call.Skip()</c> to return a value). Throws with the compiler's errors. Game thread.
        /// </summary>
        public GameObject DefineClass(ClassSpec spec)
        {
            var functions = new List<string>();
            foreach (var (key, _) in spec.Functions) functions.Add(key);
            var json = System.Text.Json.JsonSerializer.Serialize(new Dictionary<string, object>
            {
                ["name"] = spec.Name, ["extends"] = spec.Extends, ["placeable"] = spec.Placeable,
                ["vars"] = spec.Vars, ["defaults"] = spec.Defaults, ["functions"] = functions, ["extra"] = spec.Extra,
            });
            var buf = stackalloc byte[2048];
            var text = System.Runtime.InteropServices.Marshal.StringToCoTaskMemUTF8(json);
            void* cls;
            try { cls = Bridge.Api->class_define(Handle, (byte*)text, buf, 2048); }
            finally { System.Runtime.InteropServices.Marshal.FreeCoTaskMem(text); }
            if (cls == null) throw new InvalidOperationException($"{spec.Name}: {Utf8.Read(buf)}");
            var path = new GameObject(cls).Path;
            foreach (var (key, handler) in spec.Functions)
            {
                // An override is named; a new function's name is in its declaration, before '('
                var name = key;
                var paren = key.IndexOf('(');
                if (paren >= 0)
                {
                    var head = key.Substring(0, paren).TrimEnd();
                    name = head.Substring(head.LastIndexOf(' ') + 1);
                }
                Inject(path + "." + name, "head", handler);
            }
            return new GameObject(cls);
        }

        /// <summary>
        /// The actor named <paramref name="name"/> in a level layer (levels/&lt;map&gt;.toml) of this mod, or of
        /// <paramref name="modId"/>'s; not valid if there is none in the current level. Game thread.
        /// </summary>
        public GameObject LayerActor(string name, string? modId = null)
        {
            var n = System.Runtime.InteropServices.Marshal.StringToCoTaskMemUTF8(name);
            var m = System.Runtime.InteropServices.Marshal.StringToCoTaskMemUTF8(modId ?? "");
            try { return new GameObject(Bridge.Api->layer_actor(Handle, (byte*)m, (byte*)n)); }
            finally
            {
                System.Runtime.InteropServices.Marshal.FreeCoTaskMem(n);
                System.Runtime.InteropServices.Marshal.FreeCoTaskMem(m);
            }
        }

        /// <summary>
        /// The game's function or global named <paramref name="name"/> ("ProcessEvent", "StaticLoadObject", "GMalloc"):
        /// its address, 0 if the runtime doesn't know it or didn't find it on this game. Any thread.
        /// </summary>
        public nint Symbol(string name) => (nint)Bridge.Api->symbol_find(Utf8.Intern(name));

        /// <summary>
        /// Detours native code: the game calls <paramref name="detour"/> (an [UnmanagedCallersOnly] function's address,
        /// with the target's calling convention) instead of <paramref name="target"/>; it goes on through
        /// <see cref="NativeDetour.Original"/>. Higher priority runs first. A fault in it is survived (the call goes on in
        /// the original) and stops the mod. Removed when disposed or when the mod unloads. Null if it failed (the log says why).
        /// </summary>
        public NativeDetour? Detour(nint target, nint detour, int priority = 0) => NativeDetour.Create(this, target, null, detour, priority);
        /// <summary><see cref="Detour"/> of a symbol (<see cref="Symbol"/>). Null if the symbol isn't known.</summary>
        public NativeDetour? DetourSymbol(string symbol, nint detour, int priority = 0) => NativeDetour.Create(this, 0, symbol, detour, priority);

        /// <summary>
        /// A hook at any instruction: <paramref name="callback"/> runs there with the registers and may change them. A fault
        /// in it skips it from then on and stops the mod. Removed when disposed or when the mod unloads.
        /// </summary>
        public NativeMidHook? MidHook(nint at, delegate* unmanaged[Cdecl]<void*, MirageRegisters*, void> callback, nint user = 0)
        {
            var h = Bridge.Api->mid_hook_create(Handle, (void*)at, (void*)callback, (void*)user);
            if (h == null) return null;
            var hook = new NativeMidHook(h);
            Owned.Add(hook);
            return hook;
        }

        /// <summary>
        /// The loaded levels' Kismet as JSON (api.h kismet_read has the shape): parse it with System.Text.Json.
        /// <paramref name="level"/>: only that level's package ("Edge_p"); null: every loaded level's. Game thread.
        /// </summary>
        public string KismetJson(string? level = null)
        {
            var l = Utf8.Intern(level ?? "");
            var n = Bridge.Api->kismet_read(l, null, 0);
            var buffer = new byte[n + 1];
            fixed (byte* b = buffer) Bridge.Api->kismet_read(l, b, (uint)buffer.Length);
            return System.Text.Encoding.UTF8.GetString(buffer, 0, (int)n);
        }

        /// <summary>
        /// Links <paramref name="from"/>'s output to <paramref name="to"/>'s input (a link's description, "Touched", or
        /// its index, "0"; null: the first). Recorded in the open transaction (or one of its own): put back on revert
        /// and unload. Game thread.
        /// </summary>
        public bool KismetLink(GameObject from, string? output, GameObject to, string? input = null) =>
            Bridge.Api->kismet_link(Handle, from.Handle, Utf8.Intern(output ?? ""), to.Handle, Utf8.Intern(input ?? "")) != 0;

        /// <summary>Removes a link (see <see cref="KismetLink"/>); put back on revert and unload. Game thread.</summary>
        public bool KismetUnlink(GameObject from, string? output, GameObject to, string? input = null) =>
            Bridge.Api->kismet_unlink(Handle, from.Handle, Utf8.Intern(output ?? ""), to.Handle, Utf8.Intern(input ?? "")) != 0;

        /// <summary>
        /// A new op of <paramref name="className"/> (a SequenceOp class) in <paramref name="sequence"/>; taken out again on
        /// revert and unload. Not valid when it can't (the log says why). Game thread.
        /// </summary>
        public GameObject KismetCreate(GameObject sequence, string className) =>
            new GameObject(Bridge.Api->kismet_create(Handle, sequence.Handle, Utf8.Intern(className)));

        /// <summary>
        /// Fires a Kismet event as the engine does (SequenceEvent.CheckActivate): <paramref name="output"/> which output
        /// ("Touched" or an index; null: the first), <paramref name="instigator"/> default: the player. True when it
        /// activated. Game thread.
        /// </summary>
        public bool KismetFire(GameObject @event, string? output = null, GameObject instigator = default) =>
            Bridge.Api->kismet_fire(Handle, @event.Handle, Utf8.Intern(output ?? ""), instigator.Handle) != 0;

        /// <summary>The Kismet execution trace on or off (nothing runs while no mod has it on). Game thread.</summary>
        public void KismetTrace(bool on) => Bridge.Api->kismet_trace(Handle, on ? 1 : 0);

        /// <summary>The trace's entries since the last read, as JSON [{"tick", "op", "class", "times"}]. Game thread.</summary>
        public string KismetTraceJson()
        {
            while (true)
            {
                var n = Bridge.Api->kismet_trace_read(Handle, null, 0);
                var buffer = new byte[n + 1];
                uint got;
                fixed (byte* b = buffer) got = Bridge.Api->kismet_trace_read(Handle, b, (uint)buffer.Length);
                if (got < buffer.Length) return System.Text.Encoding.UTF8.GetString(buffer, 0, (int)got);
            }
        }

        /// <summary>A rigid body's state (null: no such actor). Game thread.</summary>
        public Body? Body(GameObject actor)
        {
            var b = new MirageBody { size = (uint)sizeof(MirageBody) };
            if (Bridge.Api->physics_body_get(actor.Handle, &b) == 0) return null;
            return new Body(new Vec3(b.location[0], b.location[1], b.location[2]), b.rotation[0], b.rotation[1], b.rotation[2],
                            new Vec3(b.linear_velocity[0], b.linear_velocity[1], b.linear_velocity[2]),
                            new Vec3(b.angular_velocity[0], b.angular_velocity[1], b.angular_velocity[2]),
                            b.mass_scale, b.gravity_scale, b.awake != 0, b.simulated != 0);
        }

        /// <summary>Sets a body's velocity (and wakes it). Game thread.</summary>
        public bool SetVelocity(GameObject actor, Vec3 velocity)
        {
            var b = new MirageBody { size = (uint)sizeof(MirageBody) };
            b.linear_velocity[0] = velocity.X; b.linear_velocity[1] = velocity.Y; b.linear_velocity[2] = velocity.Z;
            return Bridge.Api->physics_body_set(Handle, actor.Handle, &b, 4u) != 0;
        }

        /// <summary>An impulse at the centre of mass (<paramref name="velocityChange"/>: the mass ignored). Game thread.</summary>
        public bool Impulse(GameObject actor, Vec3 impulse, bool velocityChange = false)
        {
            float* v = stackalloc float[3] { impulse.X, impulse.Y, impulse.Z };
            return Bridge.Api->physics_impulse(Handle, actor.Handle, v, null, velocityChange ? 1 : 0) != 0;
        }

        /// <summary>A force at the centre of mass, over the next physics step. Game thread.</summary>
        public bool Force(GameObject actor, Vec3 force)
        {
            float* v = stackalloc float[3] { force.X, force.Y, force.Z };
            return Bridge.Api->physics_force(Handle, actor.Handle, v, null) != 0;
        }

        /// <summary>
        /// A physics prop with the static mesh at <paramref name="mesh"/>, its mass the mesh's times
        /// <paramref name="massScale"/> (given here only); destroyed on revert and unload. Game thread.
        /// </summary>
        public GameObject SpawnProp(string mesh, Vec3 location, float massScale = 1f)
        {
            float* at = stackalloc float[3] { location.X, location.Y, location.Z };
            return new GameObject(Bridge.Api->physics_spawn(Handle, Utf8.Intern(mesh), at, null, massScale));
        }

        /// <summary>
        /// An actor of class <paramref name="cls"/>: the mod's "classes/X.uc", another mod's
        /// "mirage-ai:classes/MirageBot.uc", or a class path "Engine.Trigger"; destroyed on revert and unload. Game thread.
        /// </summary>
        public GameObject Spawn(string cls, Vec3 location, int yaw = 0)
        {
            float* at = stackalloc float[3] { location.X, location.Y, location.Z };
            int* rot = stackalloc int[3] { 0, yaw, 0 };
            return new GameObject(Bridge.Api->actor_spawn(Handle, Utf8.Intern(cls), at, rot));
        }

        /// <summary>
        /// Plays a WAV file of the mod's folder ("sounds/x.wav": PCM, 8 or 16 bit, mono or stereo) at
        /// <paramref name="volume"/> (0 to 1), until <see cref="SoundStop"/> when <paramref name="loop"/>. A handle, or 0
        /// when it can't (the log says why). Stopped when the mod unloads. Any thread.
        /// </summary>
        public int SoundPlay(string path, float volume = 1f, bool loop = false) =>
            Bridge.Api->sound_play(Handle, Utf8.Intern(path), volume, loop ? 1u : 0u);

        /// <summary>Stops a sound <see cref="SoundPlay"/> started (nothing if it has ended).</summary>
        public void SoundStop(int handle) => Bridge.Api->sound_stop(Handle, handle);

        /// <summary>True while the sound plays.</summary>
        public bool SoundPlaying(int handle) => Bridge.Api->sound_playing(Handle, handle) != 0;

        /// <summary>
        /// A full-screen effect from an HLSL pixel shader of the mod's folder ("shaders/x.hlsl", ps_3_0, entry point
        /// main): s0 is the game's frame, c0 (seconds, width, height, aspect), c1 to c15 the mod's
        /// (<see cref="PostFxSet"/>). A handle, or 0 when the file can't be read. Any thread.
        /// </summary>
        public int PostFx(string path) => Bridge.Api->postfx_create(Handle, Utf8.Intern(path));

        /// <summary>Sets c<paramref name="index"/> (1 to 15) of the effect's shader.</summary>
        public bool PostFxSet(int handle, int index, float x, float y = 0f, float z = 0f, float w = 0f)
        {
            float* v = stackalloc float[4] { x, y, z, w };
            return Bridge.Api->postfx_set(Handle, handle, index, v) != 0;
        }

        /// <summary>Draws the effect each frame (or stops).</summary>
        public bool PostFxEnable(int handle, bool on) => Bridge.Api->postfx_enable(Handle, handle, on ? 1 : 0) != 0;

        /// <summary>The effect's compile error, "" when there is none.</summary>
        public string PostFxError(int handle)
        {
            uint n = Bridge.Api->postfx_error(Handle, handle, null, 0);
            byte* buffer = stackalloc byte[(int)n + 1];
            Bridge.Api->postfx_error(Handle, handle, buffer, n + 1);
            return System.Text.Encoding.UTF8.GetString(buffer, (int)n);
        }

        /// <summary>A line trace (or a box sweep with <paramref name="extent"/>); null when nothing was hit. Game thread.</summary>
        public TraceHit? Trace(Vec3 start, Vec3 end, bool actors = false, Vec3? extent = null, GameObject ignore = default,
                               bool material = false)
        {
            float* s = stackalloc float[3] { start.X, start.Y, start.Z };
            float* e = stackalloc float[3] { end.X, end.Y, end.Z };
            float* x = stackalloc float[3];
            if (extent is Vec3 ext) { x[0] = ext.X; x[1] = ext.Y; x[2] = ext.Z; }
            var h = new MirageHit { size = (uint)sizeof(MirageHit) };
            var r = Bridge.Api->world_trace(Handle, s, e, extent is null ? null : x, (actors ? 1u : 0u) | (material ? 2u : 0u),
                                            ignore.Handle, &h);
            if (r != 1) return null;
            return new TraceHit(new GameObject(h.actor), new Vec3(h.location[0], h.location[1], h.location[2]),
                                new Vec3(h.normal[0], h.normal[1], h.normal[2]), h.time, new GameObject(h.phys_material));
        }

        /// <summary>
        /// A trigger the engine checks (its Trigger actor, a cylinder of <paramref name="radius"/> and half height
        /// <paramref name="height"/>): <paramref name="onEnter"/> / <paramref name="onLeave"/> / <paramref name="onStay"/>
        /// (each tick, only when given) get the trigger and the actor. Only actors of <paramref name="className"/> or with
        /// <paramref name="tag"/> when given. Removed on revert and unload. Game thread.
        /// </summary>
        public GameObject Trigger(Vec3 location, float radius, float height, Action<GameObject, GameObject>? onEnter = null,
                                  Action<GameObject, GameObject>? onLeave = null, Action<GameObject, GameObject>? onStay = null,
                                  string? className = null, string? tag = null)
        {
            var callbacks = new TriggerCallbacks(this, onEnter, onLeave, onStay);
            var handle = System.Runtime.InteropServices.GCHandle.Alloc(callbacks);      // Lives as long as the mod
            Owned.Add(new TestHandle(handle));
            var d = new MirageTriggerDesc { size = (uint)sizeof(MirageTriggerDesc), radius = radius, height = height, stay = onStay is null ? 0 : 1,
                                            class_name = Utf8.Intern(className ?? ""), tag = Utf8.Intern(tag ?? "") };
            d.location[0] = location.X; d.location[1] = location.Y; d.location[2] = location.Z;
            return new GameObject(Bridge.Api->trigger_create(Handle, &d,
                (void*)(delegate* unmanaged[Cdecl]<void*, void*, void*, int, void>)&OnTrigger,
                (void*)System.Runtime.InteropServices.GCHandle.ToIntPtr(handle)));
        }

        /// <summary>Actors inside a trigger of a mod now (-1: not one). Game thread.</summary>
        public int TriggerCount(GameObject trigger) => Bridge.Api->trigger_count(trigger.Handle);

        /// <summary>
        /// A swing pole (the game's TdSwingVolume): the bar's middle at <paramref name="location"/>, the player swinging
        /// toward <paramref name="yaw"/> (65536 = a full turn). Removed on revert and unload. Game thread.
        /// </summary>
        public GameObject SwingPole(Vec3 location, int yaw, float length = 200f, bool thinGrip = false, bool shimmy = false)
        {
            var d = new MiragePieceDesc { size = (uint)sizeof(MiragePieceDesc), kind = 0, yaw = yaw, length = length,
                                          flags = (thinGrip ? 1u : 0u) | (shimmy ? 2u : 0u) };
            d.location[0] = location.X; d.location[1] = location.Y; d.location[2] = location.Z;
            return new GameObject(Bridge.Api->piece_create(Handle, &d));
        }

        /// <summary>
        /// A moving platform, lift or door: <paramref name="mesh"/> (a static mesh's path) at <paramref name="location"/>,
        /// scaled by <paramref name="scale"/> (null: 1), turned to <paramref name="yaw"/>; it blocks, and a pawn standing on it
        /// rides it. Sent with <see cref="MoveTo"/>. Removed on revert and unload. Game thread.
        /// </summary>
        public GameObject Mover(string mesh, Vec3 location, Vec3? scale = null, int yaw = 0)
        {
            var s = scale ?? new Vec3(1, 1, 1);
            var d = new MirageMoverDesc { size = (uint)sizeof(MirageMoverDesc), mesh = Utf8.Intern(mesh) };
            d.location[0] = location.X; d.location[1] = location.Y; d.location[2] = location.Z;
            d.rotation[1] = yaw;
            d.scale[0] = s.X; d.scale[1] = s.Y; d.scale[2] = s.Z;
            return new GameObject(Bridge.Api->mover_create(Handle, &d));
        }

        /// <summary>
        /// A move of the mod's own, running as <paramref name="behavesAs"/> (a game move class, default TdMove_Falling, or
        /// a mod class extending TdMove): <paramref name="onStart"/> as the player enters it, <paramref name="onTick"/>
        /// (seconds) every tick in it, <paramref name="onStop"/> as the player leaves it. Its EMovement number (what
        /// PlayerState events report), or 0. Removed on revert and unload. Game thread.
        /// </summary>
        public int Move(string name, Action? onStart = null, Action<float>? onTick = null, Action? onStop = null, string? behavesAs = null,
                        string? anim = null, AnimSlot animSlot = AnimSlot.FullBody, float animRate = 1f, float blendIn = 0.15f,
                        float blendOut = 0.2f)
        {
            var callbacks = new MoveCallbacks(this, onStart, onTick, onStop);
            var handle = System.Runtime.InteropServices.GCHandle.Alloc(callbacks);      // Lives as long as the mod
            Owned.Add(new TestHandle(handle));
            var d = new MirageMoveDesc { size = (uint)sizeof(MirageMoveDesc), name = Utf8.Intern(name),
                                         behaves_as = behavesAs is null ? null : Utf8.Intern(behavesAs),
                                         fn = (void*)(delegate* unmanaged[Cdecl]<void*, int, float, void>)&OnMove,
                                         user = (void*)System.Runtime.InteropServices.GCHandle.ToIntPtr(handle),
                                         anim = anim is null ? null : Utf8.Intern(anim), anim_slot = (int)animSlot, anim_rate = animRate,
                                         anim_blend_in = blendIn, anim_blend_out = blendOut };
            return Bridge.Api->move_register(Handle, &d);
        }

        /// <summary>
        /// Plays <paramref name="sequence"/> (a name from <see cref="AnimSetsJson"/>) on a TdPawn's <paramref name="slot"/>,
        /// the game's PlayCustomAnim: true, or false (not a TdPawn, no such sequence: the log says why). Game thread.
        /// </summary>
        public bool AnimPlay(GameObject pawn, string sequence, AnimSlot slot = AnimSlot.FullBody, float rate = 1f, float blendIn = 0.15f,
                             float blendOut = 0.2f, bool loop = false, bool rootMotion = false)
            => Bridge.Api->anim_play(pawn.Handle, (int)slot, Utf8.Intern(sequence), rate, blendIn, blendOut,
                                     (loop ? 1u : 0u) | (rootMotion ? 2u : 0u)) == 1;

        /// <summary>Stops what a TdPawn's slot plays, blending out. Game thread.</summary>
        public bool AnimStop(GameObject pawn, AnimSlot slot = AnimSlot.FullBody, float blendOut = 0.2f)
            => Bridge.Api->anim_stop(pawn.Handle, (int)slot, blendOut) == 1;

        /// <summary>What a TdPawn's slot plays now (null: no such slot). Game thread.</summary>
        public AnimState? AnimGet(GameObject pawn, AnimSlot slot = AnimSlot.FullBody)
        {
            var s = new MirageAnimState { size = (uint)sizeof(MirageAnimState) };
            if (Bridge.Api->anim_state(pawn.Handle, (int)slot, &s) != 1) return null;
            return new AnimState(s.playing != 0, s.looping != 0, Utf8.Read(s.sequence), s.position, s.length, s.rate);
        }

        /// <summary>The AnimSets of an actor's skeletal meshes, as JSON (api.h anim_sets). Game thread.</summary>
        public string AnimSetsJson(GameObject actor)
        {
            uint n = Bridge.Api->anim_sets(actor.Handle, null, 0);
            var buffer = new byte[n + 1];
            fixed (byte* b = buffer) n = Bridge.Api->anim_sets(actor.Handle, b, (uint)buffer.Length);
            return System.Text.Encoding.UTF8.GetString(buffer, 0, (int)Math.Min(n, (uint)buffer.Length - 1));
        }

        /// <summary>Puts the player into one of the mod's moves now: true, or false (the log says why). Game thread.</summary>
        public bool StartMove(int move) => Bridge.Api->move_start(Handle, move) == 1;

        /// <summary>Takes one of the mod's moves out. Game thread.</summary>
        public void RemoveMove(int move) => Bridge.Api->move_remove(Handle, move);

        private sealed record MoveCallbacks(Mod Owner, Action? Start, Action<float>? Tick, Action? Stop);

        [System.Runtime.InteropServices.UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
        private static void OnMove(void* user, int phase, float seconds)
        {
            var cb = (MoveCallbacks)System.Runtime.InteropServices.GCHandle.FromIntPtr((IntPtr)user).Target!;
            try
            {
                if (phase == 0) cb.Start?.Invoke();
                else if (phase == 1) cb.Tick?.Invoke(seconds);
                else cb.Stop?.Invoke();
            }
            catch (Exception e) { Bridge.Report(cb.Owner, "move", e); }
        }

        /// <summary>Sends a mover toward <paramref name="location"/>, arriving after <paramref name="seconds"/>. Game thread.</summary>
        public bool MoveTo(GameObject mover, Vec3 location, float seconds)
        {
            float* at = stackalloc float[3] { location.X, location.Y, location.Z };
            return Bridge.Api->mover_move(mover.Handle, at, seconds) == 1;
        }

        /// <summary>True while a mover is on its way. Game thread.</summary>
        public bool MoverMoving(GameObject mover) => Bridge.Api->mover_moving(mover.Handle) == 1;

        /// <summary>A zipline (the game's TdZiplineVolume) from <paramref name="start"/> (the high end) to <paramref name="end"/>. Game thread.</summary>
        public GameObject Zipline(Vec3 start, Vec3 end, float landingStrip = 0f)
        {
            var d = new MiragePieceDesc { size = (uint)sizeof(MiragePieceDesc), kind = 1, landing_strip = landingStrip };
            d.location[0] = start.X; d.location[1] = start.Y; d.location[2] = start.Z;
            d.end[0] = end.X; d.end[1] = end.Y; d.end[2] = end.Z;
            return new GameObject(Bridge.Api->piece_create(Handle, &d));
        }

        private sealed record TriggerCallbacks(Mod Owner, Action<GameObject, GameObject>? Enter, Action<GameObject, GameObject>? Leave,
                                               Action<GameObject, GameObject>? Stay);

        [System.Runtime.InteropServices.UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
        private static void OnTrigger(void* user, void* trigger, void* other, int kind)
        {
            var cb = (TriggerCallbacks)System.Runtime.InteropServices.GCHandle.FromIntPtr((IntPtr)user).Target!;
            var action = kind == 0 ? cb.Enter : kind == 1 ? cb.Leave : cb.Stay;
            try { action?.Invoke(new GameObject(trigger), new GameObject(other)); }
            catch (Exception e) { Bridge.Report(cb.Owner, "trigger", e); }
        }

        /// <summary>
        /// Actors in play whose collision bounds touch the sphere (bounds, not shapes), of <paramref name="className"/> or
        /// a subclass (null: any). Game thread.
        /// </summary>
        public GameObject[] Overlap(Vec3 center, float radius, string? className = null)
        {
            float* c = stackalloc float[3] { center.X, center.Y, center.Z };
            var cls = Utf8.Intern(className ?? "");
            var n = Bridge.Api->world_overlap(c, radius, cls, null, 0);
            if (n <= 0) return System.Array.Empty<GameObject>();
            var found = new void*[n];
            fixed (void** f = found) n = System.Math.Min(n, Bridge.Api->world_overlap(c, radius, cls, f, n));
            var result = new GameObject[n];
            for (var i = 0; i < n; i++) result[i] = new GameObject(found[i]);
            return result;
        }

        /// <summary>
        /// A joint between two bodies (<paramref name="b"/> default: <paramref name="a"/> held to the world) at
        /// <paramref name="location"/>; <paramref name="hinge"/>: turns only about the joint's X axis. Removed on revert and
        /// unload. Game thread.
        /// </summary>
        public GameObject Joint(GameObject a, Vec3 location, GameObject b = default, bool hinge = false, float breakThreshold = 0f)
        {
            float* at = stackalloc float[3] { location.X, location.Y, location.Z };
            return new GameObject(Bridge.Api->physics_joint(Handle, a.Handle, b.Handle, at, null, hinge ? 1 : 0, breakThreshold));
        }

        /// <summary>The player's last 256 move transitions as JSON (api.h movement_transitions has the shape). Any thread.</summary>
        public string MovementTransitionsJson()
        {
            var n = Bridge.Api->movement_transitions(null, 0);
            var buffer = new byte[n + 1];
            uint got;
            fixed (byte* b = buffer) got = Bridge.Api->movement_transitions(b, (uint)buffer.Length);
            return System.Text.Encoding.UTF8.GetString(buffer, 0, (int)System.Math.Min(got, n));
        }

        /// <summary>What the player is doing now (the move, the previous move, velocity), or null without a player. Game thread.</summary>
        public Movement? Movement()
        {
            var m = new MirageMovement { size = (uint)sizeof(MirageMovement) };
            if (Bridge.Api->movement_get(&m) == 0) return null;
            return new Movement(m.move, m.previous_move, m.physics, m.velocity[0], m.velocity[1], m.velocity[2], m.speed,
                                new GameObject(m.move_object));
        }

        /// <summary>
        /// A test of the mod's, run by the self-test on the game thread with a level loaded and the mod on
        /// (`mirage mod test`): <paramref name="test"/> returns null when it passed, else why it failed. An exception
        /// fails it with its message.
        /// </summary>
        public void Test(string name, Func<string?> test)
        {
            var handle = System.Runtime.InteropServices.GCHandle.Alloc(test);      // Lives as long as the mod
            Owned.Add(new TestHandle(handle));
            Bridge.Api->test_register(Handle, Utf8.Intern(name), (void*)(delegate* unmanaged[Cdecl]<void*, byte*, uint, int>)&RunTest,
                                      (void*)System.Runtime.InteropServices.GCHandle.ToIntPtr(handle));
        }

        [System.Runtime.InteropServices.UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
        private static int RunTest(void* user, byte* message, uint capacity)
        {
            string? why;
            try
            {
                var test = (Func<string?>)System.Runtime.InteropServices.GCHandle.FromIntPtr((nint)user).Target!;
                why = test();
            }
            catch (Exception e)
            {
                why = e.GetType().Name + ": " + e.Message;
            }
            if (why is null) return 1;
            var bytes = System.Text.Encoding.UTF8.GetBytes(why);
            var n = (int)Math.Min((uint)bytes.Length, capacity - 1);
            for (int i = 0; i < n; i++) message[i] = bytes[i];
            message[n] = 0;
            return 0;
        }

        private sealed class TestHandle : IDisposable
        {
            private System.Runtime.InteropServices.GCHandle _h;
            public TestHandle(System.Runtime.InteropServices.GCHandle h) => _h = h;
            public void Dispose() { }       // Kept: the runtime may still hold the test until the mod is gone
        }

        /// <summary>
        /// The actors in play that match (step 14): of a class (and its subclasses), with a tag or a name, within
        /// <paramref name="radius"/> of <paramref name="near"/> or of a point (nearest first). Every filter optional.
        /// Game thread.
        /// </summary>
        public GameObject[] Actors(string? className = null, string? tag = null, string? name = null, GameObject near = default,
                                   float radius = 0f, float x = 0f, float y = 0f, float z = 0f)
        {
            var cls = System.Runtime.InteropServices.Marshal.StringToCoTaskMemUTF8(className ?? "");
            var t = System.Runtime.InteropServices.Marshal.StringToCoTaskMemUTF8(tag ?? "");
            var n = System.Runtime.InteropServices.Marshal.StringToCoTaskMemUTF8(name ?? "");
            try
            {
                var q = new MirageActorQuery
                {
                    size = (uint)sizeof(MirageActorQuery), class_name = (byte*)cls, tag = (byte*)t, name = (byte*)n,
                    near_object = near.Handle, x = x, y = y, z = z, radius = radius,
                };
                int count = Bridge.Api->actors_query(&q, null, 0);
                if (count <= 0) return Array.Empty<GameObject>();
                var found = new void*[count];
                fixed (void** p = found) count = Math.Min(count, Bridge.Api->actors_query(&q, p, count));
                var result = new GameObject[count];
                for (int i = 0; i < count; i++) result[i] = new GameObject(found[i]);
                return result;
            }
            finally
            {
                System.Runtime.InteropServices.Marshal.FreeCoTaskMem(cls);
                System.Runtime.InteropServices.Marshal.FreeCoTaskMem(t);
                System.Runtime.InteropServices.Marshal.FreeCoTaskMem(n);
            }
        }

        /// <summary>
        /// A transaction (step 15): the writes <paramref name="body"/> makes to game objects are recorded with the values
        /// they replace, and put back by <see cref="Revert"/> or when the mod unloads, reloads or crashes. Returns its id.
        /// Game thread; transactions don't nest.
        /// </summary>
        public int Transaction(string name, Action body)
        {
            var n = System.Runtime.InteropServices.Marshal.StringToCoTaskMemUTF8(name);
            int id;
            try { id = Bridge.Api->change_begin(Handle, (byte*)n); }
            finally { System.Runtime.InteropServices.Marshal.FreeCoTaskMem(n); }
            if (id == 0) throw new InvalidOperationException("a transaction is open already, or this isn't the game thread");
            try { body(); }
            finally { Bridge.Api->change_end(Handle); }
            return id;
        }

        /// <summary>Puts transaction <paramref name="id"/> back (-1: every one not reverted yet); how many values</summary>
        public int Revert(int id = -1) => Bridge.Api->change_revert(Handle, id);

        /// <summary>The mod's transactions as JSON: [{"id", "name", "values": [...], "reverted"}]</summary>
        public string ChangesJson
        {
            get
            {
                uint n = Bridge.Api->change_list(Handle, null, 0);
                var buf = new byte[n + 1];
                fixed (byte* p = buf) Bridge.Api->change_list(Handle, p, n + 1);
                return System.Text.Encoding.UTF8.GetString(buf, 0, (int)n);
            }
        }

        /// <summary>Keeps an object from the garbage collector until <see cref="Release"/> or the mod's unload.</summary>
        public bool Keep(GameObject o) => Bridge.Api->object_keep(Handle, o.Handle, 1) != 0;
        public bool Release(GameObject o) => Bridge.Api->object_keep(Handle, o.Handle, 0) != 0;

        /// <summary>A notification on screen.</summary>
        public void Notify(string text) => Utf8.With(text, t => { Bridge.Api->notify(Handle, (byte*)t); return 0; });
    }

    public sealed unsafe class Logger
    {
        private readonly void* _mod;
        internal Logger(void* mod) => _mod = mod;

        public void Debug(string text) => Write(Constants.LogDebug, text);
        public void Info(string text) => Write(Constants.LogInfo, text);
        public void Warn(string text) => Write(Constants.LogWarn, text);
        public void Error(string text) => Write(Constants.LogError, text);

        private void Write(int level, string text) =>
            Utf8.With(text, t => { Bridge.Api->log(_mod, level, (byte*)t); return 0; });
    }
}

namespace Mirage
{
    /// <summary>A class for <see cref="Mod.DefineClass"/>.</summary>
    public sealed class ClassSpec
    {
        public string Name { get; set; } = "";
        public string Extends { get; set; } = "";
        public bool Placeable { get; set; }
        /// <summary>Declarations: "float Strength", "var() int Count".</summary>
        public List<string> Vars { get; set; } = new();
        /// <summary>"Strength=1500.0"</summary>
        public List<string> Defaults { get; set; } = new();
        /// <summary>An override by name ("Touch") or a new function by declaration, with its code.</summary>
        public Dictionary<string, Action<Call>> Functions { get; set; } = new();
        /// <summary>More UnrealScript, as written.</summary>
        public string Extra { get; set; } = "";
    }
}

namespace Mirage
{
    /// <summary>A detour of native code (<see cref="Mod.Detour"/>).</summary>
    public sealed unsafe class NativeDetour : IDisposable
    {
        private void* _handle;
        private readonly nint* _original;       // Native memory the runtime keeps right as the chain changes

        private NativeDetour(void* handle, nint* original)
        {
            _handle = handle;
            _original = original;
        }

        /// <summary>What the detour calls to go on: the next detour, or the real function.</summary>
        public nint Original => *_original;

        internal static NativeDetour? Create(Mod mod, nint target, string? symbol, nint detour, int priority)
        {
            var original = (nint*)System.Runtime.InteropServices.NativeMemory.AllocZeroed((nuint)sizeof(nint));  // Never freed: a call may still read it
            void* h = symbol is null
                ? Bridge.Api->detour_create(mod.Handle, (void*)target, (void*)detour, priority, (void**)original)
                : Bridge.Api->detour_symbol(mod.Handle, Utf8.Intern(symbol), (void*)detour, priority, (void**)original);
            if (h == null) return null;
            var d = new NativeDetour(h, original);
            mod.Owned.Add(d);
            return d;
        }

        public void Dispose()
        {
            if (_handle == null) return;
            Bridge.Api->detour_remove(_handle);
            _handle = null;
        }
    }

    /// <summary>A mid-function hook (<see cref="Mod.MidHook"/>).</summary>
    public sealed unsafe class NativeMidHook : IDisposable
    {
        private void* _handle;
        internal NativeMidHook(void* handle) => _handle = handle;

        public void Dispose()
        {
            if (_handle == null) return;
            Bridge.Api->mid_hook_remove(_handle);
            _handle = null;
        }
    }
}

namespace Mirage
{
    /// <summary>What the player is doing (<see cref="Mod.Movement"/>): EMovement values (the SDK's TdGame enum).</summary>
    public readonly record struct Movement(int Move, int PreviousMove, int Physics, float VelocityX, float VelocityY,
                                           float VelocityZ, float Speed, GameObject MoveObject);
}

namespace Mirage
{
    /// <summary>A vector in Unreal units</summary>
    public readonly record struct Vec3(float X, float Y, float Z);

    /// <summary>A rigid body's state (<see cref="Mod.Body"/>)</summary>
    public readonly record struct Body(Vec3 Location, int Pitch, int Yaw, int Roll, Vec3 Velocity, Vec3 Angular,
                                       float MassScale, float GravityScale, bool Awake, bool Simulated);

    /// <summary>What a trace hit (<see cref="Mod.Trace"/>)</summary>
    public readonly record struct TraceHit(GameObject Actor, Vec3 Location, Vec3 Normal, float Time, GameObject PhysMaterial);
}
