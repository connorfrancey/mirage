using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Emit;
using Microsoft.CodeAnalysis.Text;

namespace Mirage
{
    /// <summary>
    /// C# scripts: a mod's .cs files compiled in the game (Roslyn), into memory, with a portable PDB so errors and
    /// stack traces have file and line. What a script mod is: mod.toml with <c>dotnet = "main.cs"</c> (or just a
    /// main.cs), every .cs file in its folder compiled together.
    /// </summary>
    internal static class Scripts
    {
        private static readonly object Lock = new();
        private static List<MetadataReference>? _references;

        // The framework and Mirage.dll: what every script may use
        private static List<MetadataReference> References()
        {
            lock (Lock) {
                if (_references != null) return _references;
                var list = new List<MetadataReference>();
                var tpa = (AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES") as string ?? "").Split(Path.PathSeparator);
                foreach (var path in tpa)
                    if (path.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) && File.Exists(path))
                        list.Add(MetadataReference.CreateFromFile(path));
                list.Add(MetadataReference.CreateFromFile(typeof(Bridge).Assembly.Location));
                _references = list;
                return list;
            }
        }

        private const string GlobalUsings =
            "global using System;\nglobal using System.Collections.Generic;\nglobal using System.Linq;\nglobal using Mirage;\n";

        public sealed class Result
        {
            public byte[]? Assembly, Pdb;
            public List<string> Errors = new();      // "main.cs(12,5): error CS1002: ; expected"
            public List<string> Warnings = new();
            public double Milliseconds;
            public string? CacheKey;                 // Taken from the script cache (its entry), else null
        }

        // ---- The script cache -------------------------------------------------------------------------------------
        // A compiled script is kept in <mirage>\cache\scripts\ by what it was compiled from: its files (their paths and
        // bytes), the assembly's name, this Mirage.dll's build (its module id: the API scripts compile against), Roslyn's
        // and .NET's versions. The same sources load the kept assembly instead of compiling (a script mod's start goes
        // from a second or two to milliseconds); any change is another key, compiled and kept. Only what compiled is
        // kept, with its warnings; the newest MaxEntries stay. A broken entry is deleted and compiled again (Forget).

        private const string CacheVersion = "mirage-script-cache 1";
        private const int MaxEntries = 64;

        // <mirage>\cache\scripts (Mirage.dll is in <mirage>\dotnet\mirage); null when that isn't known
        private static string? CacheDir()
        {
            string location = typeof(Bridge).Assembly.Location;
            if (string.IsNullOrEmpty(location)) return null;
            string? mirage = Path.GetDirectoryName(Path.GetDirectoryName(Path.GetDirectoryName(location)));
            return mirage == null ? null : Path.Combine(mirage, "cache", "scripts");
        }

        private static string CacheKey(string name, IEnumerable<string> files)
        {
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            void Add(string text) => hash.AppendData(Encoding.UTF8.GetBytes(text + "\0"));
            Add(CacheVersion);
            Add(name);
            Add(typeof(Bridge).Assembly.ManifestModule.ModuleVersionId.ToString());
            Add(typeof(CSharpCompilation).Assembly.GetName().Version?.ToString() ?? "");
            Add(Environment.Version.ToString());
            foreach (var file in files) {
                Add(Path.GetFullPath(file));
                hash.AppendData(File.ReadAllBytes(file));
                Add("");
            }
            return Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant();
        }

