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
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "tools"), str(ROOT / "builder")]
import release_audit
import stage_android_builder
import thor_dev
import verify_android_builder
from port_common import staging
from emerald3ds_builder.recipe import Recipe
from emerald3ds_builder.voxel import STEPS


class AndroidReleaseTests(unittest.TestCase):
    def test_windows_engine_marker_is_canonical_and_native_compatible(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            source = folder / "source"
            source.mkdir()
            (source / "boot.txt").write_bytes(b"Emerald3DS RomFS v1\r\n")
            output = folder / "engine"
            staging.cmd_engine(argparse.Namespace(romfs=source, out=output))
            self.assertEqual((output / "boot.txt").read_bytes(), b"Emerald3DS RomFS v1\n")
            # This is the exact native CtrFs_Init fgets/strcmp contract.
            with (output / "boot.txt").open("rb") as marker:
                self.assertEqual(marker.readline(), staging.BOOT_MARKER)

    def test_apk_verification_rejects_crlf_boot_marker(self):
        with tempfile.TemporaryDirectory() as temp:
            apk = Path(temp) / "bad.apk"
            apk.write_bytes(self.archive({"assets/romfs/boot.txt": b"Emerald3DS RomFS v1\r\n"}))
            with self.assertRaisesRegex(ValueError, "boot marker"):
                verify_android_builder.verify(apk)

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

    def scenery_recipe(self):
        data = {output.removeprefix("3ds_port/romfs/"): b"scenery:" + output.encode() for _, _, output in STEPS}
        recipe = Recipe(123, "00" * 20, "test", inputs=[{"test": True}], vtree=[{"test": True}],
                        generated=[{"path": name, "size": len(value), "crc": zlib.crc32(value)}
                                   for name, value in data.items()])
        return recipe, data

    def test_staging_requires_all_five_matching_outputs(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            assets = folder / "assets"
            (assets / "romfs/engine").mkdir(parents=True)
            (assets / "romfs/engine/abi.bin").write_bytes(struct.pack("<I", 123))
            recipe, data = self.scenery_recipe()
            path = folder / "test.recipe"
            recipe.save(path)
            port = folder / "port"
            for name, value in data.items():
                source = port / "romfs" / name
                source.parent.mkdir(parents=True, exist_ok=True)
                source.write_bytes(value)
            with patch.object(stage_android_builder, "local_closure", return_value=([], [])), \
                 patch.object(stage_android_builder, "VOXELGEN_FILES", []):
                stage_android_builder.stage(path, assets, port)
                for name, value in data.items():
                    self.assertEqual((assets / "rombuilder/payload/scenery" / name).read_bytes(), value)
                source.write_bytes(b"corrupt")
                with self.assertRaisesRegex(ValueError, "Prebuilt scenery"):
                    stage_android_builder.stage(path, assets, port)

    def test_apk_scenery_validation_rejects_missing_corrupt_and_extra_files(self):
        recipe, data = self.scenery_recipe()
        prefix = "assets/rombuilder/payload/scenery/"
        valid = {prefix + name: value for name, value in data.items()}
        with zipfile.ZipFile(io.BytesIO(self.archive(valid))) as archive:
            self.assertEqual(verify_android_builder.verify_scenery(archive, recipe), set(valid))
        name = next(iter(valid))
        for entries in ({key: val for key, val in valid.items() if key != name},
                        {**valid, name: b"corrupt"}, {**valid, prefix + "extra.bin": b"extra"}):
            with zipfile.ZipFile(io.BytesIO(self.archive(entries))) as archive:
                with self.assertRaises(ValueError):
                    verify_android_builder.verify_scenery(archive, recipe)

    def test_rom_scan_exempts_only_verified_bundled_scenery(self):
        recipe, data = self.scenery_recipe()
        rom = bytes((i * i + i * 13) % 251 for i in range(1024))
        name = "voxel/buildings.bin"
        data[name] = rom
        item = next(item for item in recipe.generated if item["path"] == name)
        item.update(size=len(rom), crc=zlib.crc32(rom))
        entries = {"AndroidManifest.xml": b"manifest", "lib/armeabi-v7a/libemerald3ds.so": b"engine",
                   "assets/rombuilder/payload/android/emerald3ds.recipe": recipe.to_bytes(),
                   **{"assets/rombuilder/payload/scenery/" + key: value for key, value in data.items()}}
        for extra, expected in (({}, False), ({"assets/hidden.dat": rom}, True)):
            findings = release_audit.audit([("test.apk", lambda: self.archive({**entries, **extra}))],
                release_audit.Allow(), [], release_audit.RomIndex(rom), "zip")
            self.assertEqual(any(item.rule == "rom-content" for item in findings), expected)
            self.assertFalse(any(item.rule == "apk-scenery-invalid" for item in findings))

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
