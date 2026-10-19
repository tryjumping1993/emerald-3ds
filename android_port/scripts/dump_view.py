"""Turns the raw RGBA files the GL layer writes on request (c3d_gles.c, DumpAll) into PNGs.

    adb shell touch /sdcard/Android/data/com.emerald3ds.thor/files/emerald3ds/dump.txt
    adb pull /sdcard/Android/data/com.emerald3ds.thor/files/emerald3ds/ dumps
    python android_port/scripts/dump_view.py dumps
"""
import re
import sys
from pathlib import Path

from PIL import Image

for path in sorted(Path(sys.argv[1]).rglob("*.rgba")):
    match = re.search(r"_(\d+)x(\d+)\.rgba$", path.name)
    if not match:
        continue
    width, height = int(match.group(1)), int(match.group(2))
    data = path.read_bytes()
    if len(data) != width * height * 4:
        continue
    picture = Image.frombytes("RGBA", (width, height), data)
    background = Image.new("RGBA", picture.size, (255, 0, 255, 255))
    background.alpha_composite(picture)
    background.convert("RGB").save(path.with_suffix(".png"))
    print(path.with_suffix(".png"))