        /// <summary>Compile, through the script cache: the kept assembly when the same sources were compiled before,
        /// else compiled now (and kept when it compiled).</summary>
        public static Result CompileCached(string name, IReadOnlyList<string> files)
        {
            var started = DateTime.UtcNow;
            string? dir = CacheDir();
            string? key = null;
            try {
                if (dir != null) key = CacheKey(name, files);
            } catch (IOException) {
                key = null;             // A file it can't read now: compiling says which
            }
            if (dir != null && key != null) {
                string dll = Path.Combine(dir, key + ".dll");
                try {
                    if (File.Exists(dll)) {
                        var hit = new Result { Assembly = File.ReadAllBytes(dll), CacheKey = key };
                        string pdb = Path.Combine(dir, key + ".pdb"), warnings = Path.Combine(dir, key + ".warnings.txt");
                        if (File.Exists(pdb)) hit.Pdb = File.ReadAllBytes(pdb);
                        if (File.Exists(warnings)) hit.Warnings.AddRange(File.ReadAllLines(warnings));
                        File.SetLastWriteTimeUtc(dll, DateTime.UtcNow);     // In use: the newest are what stays
                        hit.Milliseconds = (DateTime.UtcNow - started).TotalMilliseconds;
                        return hit;
                    }
                } catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) {
                    Forget(key);
                }
            }
            var result = Compile(name, files);
            if (result.Assembly != null && dir != null && key != null) {
                try {
                    Directory.CreateDirectory(dir);
                    // The assembly last: an entry is whole once its .dll is there
                    if (result.Pdb != null) Write(Path.Combine(dir, key + ".pdb"), result.Pdb);
                    File.WriteAllLines(Path.Combine(dir, key + ".warnings.txt"), result.Warnings);
                    Write(Path.Combine(dir, key + ".dll"), result.Assembly);
                    Trim(dir);
                } catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) {
                    // Not kept: compiled again next time
                }
            }
            return result;
        }

        /// <summary>The entry is broken (its assembly didn't load): gone, so the next compile replaces it.</summary>
        public static void Forget(string key)
        {
            string? dir = CacheDir();
            if (dir == null) return;
            foreach (var ext in new[] { ".dll", ".pdb", ".warnings.txt" }) {
                try { File.Delete(Path.Combine(dir, key + ext)); }
                catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { }
            }
        }

        private static void Write(string path, byte[] bytes)
        {
            string temp = path + ".tmp";
            File.WriteAllBytes(temp, bytes);
            File.Move(temp, path, overwrite: true);
        }

        // The newest MaxEntries entries stay
        private static void Trim(string dir)
        {
            var old = new DirectoryInfo(dir).GetFiles("*.dll").OrderByDescending(f => f.LastWriteTimeUtc).Skip(MaxEntries).ToList();
            foreach (var f in old) Forget(Path.GetFileNameWithoutExtension(f.Name));
        }

        public static IEnumerable<string> SourcesIn(string folder) =>
            Directory.EnumerateFiles(folder, "*.cs", SearchOption.AllDirectories)
                     .Where(f => !f.Contains(Path.DirectorySeparatorChar + "obj" + Path.DirectorySeparatorChar) &&
                                 !f.Contains(Path.DirectorySeparatorChar + "bin" + Path.DirectorySeparatorChar) &&
                                 !f.Contains(Path.DirectorySeparatorChar + ".mirage" + Path.DirectorySeparatorChar))
                     .OrderBy(f => f, StringComparer.OrdinalIgnoreCase);

        /// <summary>Compiles `files` (paths) as one assembly named `name`. `overrides`: text to use instead of a file's
        /// contents (the editor checking unsaved text).</summary>
        public static Result Compile(string name, IEnumerable<string> files, IReadOnlyDictionary<string, string>? overrides = null)
        {
            var started = DateTime.UtcNow;
            var result = new Result();
            var parse = new CSharpParseOptions(LanguageVersion.Latest);
            var trees = new List<SyntaxTree> { CSharpSyntaxTree.ParseText(GlobalUsings, parse, "GlobalUsings.g.cs", Encoding.UTF8) };
            foreach (var file in files) {
                string text = overrides != null && overrides.TryGetValue(file, out var o) ? o : File.ReadAllText(file);
                trees.Add(CSharpSyntaxTree.ParseText(SourceText.From(text, Encoding.UTF8), parse, file));
            }
            var options = new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, optimizationLevel: OptimizationLevel.Release,
                                                       nullableContextOptions: NullableContextOptions.Enable, allowUnsafe: false);
            var compilation = CSharpCompilation.Create(name, trees, References(), options);
            using var dll = new MemoryStream();
            using var pdb = new MemoryStream();
            EmitResult emitted = compilation.Emit(dll, pdb, options: new EmitOptions(debugInformationFormat: DebugInformationFormat.PortablePdb));
            foreach (var d in emitted.Diagnostics) {
                if (d.Severity == DiagnosticSeverity.Error) result.Errors.Add(Format(d));
                else if (d.Severity == DiagnosticSeverity.Warning) result.Warnings.Add(Format(d));
            }
            if (emitted.Success) {
                result.Assembly = dll.ToArray();
                result.Pdb = pdb.ToArray();
            }
            result.Milliseconds = (DateTime.UtcNow - started).TotalMilliseconds;
            return result;
        }

        private static string Format(Diagnostic d)
        {
            var span = d.Location.GetLineSpan();
            var file = Path.GetFileName(span.Path);
            return $"{file}({span.StartLinePosition.Line + 1},{span.StartLinePosition.Character + 1}): " +
                   $"{(d.Severity == DiagnosticSeverity.Error ? "error" : "warning")} {d.Id}: {d.GetMessage()}";
        }
    }
}
