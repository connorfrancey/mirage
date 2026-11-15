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
