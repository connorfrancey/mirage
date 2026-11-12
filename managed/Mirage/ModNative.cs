using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Mirage.Native;

namespace Mirage
{
    /// <summary>A mod native (step 11): the game's bytecode calling C#. See <see cref="Mod.Native"/>.</summary>
    internal sealed unsafe class ModNative : IDisposable
    {
        private GCHandle _self;
        private readonly Mod _mod;
        private readonly string _name;
        private readonly Func<object?[], object?> _fn;

        private ModNative(Mod mod, string name, Func<object?[], object?> fn)
        {
            _mod = mod;
            _name = name;
            _fn = fn;
        }

        internal static int Register(Mod mod, string name, string signature, Func<object?[], object?> fn)
        {
            var n = new ModNative(mod, name, fn);
            n._self = GCHandle.Alloc(n);
            int index = Bridge.Api->native_register(mod.Handle, Utf8.Intern(name), Utf8.Intern(signature),
                                                    (delegate* unmanaged[Cdecl]<void*, void*, void>)&Thunk, (void*)GCHandle.ToIntPtr(n._self));
            if (index == 0) {
                n._self.Free();
                throw new InvalidOperationException($"the native {name} {signature} can't be registered (see the log)");
            }
            mod.Owned.Add(n);
            return index;
        }

        // The runtime stops calling it at the mod's unload; the handle goes with the mod's other resources
        public void Dispose()
        {
            if (_self.IsAllocated) _self.Free();
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void Thunk(void* user, void* call)
        {
            ModNative? n = null;
            try {
                n = GCHandle.FromIntPtr((IntPtr)user).Target as ModNative;
                if (n == null) return;
                var args = new List<object?>();
                MirageValue v;
                for (int i = 0; Bridge.Api->native_arg(call, i, &v) != 0; ++i) {
                    args.Add(v.type switch {
                        Constants.ValueBool => v.i != 0,
                        Constants.ValueInt => v.i,
                        Constants.ValueFloat => v.f,
                        Constants.ValueObject => new GameObject(v.o),
                        Constants.ValueText => Utf8.Read(v.text),
                        _ => null,
                    });
                }
                object? result = n._fn(args.ToArray());
                var r = new MirageValue();
                switch (result) {
                    case bool b: r.type = Constants.ValueBool; r.i = b ? 1 : 0; break;
                    case int i: r.type = Constants.ValueInt; r.i = i; break;
                    case byte by: r.type = Constants.ValueInt; r.i = by; break;
                    case float f: r.type = Constants.ValueFloat; r.f = f; break;
                    case double d: r.type = Constants.ValueFloat; r.f = (float)d; break;
                    case GameObject o: r.type = Constants.ValueObject; r.o = o.Handle; break;
                    default: return;
                }
                Bridge.Api->native_return(call, &r);
            } catch (Exception ex) {
                Bridge.Report(n?._mod, "native " + (n?._name ?? "?"), ex);
            }
        }
    }
}
