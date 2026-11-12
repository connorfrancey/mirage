using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
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
