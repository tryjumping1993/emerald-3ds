import argparse
import io
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "builder")]
import release_audit
import stage_android_builder
import thor_dev
from emerald3ds_builder.recipe import Recipe


class AndroidReleaseTests(unittest.TestCase):
    def test_mismatched_recipe_cannot_be_staged(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            assets = folder / "assets"
            (assets / "romfs/engine").mkdir(parents=True)
            (assets / "romfs/engine/abi.bin").write_bytes(struct.pack("<I", 123))
            recipe = folder / "test.recipe"
            Recipe(456, "00" * 20, "test").save(recipe)
            with self.assertRaisesRegex(ValueError, "different ABIs"):
                stage_android_builder.stage(recipe, assets, ROOT / "3ds_port")
            self.assertFalse((assets / "rombuilder").exists())

    def test_release_requires_local_reconstruction_inputs(self):
        with tempfile.TemporaryDirectory() as temp, patch("pathlib.Path.home", return_value=Path(temp)):
            args = argparse.Namespace(release=True, rom=None, gba_elf=None, build_python=None)
            with self.assertRaisesRegex(SystemExit, "on-device ROM import"):
                thor_dev.builder_inputs(args)

    def test_builder_configuration_only_supplies_local_paths(self):
        with tempfile.TemporaryDirectory() as temp, patch("pathlib.Path.home", return_value=Path(temp)):
            config = Path(temp) / ".android/emerald3ds-thor-builder.json"
            config.parent.mkdir()
            config.write_text(json.dumps({"rom": "own.gba", "gba_elf": "own.elf", "build_python": "python.exe"}))
            args = argparse.Namespace(release=True, rom=Path("explicit.gba"), gba_elf=None, build_python=None)
            thor_dev.builder_inputs(args)
            self.assertEqual(args.rom, Path("explicit.gba"))
            self.assertEqual(args.gba_elf, Path("own.elf"))

    def archive(self, entries):
        output = io.BytesIO()
        with zipfile.ZipFile(output, "w") as archive:
            for name, data in entries.items(): archive.writestr(name, data)
        return output.getvalue()

    def audit_python(self, contents):
        name = "Emerald3DS-Thor.apk!/assets/chaquopy/stdlib-common.imy"
        data = self.archive(contents)
        return release_audit.audit([(name, lambda: data)],
            release_audit.Allow.load(ROOT / "tools/release_audit_allow.toml"), [], None, "apk")

    def test_python_archives_are_inspected_even_for_vendor_runtime(self):
        self.assertFalse(self.audit_python({"module.pyc": b"compiled Python module"}))
        rules = {item.rule for item in self.audit_python({"hidden.pyc": b"EM3DPAK\0secret pack"})}
        self.assertIn("data-pack", rules)
        rules = {item.rule for item in self.audit_python({"../escape.pyc": b"compiled"})}
        self.assertIn("python-archive-invalid-path", rules)

    def test_apk_rom_scan_checks_decoded_members(self):
        rom = bytes(range(255, -1, -1)) * 4
        archive = self.archive({"AndroidManifest.xml": b"manifest",
            "lib/armeabi-v7a/libemerald3ds.so": b"engine",
            "assets/hidden.dat": rom})
        findings = release_audit.audit([("test.apk", lambda: archive)],
            release_audit.Allow(), [], release_audit.RomIndex(rom), "zip")
        matches = [item.path for item in findings if item.rule == "rom-content"]
        self.assertEqual(matches, ["test.apk!/assets/hidden.dat"])
