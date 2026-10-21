#!/usr/bin/env python3
"""Reject release APKs without a complete, matching offline ROM builder."""
import argparse
import io
import json
from pathlib import Path
import struct
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder"))
from emerald3ds_builder.recipe import Recipe
from emerald3ds_builder.voxel import STEPS
from port_common.staging import BOOT_MARKER


def verify(apk):
    with zipfile.ZipFile(apk) as archive:
        if archive.read("assets/romfs/boot.txt") != BOOT_MARKER:
            raise ValueError("APK RomFS boot marker is invalid (must use LF line endings)")
        abi = struct.unpack("<I", archive.read("assets/romfs/engine/abi.bin"))[0]
        manifest = json.loads(archive.read("assets/rombuilder/manifest.json"))
        recipe = Recipe.from_bytes(archive.read("assets/rombuilder/payload/android/emerald3ds.recipe"))
        if not (abi == manifest["abi"] == recipe.engine_abi and manifest["rom_sha1"] == recipe.rom_sha1):
            raise ValueError("APK recipe and game engine do not match")
        if not recipe.inputs or not recipe.vtree or len(recipe.generated) != 5:
            raise ValueError("APK recipe is missing scenery reconstruction metadata")
        for script, _, output in STEPS:
            if output.removeprefix("3ds_port/romfs/") not in {item["path"] for item in recipe.generated}:
                raise ValueError("APK recipe is missing a generated file")
            archive.getinfo("assets/rombuilder/payload/voxelgen/scripts/" + script)
        archive.getinfo("lib/armeabi-v7a/libpython3.11.so")
        with zipfile.ZipFile(io.BytesIO(archive.read("assets/chaquopy/app.imy"))) as modules:
            for name in ("android_builder", "emerald3ds_builder/build", "emerald3ds_builder/recipe",
                         "emerald3ds_builder/voxel", "emerald3ds_builder/vtree", "emerald3ds_builder/pak"):
                if not any(name + suffix in modules.namelist() for suffix in (".py", ".pyc")):
                    raise ValueError("Missing Android builder module: " + name)
        with zipfile.ZipFile(io.BytesIO(archive.read("assets/chaquopy/requirements-common.imy"))) as requirements:
            requirements.getinfo("PIL/_imaging.so")
    print("PASS self-contained Android ROM builder, ABI %08x" % abi)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("apk", type=Path)
    verify(parser.parse_args().apk)
