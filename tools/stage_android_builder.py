#!/usr/bin/env python3
"""Stage the ROM builder, matching recipe and prebuilt Android scenery.

The five scenery outputs are bundled; the ROM, GBA ELF and complete game pack
remain on the build machine. Setup still requires the player's matching ROM.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder"))
from emerald3ds_builder.recipe import Recipe
from emerald3ds_builder.voxel import STEPS
from build_release import GENERATORS, VOXELGEN_FILES, local_closure


def stage(recipe_path: Path, assets: Path, port: Path):
    recipe = Recipe.load(recipe_path)
    abi = struct.unpack("<I", (assets / "romfs/engine/abi.bin").read_bytes())[0]
    if recipe.engine_abi != abi:
        raise ValueError("ROM builder recipe and APK engine have different ABIs")
    if len(recipe.generated) != len(GENERATORS) or not recipe.inputs or not recipe.vtree:
        raise ValueError("The on-device builder requires complete scenery reconstruction metadata")
    expected = {output.removeprefix("3ds_port/romfs/") for _, _, output in STEPS}
    if {item["path"] for item in recipe.generated} != expected:
        raise ValueError("The APK must contain exactly the five matching scenery outputs")
    scenery = {}
    for item in recipe.generated:
        data = (port / "romfs" / item["path"]).read_bytes()
        if len(data) != item["size"] or zlib.crc32(data) & 0xFFFFFFFF != item["crc"]:
            raise ValueError("Prebuilt scenery does not match the recipe: " + item["path"])
        scenery[item["path"]] = data
    target = assets / "rombuilder"
    payload = target / "payload"
    (payload / "android").mkdir(parents=True, exist_ok=True)
    shutil.copy2(recipe_path, payload / "android/emerald3ds.recipe")
    for rel, data in scenery.items():
        dest = payload / "scenery" / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
    scripts, _ = local_closure(port / "scripts", GENERATORS)
    for rel in [*("scripts/" + name for name in scripts), *VOXELGEN_FILES]:
        dest = payload / "voxelgen" / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(port / rel, dest)
    (target / "manifest.json").write_text(json.dumps({"schema": 2, "abi": abi, "prebuilt_scenery": True,
        "rom_sha1": recipe.rom_sha1, "release": recipe.release}), encoding="utf-8")
    print("ROM builder and five verified prebuilt scenery files staged, ABI %08x" % abi)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recipe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--port", type=Path, default=ROOT / "3ds_port")
    args = parser.parse_args()
    stage(args.recipe, args.assets, args.port)


if __name__ == "__main__":
    main()
