#!/usr/bin/env python3
"""Stage original builder scripts and a verified recipe inside the Android APK.

Only reconstruction instructions are shipped. The ROM, GBA ELF and generated
game pack remain on the build machine. The same core runs on desktop and Android.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder"))
from emerald3ds_builder.recipe import Recipe
from build_release import GENERATORS, VOXELGEN_FILES, local_closure


def stage(recipe_path: Path, assets: Path, port: Path):
    recipe = Recipe.load(recipe_path)
    abi = struct.unpack("<I", (assets / "romfs/engine/abi.bin").read_bytes())[0]
    if recipe.engine_abi != abi:
        raise ValueError("ROM builder recipe and APK engine have different ABIs")
    if len(recipe.generated) != len(GENERATORS) or not recipe.inputs or not recipe.vtree:
        raise ValueError("The on-device builder requires complete scenery reconstruction metadata")
    target = assets / "rombuilder"
    payload = target / "payload"
    (payload / "android").mkdir(parents=True, exist_ok=True)
    shutil.copy2(recipe_path, payload / "android/emerald3ds.recipe")
    scripts, _ = local_closure(port / "scripts", GENERATORS)
    for rel in [*("scripts/" + name for name in scripts), *VOXELGEN_FILES]:
        dest = payload / "voxelgen" / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(port / rel, dest)
    (target / "manifest.json").write_text(json.dumps({"schema": 1, "abi": abi,
        "rom_sha1": recipe.rom_sha1, "release": recipe.release}), encoding="utf-8")
    print("On-device ROM builder staged, ABI %08x" % abi)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recipe", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--port", type=Path, default=ROOT / "3ds_port")
    args = parser.parse_args()
    stage(args.recipe, args.assets, args.port)


if __name__ == "__main__":
    main()
