using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;
using Mirage.Native;

namespace Mirage
{
    /// <summary>
    /// What the menu's runtime calls (src/runtime/mod_dotnet.cpp). Not for mods. Every entry point catches everything:
    /// an exception may never cross into native code.
    /// </summary>
    public static unsafe class Bridge
    {
        internal static MirageApi* Api;
        private static readonly object Lock = new();
        private static readonly Dictionary<IntPtr, Loaded> Mods = new();
        private static readonly List<WeakReference> Unloading = new();    // Contexts asked to unload: for the diagnostics

        private sealed class Loaded
        {
            public ModContext Context = null!;
            public Mod Instance = null!;
            public string Id = "";
        }

        /// <summary>A mod's own load context: its assembly and dependencies from its folder, Mirage itself shared.</summary>
        private sealed class ModContext : AssemblyLoadContext
        {
            private readonly string _folder;
            public ModContext(string name, string folder) : base(name, isCollectible: true) => _folder = folder;

            protected override Assembly? Load(AssemblyName name)
            {
                if (name.Name == "Mirage") return typeof(Bridge).Assembly;        // One API for every mod
                var path = Path.Combine(_folder, name.Name + ".dll");
                return File.Exists(path) ? LoadFromBytes(path) : null;           // Else: the framework's
            }

            public Assembly LoadBytes(byte[] dll, byte[]? pdb)
            {
                using var d = new MemoryStream(dll);
                if (pdb == null) return LoadFromStream(d);
                using var p = new MemoryStream(pdb);
                return LoadFromStream(d, p);
            }

            // From memory: the files stay unlocked (a rebuild while the game runs is picked up by hot reload)
            public Assembly LoadFromBytes(string path)
            {
                using var dll = new MemoryStream(File.ReadAllBytes(path));
                var pdbPath = Path.ChangeExtension(path, ".pdb");
                if (!File.Exists(pdbPath)) return LoadFromStream(dll);
                using var pdb = new MemoryStream(File.ReadAllBytes(pdbPath));
                return LoadFromStream(dll, pdb);
            }
        }

        /// <summary>
        /// Called once. `apiSize`: sizeof(MirageApi) in the runtime. Returns 1 when the runtime's table has every entry
        /// this assembly was built against (else 0: written to `error`).
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int Initialize(MirageApi* api, int apiSize, byte* error, int capacity)
        {
            try {
                if (sizeof(MirageApi) > apiSize) {
                    WriteError($"Mirage.dll needs a runtime with {sizeof(MirageApi)} bytes of API, this one has {apiSize}", error, capacity);
                    return 0;
                }
                if (sizeof(MirageModuleDesc) != 56 || sizeof(MirageEvent) != 24 || sizeof(MirageValue) != 272) {
                    WriteError($"struct layouts differ from api.h (desc {sizeof(MirageModuleDesc)}, event {sizeof(MirageEvent)}, value {sizeof(MirageValue)})",
                               error, capacity);
                    return 0;
                }
                Api = api;              // Roslyn loads only when a script compiles: a DLL-only mod never pays for it
                return 1;
            } catch (Exception e) {
                WriteError(e.ToString(), error, capacity);
                return 0;
            }
        }

        /// <summary>Loads a mod's assembly and starts it. Returns 1 on success, else 0 with the reason in `error`.</summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int LoadMod(void* mod, byte* assemblyPath, byte* error, int capacity)
        {
            try {
                string path = Utf8.Read(assemblyPath);
                string id = Utf8.Read(Api->mod_id(mod));
                var context = new ModContext("mod " + id, Path.GetDirectoryName(path)!);
                Assembly assembly;
                try {
                    if (path.EndsWith(".cs", StringComparison.OrdinalIgnoreCase)) {
                        // A script mod: every .cs in its folder, compiled now
                        var compiled = Scripts.Compile("mod_" + id.Replace('-', '_'), Scripts.SourcesIn(Path.GetDirectoryName(path)!));
                        if (compiled.Assembly == null) {
                            context.Unload();
                            WriteError(string.Join("\n", compiled.Errors.Take(20)), error, capacity);
                            return 0;
                        }
                        foreach (var w in compiled.Warnings.Take(10)) Utf8.With(w, t => { Api->log(mod, Constants.LogWarn, (byte*)t); return 0; });
                        Utf8.With($"compiled in {compiled.Milliseconds:0} ms", t => { Api->log(mod, Constants.LogInfo, (byte*)t); return 0; });
                        assembly = context.LoadBytes(compiled.Assembly, compiled.Pdb);
                    } else {
                        assembly = context.LoadFromBytes(path);
                    }
                } catch (Exception e) {
                    context.Unload();
                    WriteError($"{Path.GetFileName(path)} could not be loaded: {e.Message}", error, capacity);
                    return 0;
                }
                var types = assembly.GetTypes().Where(t => typeof(Mod).IsAssignableFrom(t) && !t.IsAbstract).ToList();
                if (types.Count != 1) {
                    context.Unload();
                    WriteError(types.Count == 0 ? "no class derives from Mirage.Mod" : "more than one class derives from Mirage.Mod: " +
                               string.Join(", ", types.Select(t => t.FullName)), error, capacity);
                    return 0;
                }
                var instance = (Mod)Activator.CreateInstance(types[0])!;
                instance.Handle = mod;
                instance.Id = id;
                instance.Version = Utf8.Read(Api->mod_version(mod));
                instance.Folder = Utf8.Read(Api->mod_dir(mod));
                instance.Log = new Logger(mod);
                try {
                    instance.OnLoad();
                } catch (Exception e) {
                    Release(instance);
                    context.Unload();
                    WriteError($"OnLoad threw {e.GetType().Name}: {e.Message}", error, capacity);
                    Report(instance, "OnLoad", e);
                    return 0;
                }
                lock (Lock) Mods[(IntPtr)mod] = new Loaded { Context = context, Instance = instance, Id = id };
                return 1;
            } catch (Exception e) {
                WriteError(e.ToString(), error, capacity);
                return 0;
            }
        }

