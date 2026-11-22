import json
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from click.testing import CliRunner

from mirage_apigen import DEFAULT_CONFIG, DEFAULT_INPUT
from mirage_apigen.artifacts import apply_plan, output_lock, plan_output
from mirage_apigen.browser import BrowserMetadata
from mirage_apigen.cli import api
from mirage_apigen.pipeline import generate
from test_luals import RECORDS


class GenerationTests(unittest.TestCase):
    def test_cpp_and_markdown_goldens(self):
        result = generate(DEFAULT_INPUT, DEFAULT_CONFIG, ("cpp", "docs"))
        golden = Path(__file__).parent / "golden"
        for name in ("cpp/package-ALAudio.cpp", "docs/type-ALAudio.ALAudioDevice.md"):
            self.assertEqual(result.files[name], (golden / Path(name).name).read_text(encoding="utf-8"))

    def test_incremental_and_obsolete_files(self):
        with TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            result = generate(DEFAULT_INPUT, DEFAULT_CONFIG, ("luals", "docs"))
            plan = plan_output(root, result.files, result.metadata)
            apply_plan(root, plan)
            original = (root / "lua/runtime.lua").stat().st_mtime_ns
            plan = plan_output(root, result.files, result.metadata)
            self.assertEqual((plan.writes, plan.removes), ({}, []))
            result = generate(DEFAULT_INPUT, DEFAULT_CONFIG, ("luals",))
            plan = plan_output(root, result.files, result.metadata)
            self.assertIn("docs/index.md", plan.removes)
            apply_plan(root, plan)
            self.assertEqual((root / "lua/runtime.lua").stat().st_mtime_ns, original)

    def test_determinism(self):
        with TemporaryDirectory() as temp:
            input_path = Path(temp) / "sdk.json"
            input_path.write_text(json.dumps(RECORDS))
            first = generate(input_path, DEFAULT_CONFIG, ("luals", "docs"), include_inspection=True)
            record = {**RECORDS[0], "properties": RECORDS[0]["properties"][::-1]}
            input_path.write_text(json.dumps([record]))
            second = generate(input_path, DEFAULT_CONFIG, ("luals", "docs"), include_inspection=True)
            self.assertEqual(first.files, second.files)
            self.assertEqual(first.metadata, second.metadata)

    def test_dependency_changes(self):
        with TemporaryDirectory() as temp:
            path = Path(temp) / "sdk.json"
            records = [{"kind": "Class", "path": "P.A"},
                       {"kind": "Class", "path": "P.B", "super": "P.A"}]
            path.write_text(json.dumps(records))
            first = generate(path, None, ("docs",), include_inspection=True)
            records[0]["size"] = 4
            path.write_text(json.dumps(records))
            second = generate(path, None, ("docs",), include_inspection=True)
            self.assertNotEqual(first.metadata["types"]["P.B"]["dependency_hash"],
                                second.metadata["types"]["P.B"]["dependency_hash"])
            self.assertIn("[`P.A`](type-P.A.md)", second.files["docs/type-P.B.md"])

    def test_cli_and_stale_outputs(self):
        runner = CliRunner()
        with TemporaryDirectory() as temp:
            output = Path(temp) / "out"
            args = ["--output", str(output), "--input", str(DEFAULT_INPUT)]
            result = runner.invoke(api, ["generate", "--target", "luals", "--dry-run", *args])
            self.assertEqual(result.exit_code, 0, result.output)
            self.assertFalse(output.exists())
            for command in (["generate", "--target", "luals"], ["validate"], ["coverage", "--json"]):
                result = runner.invoke(api, [*command, *args])
                self.assertEqual(result.exit_code, 0, result.output)
            (output / "lua/runtime.lua").write_text("stale")
            self.assertIn("Stale outputs", runner.invoke(api, ["validate", *args]).output)
            self.assertEqual(runner.invoke(api, ["clean", "--force", "--output", str(output)]).exit_code, 0)
            self.assertFalse((output / "lua/runtime.lua").exists())

    def test_hostile_manifest_cannot_escape_output(self):
        with TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            (root / "generation.json").write_text(json.dumps({
                "generator": "MirageAPIGen", "format": 1, "files": {"../kept.txt": "a" * 64},
            }))
            with self.assertRaisesRegex(ValueError, "Invalid generated file path"):
                plan_output(root, {}, {}, clean=True)

    def test_validation_json_for_invalid_metadata(self):
        runner = CliRunner()
        with TemporaryDirectory() as temp:
            path = Path(temp) / "sdk.json"
            path.write_text(json.dumps([{"kind": "Class", "path": "../bad"}]))
            result = runner.invoke(api, ["validate", "--input", str(path), "--output", str(Path(temp) / "out"), "--json"])
            self.assertEqual(result.exit_code, 1, result.output)
            self.assertIn("invalid-record", {d["code"] for d in json.loads(result.stdout)["diagnostics"]})

    def test_manifest_diff(self):
        runner = CliRunner()
        with TemporaryDirectory() as temp:
            old, new = Path(temp) / "old.json", Path(temp) / "new.json"
            snapshot = generate(DEFAULT_INPUT, DEFAULT_CONFIG, ("cpp",)).public
            old.write_text(json.dumps(snapshot))
            del snapshot["functions"]["mirage.sdk.get_vorbis_game_threshold"]
            new.write_text(json.dumps(snapshot))
            result = runner.invoke(api, ["diff", str(old), str(new), "--json", "--strict"])
            self.assertEqual(result.exit_code, 1, result.output)
            self.assertTrue(json.loads(result.stdout)["breaking"])

    def test_browser_metadata_and_navigation(self):
        with TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            result = generate(DEFAULT_INPUT, DEFAULT_CONFIG, ("luals", "docs"))
            apply_plan(root, plan_output(root, result.files, result.metadata))
            metadata = BrowserMetadata(root)
            path = "ALAudio.ALAudioDevice"
            self.assertTrue(metadata.artifact(path, "docs").is_file())
            self.assertTrue(metadata.artifact(path, "runtime_definitions").is_file())
            self.assertIn("mirage.sdk.get_vorbis_game_threshold",
                          [name for _, member in metadata.members[path] for name in member["lua_names"]])
            metadata.types[path]["docs"] = "docs/../secret.md"
            self.assertIsNone(metadata.artifact(path, "docs"))

    def test_output_lock(self):
        with TemporaryDirectory() as temp:
            root = Path(temp).resolve()
            with output_lock(root):
                with self.assertRaisesRegex(ValueError, "Another MirageAPIGen writer"), output_lock(root):
                    self.fail("The second writer acquired an active lock.")
            with output_lock(root):
                self.assertTrue(root.is_dir())
