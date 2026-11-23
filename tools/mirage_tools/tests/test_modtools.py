import unittest
import zipfile
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

import click
from click.testing import CliRunner
from rich.console import Console

from mirage_tools import modtools


@click.group()
def cli():
    pass


def unexpected_toolchain(_):
    raise AssertionError("This operation should not require a compiler")


modtools.register(cli, Console(), unexpected_toolchain, lambda: None)


class ModAuthoringTests(unittest.TestCase):
    def test_content_template_checks_builds_and_packages(self):
        runner = CliRunner()
        with TemporaryDirectory() as temp:
            root = Path(temp)
            mod = root / "step-tuning"
            created = runner.invoke(cli, ["new", mod.name, "--lang", "content", "--dir", temp,
                                          "--name", 'Faith’s "High Steps"'])
            self.assertEqual(created.exit_code, 0, created.output)
            info, problems = modtools.inspect(mod)
            self.assertEqual(problems, [])
            self.assertEqual(info.manifest["name"], 'Faith’s "High Steps"')
            self.assertFalse((mod / "src").exists())

            with patch.object(modtools, "OUT", root / "build"), patch.object(modtools, "DIST", root / "dist"):
                for command in ("check", "build", "package"):
                    result = runner.invoke(cli, ["mod", command, str(mod)])
                    self.assertEqual(result.exit_code, 0, result.output)
            with zipfile.ZipFile(root / "dist" / "step-tuning-0.1.0.zip") as archive:
                self.assertIn("step-tuning/mod.toml", archive.namelist())
                self.assertIn(b"MaxStepHeight = 105.0", archive.read("step-tuning/content/defaults.toml"))

    def test_existing_templates_still_check(self):
        with TemporaryDirectory() as temp:
            for kind in ("lua", "cs", "native"):
                with self.subTest(kind=kind):
                    mod = modtools.create(f"example-{kind}", kind, Path(temp), author="A Mod Author")
                    info, problems = modtools.inspect(mod)
                    self.assertEqual(problems, [])
                    self.assertEqual(info.kind, kind)

    def test_bad_manifest_values_report_each_field(self):
        runner = CliRunner()
        with TemporaryDirectory() as temp:
            Path(temp, "mod.toml").write_text(
                'id = "demo-mod"\nversion = "1.0.0"\nmirage = false\nsettings = []\n'
                'lua = "../main.lua"\nentry = "C:demo.dll"\ndotnet = "src/missing.cs"\n'
                'content = ["absent", "../shared"]\n',
                encoding="utf-8",
            )
            result = runner.invoke(cli, ["mod", "check", temp])
            self.assertEqual(result.exit_code, 1, result.output)
            for message in ("mirage must", "settings must", "lua must", "entry must",
                            "src/missing.cs is missing", "folder 'absent' is missing", "'../shared' must"):
                self.assertIn(message, result.output)

    def test_prebuilt_native_uses_declared_content_and_needs_no_compiler(self):
        with TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "assets").mkdir()
            (root / "payload.dll").write_bytes(b"example")
            (root / "mod.toml").write_text(
                'id = "demo_mod"\nversion = "1.0.0"\nmirage = "^0.1"\n'
                'entry = "payload.dll"\ncontent = ["assets"]\n',
                encoding="utf-8",
            )
            result = CliRunner().invoke(cli, ["mod", "build", temp])
            self.assertEqual(result.exit_code, 0, result.output)
            self.assertIn("native-binary", result.output)

    def test_setting_type_must_be_a_string(self):
        with TemporaryDirectory() as temp:
            Path(temp, "mod.toml").write_text(
                'id = "demo-mod"\nversion = "1.0.0"\nmirage = "^0.1"\n'
                '[settings.speed]\ntype = ["float"]\n',
                encoding="utf-8",
            )
            result = CliRunner().invoke(cli, ["mod", "check", temp])
            self.assertEqual(result.exit_code, 1, result.output)
            self.assertIn("settings.speed", result.output)
