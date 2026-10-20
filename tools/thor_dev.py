#!/usr/bin/env python3
"""Windows Thor loop: sync/build/deploy/run/shot without untracked helper scripts.

Bootstrap once with tools/bootstrap.py. `sync` then refreshes only the port
overlay, without fetching, resetting or patching the upstream checkout.
"""
from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "com.emerald3ds.thor"


def release_version(value: str) -> tuple[str, int]:
    """Use the tag as the display version and reserve 100 codes per patch."""
    match = re.fullmatch(r"v?(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z.-]+))?", value)
    if not match:
        raise argparse.ArgumentTypeError("Expected vMAJOR.MINOR.PATCH[-prerelease].")
    major, minor, patch = map(int, match.group(1, 2, 3))
    code = major * 10000000 + minor * 100000 + patch * 100 + 99
    if minor > 99 or patch > 999 or code > 2100000000:
        raise argparse.ArgumentTypeError("Android version needs minor <= 99, patch <= 999 and code <= 2100000000.")
    # Prerelease tags have a lower code than the corresponding final release.
    if match.group(4):
        code -= 99
    return value.removeprefix("v"), code


def run(cmd, **kwargs):
    print("+ " + " ".join(map(str, cmd)), flush=True)
    return subprocess.run(list(map(str, cmd)), check=True, **kwargs)


def sync(tree: Path):
    if not (tree / "src" / "main.c").is_file():
        raise SystemExit("Bootstrap the upstream tree first with tools/bootstrap.py.")
    for name in ("3ds_port", "android_port", "tools", "builder"):
        shutil.copytree(ROOT / name, tree / name, dirs_exist_ok=True,
                        ignore=shutil.ignore_patterns("build", "dist", "__pycache__", "*.pyc", ".gradle", ".cxx",
                                                     "app", "gradle", "local.properties", "payload", "local.mk"))
    print("Port overlay refreshed; upstream game sources were kept.")


def tool_environment(args) -> tuple[dict, Path, Path]:
    sdk = args.sdk.resolve()
    ndk = (args.ndk or sdk / "ndk" / "27.3.13750724").resolve()
    env = dict(os.environ)
    signing = Path.home() / ".android" / "emerald3ds-thor-signing.json"
    if args.release and signing.is_file():
        # Maintainer-local credentials, never copied to the build tree or printed.
        for key, value in json.loads(signing.read_text(encoding="utf-8")).items():
            if key in ("THOR_KEYSTORE", "THOR_KEY_ALIAS", "THOR_STORE_PASSWORD", "THOR_KEY_PASSWORD"):
                env.setdefault(key, value)
    msys = args.msys.resolve()
    env["PATH"] = os.pathsep.join(map(str, (Path(sys.executable).parent, msys / "ucrt64" / "bin",
                                          msys / "usr" / "bin", ndk / "prebuilt" / "windows-x86_64" / "bin"))) + os.pathsep + env.get("PATH", "")
    env["ANDROID_HOME"] = str(sdk)
    env.setdefault("JAVA_HOME", str(Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "Android/Android Studio/jbr"))
    env["MSYS_NO_PATHCONV"] = "1"
    return env, sdk, ndk


