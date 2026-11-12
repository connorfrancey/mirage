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
