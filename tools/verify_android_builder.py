#!/usr/bin/env python3
"""Reject release APKs without a complete, matching offline ROM builder."""
import argparse
import io
import json
from pathlib import Path
import struct
import sys
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "builder"))
from emerald3ds_builder.recipe import Recipe
from emerald3ds_builder.voxel import STEPS
from port_common.staging import BOOT_MARKER


def verify_scenery(archive, recipe):
    """Return only the exact APK members permitted to contain prebuilt scenery."""
    expected = {output.removeprefix("3ds_port/romfs/") for _, _, output in STEPS}
    if len(recipe.generated) != len(expected) or {item["path"] for item in recipe.generated} != expected:
        raise ValueError("APK recipe must describe exactly five scenery files")
    prefix = "assets/rombuilder/payload/scenery/"
    names = {prefix + rel for rel in expected}
    actual = [i.filename for i in archive.infolist() if i.filename.startswith(prefix) and not i.is_dir()]
    if len(actual) != len(names) or set(actual) != names:
        raise ValueError("APK has missing, duplicate or unexpected prebuilt scenery")
    for item in recipe.generated:
        data = archive.read(prefix + item["path"])
        if len(data) != item["size"] or zlib.crc32(data) & 0xFFFFFFFF != item["crc"]:
            raise ValueError("APK prebuilt scenery does not match its recipe: " + item["path"])
    return names


def verify(apk):
    with zipfile.ZipFile(apk) as archive:
        if archive.read("assets/romfs/boot.txt") != BOOT_MARKER:
            raise ValueError("APK RomFS boot marker is invalid (must use LF line endings)")
        abi = struct.unpack("<I", archive.read("assets/romfs/engine/abi.bin"))[0]
        manifest = json.loads(archive.read("assets/rombuilder/manifest.json"))
        recipe = Recipe.from_bytes(archive.read("assets/rombuilder/payload/android/emerald3ds.recipe"))
        if manifest.get("schema") != 2 or manifest.get("prebuilt_scenery") is not True:
            raise ValueError("APK is missing its prebuilt scenery declaration")
        verify_scenery(archive, recipe)
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
