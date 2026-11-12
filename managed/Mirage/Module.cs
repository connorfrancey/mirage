using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Mirage.Native;

namespace Mirage
{
    [Flags]
    public enum ModuleFlags : uint
    {
        None = 0,
        Action = Constants.ModuleAction,                  // A button: OnTrigger, no on/off state
        EnabledByDefault = Constants.ModuleEnabledByDefault,
        EveryTick = Constants.ModuleEveryTick,            // OnTick never deferred when the menu is busy
        PanelOpen = Constants.ModulePanelOpen,
    }

    /// <summary>
    /// A row in the menu, like a native or Lua module: derive from it and override what it does. Callbacks run on the
    /// game thread (OnTick, OnEnable, OnDisable, OnTrigger) or the render thread (OnRender, OnPanel). An exception in
    /// one is logged with the mod's id and switches the module off: it never reaches the game.
    /// </summary>
    public abstract unsafe class Module : IDisposable
    {
        private GCHandle _self;
        internal void* Handle;
        public Mod Mod { get; }
        public string Name { get; }

        protected Module(Mod mod, string name, string tab = "Mods", string description = "", ModuleFlags flags = ModuleFlags.None,
                         int order = 100, int key = 0)
        {
            Mod = mod;
            Name = name;
            _self = GCHandle.Alloc(this);
            var desc = new MirageModuleDesc
            {
                size = (uint)sizeof(MirageModuleDesc),
                name = Utf8.Intern(name),
                tab = Utf8.Intern(tab),
                description = Utf8.Intern(description),
                order = order,
                key = key,
                flags = (uint)flags,
                user = (void*)GCHandle.ToIntPtr(_self),
                on_enable = (delegate* unmanaged[Cdecl]<void*, void>)&EnableThunk,
                on_disable = (delegate* unmanaged[Cdecl]<void*, void>)&DisableThunk,
                on_tick = (delegate* unmanaged[Cdecl]<void*, void>)&TickThunk,
                on_render = (delegate* unmanaged[Cdecl]<void*, void>)&RenderThunk,
                on_trigger = (delegate* unmanaged[Cdecl]<void*, void>)&TriggerThunk,
                on_panel = (delegate* unmanaged[Cdecl]<void*, void>)&PanelThunk,
            };
            Handle = Bridge.Api->module_register(mod.Handle, &desc);
            if (Handle == null) {
                _self.Free();
                throw new InvalidOperationException($"the module '{name}' could not be registered (see the log)");
            }
            mod.Owned.Add(this);
        }

        public bool Enabled
        {
            get => Bridge.Api->module_enabled(Handle) != 0;
            set => Bridge.Api->module_set_enabled(Handle, value ? 1 : 0);
        }

        protected virtual void OnEnable() { }
        protected virtual void OnDisable() { }
        protected virtual void OnTick() { }
        protected virtual void OnRender() { }
        protected virtual void OnTrigger() { }
        protected virtual void OnPanel() { }

        // ---- Settings (shown in the module's panel, saved with the menu's config) ---------------------------------------
        protected Setting Checkbox(string name, bool value, string tooltip = "") =>
            new(Bridge.Api->setting_checkbox(Handle, Utf8.Intern(name), value ? 1 : 0, Utf8.Intern(tooltip)));
        protected Setting Slider(string name, float value, float min, float max, string format = "%.2f", string tooltip = "") =>
            new(Bridge.Api->setting_slider(Handle, Utf8.Intern(name), value, min, max, Utf8.Intern(format), Utf8.Intern(tooltip)));
        protected Setting SliderInt(string name, int value, int min, int max, string tooltip = "") =>
            new(Bridge.Api->setting_slider_int(Handle, Utf8.Intern(name), value, min, max, Utf8.Intern(tooltip)));
        protected Setting Key(string name, int virtualKey, string tooltip = "") =>
            new(Bridge.Api->setting_key(Handle, Utf8.Intern(name), virtualKey, Utf8.Intern(tooltip)));

        public void Dispose()
        {
            // The runtime unbinds the module itself at unload; here only the handle that kept this object reachable goes
            if (_self.IsAllocated) _self.Free();
        }

        // ---- Callbacks from the runtime: never let an exception through ---------------------------------------------------
        private static Module? From(void* user)
        {
            var target = GCHandle.FromIntPtr((IntPtr)user).Target;
            return target as Module;
        }

        private static void Guard(void* user, Action<Module> call, [CallerMemberName] string what = "")
        {
            Module? m = null;
            try {
                m = From(user);
                if (m != null) call(m);
            } catch (Exception e) {
                Bridge.Report(m?.Mod, $"{m?.Name ?? "a module"}: {what}", e);
                if (m != null) {
                    try { Bridge.Api->module_set_enabled(m.Handle, 0); } catch { /* Never past this point */ }
                }
            }
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void EnableThunk(void* user) => Guard(user, m => m.OnEnable(), "OnEnable");
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void DisableThunk(void* user)
        {
            // Not switched off again from here (it is going off): just reported
            try { From(user)?.OnDisable(); } catch (Exception e) { Bridge.Report(From(user)?.Mod, "OnDisable", e); }
        }
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void TickThunk(void* user) => Guard(user, m => m.OnTick(), "OnTick");
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void RenderThunk(void* user) => Guard(user, m => m.OnRender(), "OnRender");
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void TriggerThunk(void* user) => Guard(user, m => m.OnTrigger(), "OnTrigger");
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void PanelThunk(void* user) => Guard(user, m => m.OnPanel(), "OnPanel");
    }

    /// <summary>A control in a module's panel.</summary>
    public readonly unsafe struct Setting
    {
        private readonly void* _h;
        internal Setting(void* handle) => _h = handle;
        public bool IsValid => _h != null;
        public bool Bool { get => Bridge.Api->setting_get_int(_h) != 0; set => Bridge.Api->setting_set_int(_h, value ? 1 : 0); }
        public int Int { get => Bridge.Api->setting_get_int(_h); set => Bridge.Api->setting_set_int(_h, value); }
        public float Float { get => Bridge.Api->setting_get_float(_h); set => Bridge.Api->setting_set_float(_h, value); }
        /// <summary>A key setting: held down right now.</summary>
        public bool Down => Bridge.Api->key_down(_h) != 0;
        public bool Visible { set => Bridge.Api->setting_set_visible(_h, value ? 1 : 0); }
    }
}
