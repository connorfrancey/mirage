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
