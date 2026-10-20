"""Offline Android entry point. The caller stages files and installs atomically."""
from pathlib import Path
import json
import tempfile

from emerald3ds_builder.build import Payload, build_pack
from emerald3ds_builder.recipe import Recipe
from emerald3ds_builder.errors import BuilderError


def build(rom_path, payload_path, output_path, expected_abi, callback, cache_pack=None):
    payload = Payload(Path(payload_path), platform="thor")
    recipe = Recipe.load(payload.recipe)
    if recipe.engine_abi != expected_abi:
        raise ValueError("The app's ROM builder does not match its game engine. Install the complete APK again.")

    def progress(fraction, message):
        callback.update(float(fraction), str(message))

    # Keep all intermediate ROM-derived data under the caller's disposable
    # directory, including after a cancelled process is cleaned on next launch.
    previous_tempdir = tempfile.tempdir
    tempfile.tempdir = str(Path(output_path).parent)
    try:
        result = build_pack(Path(rom_path), payload, Path(output_path), progress=progress,
                            runner="inprocess", follower_fetcher=False,
                            generated_cache=Path(cache_pack) if cache_pack else None)
        return json.dumps({"ok": True, "release": result["release"]})
    except BuilderError as exc:
        return json.dumps({"ok": False, "message": exc.message, "hint": exc.hint})
    finally:
        tempfile.tempdir = previous_tempdir