def build(args, env: dict, ndk: Path) -> Path:
    tree = args.tree.resolve()
    sync(tree)
    # apk-files replaces these generated directories. Check their absolute bounds before make runs.
    output = tree.parent / "android-out"
    work = tree / "3ds_port" / "build" / "romfs_release"
    build_root = (ROOT / "build").resolve()
    for target in (output, work):
        if not target.resolve().is_relative_to(build_root) or target.resolve() == build_root:
            raise SystemExit("Thor build outputs must stay beneath this workspace's build/ directory.")
    make = ndk / "prebuilt/windows-x86_64/bin/make.exe"
    make_args = [make, "-f", "../android_port/Makefile", "-j%d" % args.jobs,
                 "NDK=" + ndk.as_posix(), "MSYS_BIN=" + (args.msys.resolve() / "ucrt64/bin").as_posix(),
                 "PYTHON=" + Path(sys.executable).as_posix(), "VOXEL=1", "EMBED_DATA=0",
                 "MEASURE_AID=" + ("1" if args.measure else "0"), "apk-files", "pak"]
    if args.skip_pack:
        make_args.remove("pak")
    if args.version:
        make_args.append("APP_VERSION=" + args.version[0])
    if args.commit:
        make_args.append("PORT_COMMIT=" + args.commit)
    if args.release and args.measure:
        raise SystemExit("Measurement hooks cannot be enabled in a release.")
    if args.release:
        if (tree / "3ds_port/local.mk").exists():
            raise SystemExit("Remove local.mk from the staged tree before a production build.")
        required = ("THOR_KEYSTORE", "THOR_KEY_ALIAS", "THOR_STORE_PASSWORD", "THOR_KEY_PASSWORD")
        if any(not env.get(k) for k in required):
            raise SystemExit("Release signing needs " + ", ".join(required) + " in the environment.")
    run(make_args, cwd=tree / "3ds_port", env=env)
    assets = build_root / "android-assets"
    if assets.exists():
        if assets.resolve() != build_root / "android-assets":
            raise SystemExit("Refusing to replace an assets directory outside build/.")
        shutil.rmtree(assets)
    shutil.copytree(output / "romfs", assets / "romfs")
    gradle = ROOT / "android_port" / "gradlew.bat"
    gradle_args = [gradle, "assembleRelease" if args.release else "assembleDebug",
                   "-Pemerald3dsLib=" + (output / "lib").as_posix(), "-Pemerald3dsAssets=" + assets.as_posix()]
    if args.version:
        gradle_args += ["-PthorVersionName=" + args.version[0], "-PthorVersionCode=" + str(args.version[1])]
    run(gradle_args,
        cwd=ROOT / "android_port", env=env)
    kind = "release" if args.release else "debug"
    apk = ROOT / "android_port/app/build/outputs/apk" / kind / ("app-%s.apk" % kind)
    if args.release:
        signed = build_root / "Emerald3DS-Thor.apk"
        shutil.copy2(apk, signed)
        run([sys.executable, ROOT / "tools/release_audit.py", "--apk", signed, "--strict"], env=env)
    return apk


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("sync", "build", "deploy", "run", "shot"))
    parser.add_argument("--tree", type=Path, default=ROOT / "build/upstream")
    parser.add_argument("--sdk", type=Path, default=Path(os.environ.get("ANDROID_HOME", ROOT.anchor + "android-sdk")))
    parser.add_argument("--ndk", type=Path)
    parser.add_argument("--msys", type=Path, default=Path(os.environ.get("MSYS2_ROOT", "C:/msys64")))
    parser.add_argument("--serial", default=os.environ.get("ANDROID_SERIAL"))
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    parser.add_argument("--measure", action="store_true", help="enable measure.txt warp/battle hooks (development only)")
    parser.add_argument("--release", action="store_true", help="production signing from THOR_* environment variables")
    parser.add_argument("--version", type=release_version, help="release tag, also sets the Android version code")
    parser.add_argument("--commit", help="port commit recorded in the native build log")
    parser.add_argument("--skip-pack", action="store_true", help="build only the engine APK, without a local data pack")
    parser.add_argument("--continue-save", action="store_true", help="run: send Start/A to reach Continue")
    parser.add_argument("--display", help="shot: physical display id from adb shell dumpsys SurfaceFlinger --display-id")
    parser.add_argument("--out", type=Path, default=ROOT / "build/thor-shot.png")
    args = parser.parse_args()
    if args.action == "deploy" and args.skip_pack:
        parser.error("deploy requires its matching data pack")
    if args.commit and not re.fullmatch(r"[0-9a-fA-F]{7,40}", args.commit):
        parser.error("--commit must be a Git commit hash")
    if args.action == "sync":
        sync(args.tree.resolve())
        return
    env, sdk, ndk = tool_environment(args)
    adb = [sdk / "platform-tools/adb.exe"] + (["-s", args.serial] if args.serial else [])
    if args.action in ("build", "deploy"):
        apk = build(args, env, ndk)
        if args.action == "deploy":
            run(adb + ["install", "-r", apk], env=env)
            destination = "/sdcard/Android/data/%s/files/emerald3ds/" % PACKAGE
            run(adb + ["shell", "mkdir", "-p", destination], env=env)
            run(adb + ["push", args.tree.resolve() / "3ds_port/build/emerald3ds.pak", destination], env=env)
        print(apk)
    elif args.action == "run":
        run(adb + ["shell", "input", "keyevent", "KEYCODE_WAKEUP"], env=env)
        run(adb + ["shell", "am", "force-stop", PACKAGE], env=env)
        run(adb + ["shell", "am", "start", "-n", PACKAGE + "/.MainActivity"], env=env)
        if args.continue_save:
            time.sleep(6)
            for code, count, delay in (("KEYCODE_BUTTON_START", 3, 0.8), ("KEYCODE_BUTTON_A", 4, 1.0)):
                for _ in range(count):
                    run(adb + ["shell", "input", "keyevent", code], env=env)
                    time.sleep(delay)
    else:
        if not args.display:
            parser.error("shot needs --display; list physical ids with adb shell dumpsys SurfaceFlinger --display-id")
        args.out.parent.mkdir(parents=True, exist_ok=True)
        image = subprocess.check_output(list(map(str, adb + ["exec-out", "screencap", "-d", args.display, "-p"])), env=env)
        args.out.write_bytes(image)
        print(args.out.resolve())


if __name__ == "__main__":
    main()
