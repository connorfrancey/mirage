using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Mirage.Native;

namespace Mirage
{
    public enum GameEvent
    {
        Tick = Constants.EventTick,                    // Game thread, every tick in a level: Number = seconds since the last
        LevelLoad = Constants.EventLevelLoad,          // Value 0: a load starts, 1: it finished; Text = the map
        Death = Constants.EventDeath,                  // Value 0: the player died (before the game handles it), 1: after
        PlayerState = Constants.EventPlayerState,      // The move state changed: Value = new, Value2 = old
        Key = Constants.EventKey,                      // A key went down: Value = virtual-key code
        StateRestored = Constants.EventStateRestored,  // The TAS put back a memory snapshot: look objects up again
        MemoryPressure = Constants.EventMemoryPressure, // Value = 0 normal .. 3 critical: give back what can be made again
        ActorAdded = Constants.EventActorAdded,        // An actor came into play: Object = it, Text = its class
        ActorRemoved = Constants.EventActorRemoved,    // An actor left play: Object = the one it was (compare, never use)
        LevelStreamed = Constants.EventLevelStreamed,  // A streaming level shown (Value 1) or hidden (0): Text = its name
        Restart = Constants.EventRestart,              // A checkpoint restart: a new player pawn in the same level
    }

    public readonly struct EventArgs
    {
        public GameEvent Type { get; init; }
        public int Value { get; init; }
        public int Value2 { get; init; }
        public float Number { get; init; }
        public string Text { get; init; }
        /// <summary>The event's object (actor events); for ActorRemoved only to compare with objects kept</summary>
        public GameObject Object { get; init; }
    }

    /// <summary>An event subscription: ends when disposed or when the mod unloads.</summary>
    public sealed unsafe class Subscription : IDisposable
    {
        private GCHandle _self;
        private void* _handle;
        private readonly Mod _mod;
        private readonly Action<EventArgs> _handler;

        private Subscription(Mod mod, Action<EventArgs> handler)
        {
            _mod = mod;
            _handler = handler;
        }

        internal static Subscription Create(Mod mod, GameEvent type, Action<EventArgs> handler)
        {
            var s = new Subscription(mod, handler);
            s._self = GCHandle.Alloc(s);
            s._handle = Bridge.Api->event_subscribe(mod.Handle, (int)type, (delegate* unmanaged[Cdecl]<void*, MirageEvent*, void>)&Thunk,
                                                   (void*)GCHandle.ToIntPtr(s._self));
            if (s._handle == null) {
                s._self.Free();
                throw new InvalidOperationException($"{type} can't be subscribed to");
            }
            mod.Owned.Add(s);
            return s;
        }

        public void Dispose()
        {
            if (_handle != null) Bridge.Api->event_unsubscribe(_handle);
            _handle = null;
            if (_self.IsAllocated) _self.Free();
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void Thunk(void* user, MirageEvent* e)
        {
            Subscription? s = null;
            try {
                s = GCHandle.FromIntPtr((IntPtr)user).Target as Subscription;
                if (s == null || s._handle == null) return;
                void* o = e->size >= sizeof(MirageEvent) ? e->@object : null;
                s._handler(new EventArgs { Type = (GameEvent)e->type, Value = e->value, Value2 = e->value2, Number = e->number,
                                           Text = Utf8.Read(e->text), Object = new GameObject(o) });
            } catch (Exception ex) {
                Bridge.Report(s?._mod, "event " + (GameEvent)e->type, ex);
            }
        }
    }

    public enum HookWhen { Before = Constants.HookBefore, After = Constants.HookAfter, Replace = Constants.HookReplace }

    /// <summary>A hook on a game function: ends when disposed or when the mod unloads.</summary>
    public sealed unsafe class Hook : IDisposable
    {
        private GCHandle _self;
        private void* _handle;
        private readonly Mod _mod;
        private readonly Action<Call> _handler;
        public string Function { get; }

        private Hook(Mod mod, string function, Action<Call> handler)
        {
            _mod = mod;
            Function = function;
            _handler = handler;
        }

        internal static Hook Create(Mod mod, string function, HookWhen when, int priority, Action<Call> handler)
        {
            var h = new Hook(mod, function, handler);
            h._self = GCHandle.Alloc(h);
            h._handle = Bridge.Api->hook_function(mod.Handle, Utf8.Intern(function), (int)when, priority,
                                                  (delegate* unmanaged[Cdecl]<void*, void*, void>)&Thunk, (void*)GCHandle.ToIntPtr(h._self));
            if (h._handle == null) {
                h._self.Free();
                throw new InvalidOperationException($"{function} can't be hooked (see the log)");
            }
            mod.Owned.Add(h);
            return h;
        }

        /// <summary>Script injection (step 11): see <see cref="Mod.Inject"/>.</summary>
        internal static Hook CreateInjection(Mod mod, string function, string point, int priority, Action<Call> handler)
        {
            var h = new Hook(mod, function, handler);
            h._self = GCHandle.Alloc(h);
            h._handle = Bridge.Api->inject_function(mod.Handle, Utf8.Intern(function), Utf8.Intern(point), priority,
                                                    (delegate* unmanaged[Cdecl]<void*, void*, void>)&Thunk, (void*)GCHandle.ToIntPtr(h._self));
            if (h._handle == null) {
                h._self.Free();
                throw new InvalidOperationException($"{function} at {point} can't be injected (see the log)");
            }
            mod.Owned.Add(h);
            return h;
        }

        public void Dispose()
        {
            if (_handle != null) Bridge.Api->hook_remove(_handle);
            _handle = null;
            if (_self.IsAllocated) _self.Free();
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void Thunk(void* user, void* call)
        {
            Hook? h = null;
            try {
                h = GCHandle.FromIntPtr((IntPtr)user).Target as Hook;
                if (h == null || h._handle == null) return;
                h._handler(new Call(call));
            } catch (Exception ex) {
                Bridge.Report(h?._mod, "hook " + (h?.Function ?? "?"), ex);
            }
        }
    }

    /// <summary>A call of a game function, as a hook sees it (valid only during the hook).</summary>
    public readonly unsafe struct Call
    {
        private readonly void* _c;
        internal Call(void* call) => _c = call;
        public GameObject Object => new(Bridge.Api->call_object(_c));
        public string Function => Utf8.Read(Bridge.Api->call_function(_c));
        /// <summary>A before hook: the function doesn't run.</summary>
        public void Skip() => Bridge.Api->call_skip(_c);
        public Value Get(string parameter)
        {
            MirageValue v;
            return Bridge.Api->call_get(_c, Utf8.Intern(parameter), &v) != 0 ? new Value(v) : default;
        }
        public bool Set(string parameter, Value value)
        {
            var v = value.ToNative();
            return Bridge.Api->call_set(_c, Utf8.Intern(parameter), &v) != 0;
        }
    }
}
