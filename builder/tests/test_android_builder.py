import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zlib
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "builder"))

from emerald3ds_builder import pak
from emerald3ds_builder.recipe import Recipe
from emerald3ds_builder.rom import Rom
from emerald3ds_builder import voxel
from emerald3ds_builder.build import build_pack, Payload

spec = importlib.util.spec_from_file_location("android_builder", ROOT / "android_port/app/src/main/python/android_builder.py")
android_builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(android_builder)


class Progress:
    def __init__(self): self.values = []
    def update(self, fraction, message): self.values.append((fraction, message))


class AndroidBuilderTests(unittest.TestCase):
    def test_terrain_solver_keeps_small_terms_on_python_311(self):
        # The release host's Python >=3.12 and Android's 3.11 used to sum
        # floats differently, changing which world seam the solver discarded.
        import ast
        import math
        source = ROOT / "3ds_port/scripts/gen_voxel_relief.py"
        tree = ast.parse(source.read_text())
        function = next(node for node in tree.body
                        if isinstance(node, ast.FunctionDef) and node.name == "_gauss_seidel")
        namespace = {"math": math}
        exec(compile(ast.Module(body=[function], type_ignores=[]), str(source), "exec"), namespace)
        pairs = {(0, i): (0, 1) for i in (1, 2, 3)}
        result = namespace["_gauss_seidel"](4, pairs, {1: 1e16, 2: 1.0, 3: -1e16}, iterations=1)
        self.assertEqual(result[0], 1 / 3)

    def recipe(self, folder):
        data = b"test game data"
        abi = pak.engine_abi([("data/test.bin", len(data), zlib.crc32(data))])
        recipe = Recipe(abi, "00" * 20, "test", entries=[{
            "path": "data/test.bin", "size": len(data), "crc": zlib.crc32(data), "ops": [["C", 0, len(data)]]}])
        path = folder / "payload/android/emerald3ds.recipe"
        path.parent.mkdir(parents=True)
        recipe.save(path)
        return recipe, data

    def test_real_core_writes_verified_pack_without_network_or_subprocesses(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            recipe, data = self.recipe(folder)
            progress = Progress()
            rom = Rom(data, recipe.rom_sha1, "test", "BPEE", folder / "test.gba")
            with patch("emerald3ds_builder.build.load_rom", return_value=rom), \
                    patch("emerald3ds_builder.followers.default_fetcher", side_effect=AssertionError("network")):
                result = json.loads(android_builder.build(str(rom.source), str(folder / "payload"),
                    str(folder / "result.pak"), recipe.engine_abi, progress))
            self.assertTrue(result["ok"])
            with pak.PakReader(folder / "result.pak") as reader:
                reader.verify()
                self.assertEqual(reader.read("data/test.bin"), data)
            self.assertEqual(progress.values[-1][0], 1.0)

    def test_wrong_engine_is_rejected_before_rom_is_read(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            recipe, _ = self.recipe(folder)
            with self.assertRaisesRegex(ValueError, "does not match"):
                android_builder.build("missing.gba", str(folder / "payload"), str(folder / "result.pak"),
                                      recipe.engine_abi ^ 1, Progress())
            self.assertFalse((folder / "result.pak").exists())

    def test_bad_rom_reports_plain_error_and_keeps_previous_output(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            recipe, _ = self.recipe(folder)
            rom = folder / "bad.gba"
            rom.write_bytes(b"invalid")
            output = folder / "result.pak"
            output.write_bytes(b"previous")
            result = json.loads(android_builder.build(str(rom), str(folder / "payload"), str(output),
                                                     recipe.engine_abi, Progress()))
            self.assertFalse(result["ok"])
            self.assertIn("ROM", result["message"])
            self.assertEqual(output.read_bytes(), b"previous")

    def test_generator_copy_needs_no_filesystem_metadata_permissions(self):
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            payload = folder / "payload"
            (payload / "scripts").mkdir(parents=True)
            (payload / "scripts/gen_voxel_regions.py").write_text(
                "from pathlib import Path\nPath('romfs/voxel/regions.bin').write_bytes(b'test')\n")
            expected = [{"path": "voxel/regions.bin", "size": 4, "crc": zlib.crc32(b"test")}]
            with patch("shutil.copystat", side_effect=PermissionError("Android metadata")):
                result = voxel.run_generators(folder / "tree", payload, expected, runner="inprocess")
            self.assertEqual(result, {"voxel/regions.bin": b"test"})

    def test_updates_reuse_only_matching_generated_data(self):
        for old_files, regenerate in (([("voxel/regions.bin", b"scenery")], False),
                                      ([("voxel/regions.bin", b"wrong")], True),
                                      ([("data/unrelated.bin", b"test")], True)):
            with self.subTest(regenerate=regenerate), tempfile.TemporaryDirectory() as temp:
                folder = Path(temp)
                recipe, data = self.recipe(folder)
                scene = b"scenery"
                recipe.generated = [{"path": "voxel/regions.bin", "size": len(scene), "crc": zlib.crc32(scene)}]
                recipe.engine_abi = pak.engine_abi([("data/test.bin", len(data), zlib.crc32(data)),
                                                   ("voxel/regions.bin", len(scene), zlib.crc32(scene))])
                recipe.save(folder / "payload/android/emerald3ds.recipe")
                cache = folder / "old.pak"
                pak.write_pak(cache, old_files, 999, bytes(20))
                rom = Rom(data, recipe.rom_sha1, "test", "BPEE", folder / "test.gba")
                progress = Progress()
                with patch("emerald3ds_builder.build.load_rom", return_value=rom), \
                     patch("emerald3ds_builder.build.build_tree"), \
                     patch("emerald3ds_builder.build.run_generators", return_value={"voxel/regions.bin": scene}) as generate:
                    build_pack(rom.source, Payload(folder / "payload", platform="thor"), folder / "result.pak",
                               generated_cache=cache, follower_fetcher=False)
                self.assertEqual(generate.called, regenerate)
                with pak.PakReader(folder / "result.pak") as reader:
                    self.assertEqual(reader.read("voxel/regions.bin"), scene)

    def test_fresh_android_setup_uses_bundled_scenery_after_rom_validation(self):
        for state in ("valid", "corrupt", "missing", "wrong_rom"):
            with self.subTest(state=state), tempfile.TemporaryDirectory() as temp:
                folder = Path(temp)
                recipe, data = self.recipe(folder)
                scene = b"prebuilt scenery"
                recipe.generated = [{"path": "voxel/regions.bin", "size": len(scene), "crc": zlib.crc32(scene)}]
                recipe.engine_abi = pak.engine_abi([("data/test.bin", len(data), zlib.crc32(data)),
                                                   ("voxel/regions.bin", len(scene), zlib.crc32(scene))])
                recipe.save(folder / "payload/android/emerald3ds.recipe")
                source = folder / "payload/scenery/voxel/regions.bin"
                source.parent.mkdir(parents=True)
                if state != "missing": source.write_bytes(b"bad" if state == "corrupt" else scene)
                rom = Rom(data, "11" * 20 if state == "wrong_rom" else recipe.rom_sha1,
                          "test", "BPEE", folder / "test.gba")
                output = folder / "result.pak"
                output.write_bytes(b"previous pack")
                progress = Progress()
                with patch("emerald3ds_builder.build.load_rom", return_value=rom), \
                     patch("emerald3ds_builder.build.build_tree", side_effect=AssertionError("tree generation")), \
                     patch("emerald3ds_builder.build.run_generators", side_effect=AssertionError("scenery generation")):
                    result = json.loads(android_builder.build(str(rom.source), str(folder / "payload"),
                        str(output), recipe.engine_abi, progress))
                self.assertEqual(result["ok"], state == "valid")
                if state == "valid":
                    with pak.PakReader(output) as reader:
                        reader.verify()
                        self.assertEqual(reader.read("voxel/regions.bin"), scene)
                    self.assertIn("Installing prebuilt scenery", [message for _, message in progress.values])
                else:
                    self.assertEqual(output.read_bytes(), b"previous pack")
                    self.assertIn("no game data" if state == "wrong_rom" else "prebuilt scenery", result["message"])
