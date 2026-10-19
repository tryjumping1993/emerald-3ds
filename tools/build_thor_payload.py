#!/usr/bin/env python3
"""Make payload/android[/<language>] from a signed engine APK and its matching staged tree.

Build with `tools/thor_dev.py build --release` first. ROM and GBA ELF stay
local. Then pass this payload to tools/build_release.py --thor-payload.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def run(cmd):
    subprocess.run(list(map(str, cmd)), check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tree", type=Path, default=ROOT / "build/upstream")
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--gba-elf", type=Path, required=True, help="matching original GBA ELF for voxel metadata")
    parser.add_argument("--version", required=True)
    parser.add_argument("--language", choices=("en", "es", "fr", "it"), default="en")
    parser.add_argument("--out", type=Path, default=ROOT / "dist/thor-payload")
    parser.add_argument("--nm", default="arm-none-eabi-nm")
    parser.add_argument("--apksigner", type=Path, required=True)
    args = parser.parse_args()
    port = args.tree.resolve() / "3ds_port"
    info = (port / "build/buildinfo.c").read_text(encoding="utf-8")
    if "-DCTR_MEASURE_AID" in info or (port / "romfs/data.embedded").exists() or (port / "local.mk").exists():
        parser.error("release trees must have no measure hooks, embedded data or local.mk")
    run([args.apksigner, "verify", args.apk])
    with zipfile.ZipFile(args.apk) as archive:
        for name in ("engine/abi.bin", "engine/build_id.bin"):
            if archive.read("assets/romfs/" + name) != (port / "romfs" / name).read_bytes():
                parser.error("the APK and the staged engine tree do not match")
    output = args.out / (args.language if args.language != "en" else "")
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="thor-release-") as temp:
        work = Path(temp)
        apk = work / "Emerald3DS-Thor.apk"
        shutil.copy2(args.apk, apk)
        run([sys.executable, ROOT / "tools/release_audit.py", "--apk", apk, "--strict", "--rom", args.rom])
        recipe = work / "emerald3ds.recipe"
        recipe_cmd = [sys.executable, ROOT / "tools/gen_recipe.py", "--romfs", port / "romfs", "--rom", args.rom,
             "--out", recipe, "--release", "v" + args.version, "--elf", port / "emerald3ds.elf",
             "--image-elf", port / "build/gamedata_image.elf",
             "--image-map", port / "build/gamedata_image.map", "--nm", args.nm,
             "--decomp", args.tree.resolve(), "--report", output / "recipe-literal-report.txt"]
        if args.gba_elf:
            recipe_cmd += ["--gba-elf", args.gba_elf]
        run(recipe_cmd)
        # Only distributable files enter the payload; never copy the staging directory or the local ROM.
        shutil.copy2(apk, output / apk.name)
        shutil.copy2(recipe, output / recipe.name)
    print(output.resolve())


if __name__ == "__main__":
    main()