        /// <summary>Stops a mod and unloads its assembly (the runtime has unbound its modules and removed its events).</summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static void UnloadMod(void* mod)
        {
            try {
                Loaded? loaded;
                lock (Lock) {
                    Mods.Remove((IntPtr)mod, out loaded);
                }
                if (loaded == null) return;
                try {
                    loaded.Instance.OnUnload();
                } catch (Exception e) {
                    Report(loaded.Instance, "OnUnload", e);
                }
                Release(loaded.Instance);
                var weak = new WeakReference(loaded.Context);
                loaded.Context.Unload();
                lock (Lock) Unloading.Add(weak);
            } catch {
                // Never into native code
            }
        }

        /// <summary>Contexts asked to unload that are still alive (a mod kept a reference somewhere: a leak). Runs a GC.</summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int StillLoaded()
        {
            try {
                for (int i = 0; i < 3; ++i) {
                    GC.Collect();
                    GC.WaitForPendingFinalizers();
                }
                lock (Lock) {
                    Unloading.RemoveAll(w => !w.IsAlive);
                    return Unloading.Count;
                }
            } catch {
                return -1;
            }
        }

        /// <summary>
        /// Compiles a script mod's folder without loading it (the editor's check). `file`/`text`: one file's unsaved text
        /// in place of what is on disk (may be null). Writes the diagnostics, one per line; returns how many are errors
        /// (-1: the check itself failed).
        /// </summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static int CheckScripts(byte* folder, byte* file, byte* text, byte* output, int capacity)
        {
            try {
                string dir = Utf8.Read(folder);
                Dictionary<string, string>? overrides = null;
                if (file != null && text != null) overrides = new() { [Path.GetFullPath(Utf8.Read(file))] = Utf8.Read(text) };
                var files = Scripts.SourcesIn(dir).Select(Path.GetFullPath).ToList();
                if (overrides != null) foreach (var f in overrides.Keys) if (!files.Contains(f, StringComparer.OrdinalIgnoreCase)) files.Add(f);
                var r = Scripts.Compile("check", files, overrides);
                WriteError(string.Join("\n", r.Errors.Concat(r.Warnings).Take(50)) + $"\n({r.Milliseconds:0} ms)", output, capacity);
                return r.Errors.Count;
            } catch (Exception e) {
                WriteError(e.Message, output, capacity);
                return -1;
            }
        }

        /// <summary>Managed memory (bytes in the GC heap) for the memory report.</summary>
        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        public static long ManagedBytes()
        {
            try {
                return GC.GetTotalMemory(false);
            } catch {
                return -1;
            }
        }

        private static void Release(Mod instance)
        {
            foreach (var owned in instance.Owned) {
                try {
                    owned.Dispose();
                } catch {
                    // Keep releasing the others
                }
            }
            instance.Owned.Clear();
        }

        internal static void Report(Mod? mod, string where, Exception e)
        {
            try {
                var text = $"{where} threw {e.GetType().Name}: {e.Message}\n{e.StackTrace}";
                Utf8.With(text, t => { Api->log(mod == null ? null : mod.Handle, Constants.LogError, (byte*)t); return 0; });
            } catch {
                // Logging failed: nothing more to do
            }
        }

        private static void WriteError(string text, byte* buffer, int capacity)
        {
            if (buffer == null || capacity <= 0) return;
            var bytes = System.Text.Encoding.UTF8.GetBytes(text);
            int n = Math.Min(bytes.Length, capacity - 1);
            Marshal.Copy(bytes, 0, (IntPtr)buffer, n);
            buffer[n] = 0;
        }
    }
}
