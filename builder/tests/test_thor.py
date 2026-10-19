"""Thor variants cannot accidentally select the 3DS recipe or Morton texture order."""
import copy
import io
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "builder"), str(ROOT / "tools")]
import build_web_payload
import release_audit
from emerald3ds_builder import web, webmanifest, voxel
from emerald3ds_builder.build import Payload
from emerald3ds_builder.install import install
from emerald3ds_builder.recipe import Recipe


class ThorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        with redirect_stdout(io.StringIO()):
            self.synthetic = build_web_payload.make_synthetic(self.root / "synthetic")
        self.payload = self.synthetic["payload"]
        for lang in ("", "es"):
            dest = self.payload / "android" / lang
            dest.mkdir(parents=True)
            shutil.copy2(self.payload / lang / "emerald3ds.recipe", dest / "emerald3ds.recipe")
            with zipfile.ZipFile(dest / "Emerald3DS-Thor.apk", "w") as z:
                z.writestr("AndroidManifest.xml", b"synthetic manifest")
                z.writestr("lib/armeabi-v7a/libemerald3ds.so", b"synthetic engine")

    def manifest(self):
        return build_web_payload.make_manifest(build_web_payload.collect(self.payload), "0.0.0", "v0.0.0", synthetic=True)

    def test_target_and_language_selection(self):
        sha = Recipe.load(self.payload / "es/emerald3ds.recipe").rom_sha1
        console = Payload(self.payload).for_rom(sha)
        android = Payload(self.payload, platform="thor").for_rom(sha)
        self.assertEqual(console.root, self.payload / "es")
        self.assertEqual(android.root, self.payload / "android/es")
        self.assertEqual(android.voxelgen, self.payload / "voxelgen")
        self.assertEqual([p.name for p in android.executables()], ["Emerald3DS-Thor.apk"])
        android.check()

    def test_web_build_selects_target_and_leaves_environment(self):
        manifest = self.manifest()
        path = self.root / "web-manifest.json"
        path.write_text(json.dumps(manifest))
        with patch.dict(os.environ, {"CTR_TEXEL_ORDER": "inherited"}):
            output = web.build_pack_for_web(self.synthetic["rom_es"], self.payload, self.root / "out.pak", manifest_path=path, target="thor")
            self.assertEqual(os.environ["CTR_TEXEL_ORDER"], "inherited")
        self.assertEqual(output["platform"], "thor")
        self.assertEqual(len(webmanifest.rom_variants(manifest, "thor")), 2)
        duplicate = copy.deepcopy(manifest)
        duplicate["variants"].append(duplicate["variants"][-1])
        with self.assertRaises(webmanifest.ManifestError):
            webmanifest.validate(duplicate)

    def test_install_layout(self):
        src = self.root / "data"
        src.write_bytes(b"synthetic pack")
        dest = install(self.root, {"emerald3ds.pak": src}, target="thor")
        self.assertEqual(dest, self.root / "emerald3ds")
        self.assertFalse((self.root / "3ds").exists())

    def test_generators_use_requested_order_in_both_runners(self):
        script = self.root / "generator.py"
        output = self.root / "order.txt"
        script.write_text("import os,sys; open(sys.argv[1], 'w').write(os.environ['CTR_TEXEL_ORDER'])\n")
        for runner in (voxel.run_step_subprocess, voxel.run_step_inprocess):
            for order in ("rows", "morton"):
                runner(script, [str(output)], self.root, texel_order=order)
                self.assertEqual(output.read_text(), order)

    def test_apk_audit_opens_contents_and_rejects_embedded_data(self):
        apk = self.payload / "android/Emerald3DS-Thor.apk"
        allow = release_audit.Allow.load(ROOT / "tools/release_audit_allow.toml")
        entries = [(apk.name, apk.read_bytes)]
        self.assertEqual(release_audit.audit(entries, allow, [], None, "zip"), [])
        with zipfile.ZipFile(apk, "a") as z:
            z.writestr("assets/romfs/graphics/from-rom.bin", b"secret game data")
            z.writestr("assets/romfs/emerald3ds.pak", b"EM3DPAK\x00")
        findings = release_audit.audit(entries, allow, [], None, "zip")
        self.assertIn("generated-romfs", [f.rule for f in findings])
        self.assertIn("data-pack", [f.rule for f in findings])

    def test_apk_classes_are_scanned_for_rom_content(self):
        import random
        data = random.Random(42).randbytes(8192)
        apk = self.payload / "android/Emerald3DS-Thor.apk"
        with zipfile.ZipFile(apk, "a") as z:
            z.writestr("classes.dex", data)
        allow = release_audit.Allow.load(ROOT / "tools/release_audit_allow.toml")
        findings = release_audit.audit([(apk.name, apk.read_bytes)], allow, [],
                                       release_audit.RomIndex(data), "zip")
        self.assertTrue(any(f.rule == "rom-content" and f.path.endswith("!/classes.dex") for f in findings))


if __name__ == "__main__":
    unittest.main()
