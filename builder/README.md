# Emerald 3DS Builder

Turns the player's own Pokémon Emerald (USA, Europe), Pokémon Esmeralda
(Spain) or Pokémon Émeraude (France) ROM into `emerald3ds.pak` and installs
the game on an SD card, in that ROM's language.

Release users get a standalone Windows executable (no Python needed). From
source (any OS, Python 3.11+ and Pillow):

```
python -m emerald3ds_builder                          # window
python -m emerald3ds_builder --payload DIR build --rom ROM --output OUT
python -m emerald3ds_builder --payload DIR install --rom ROM --sd /media/me/3DS
python -m emerald3ds_builder --payload DIR verify --pak FILE
python -m emerald3ds_builder detect
```

`DIR` is a release's `payload/` folder (executable, `emerald3ds.recipe`,
`voxelgen/`); by default the builder looks next to itself.

For Thor, choose **thor** in the window, or add `--target thor` to
`build`, `install` or `verify`. The release payload must contain
`android/Emerald3DS-Thor.apk` and `android/emerald3ds.recipe`; localized
variants live under `android/es`, `android/fr` or `android/it`. Generators
are shared in `voxelgen/`, but Thor generates row-ordered texture tiles.
The output folder is `emerald3ds/`. Install the APK and import the pack
through its Android Back menu; see [installation](../docs/THOR_INSTALLATION.md).

Web callers pass `target="thor"` to `build_pack_for_web` or `run_web_build`.
Manifest variants with `platform: "thor"` carry `apk` and `recipe` assets;
an omitted platform continues to mean `3ds`. A ROM may occur once per target.

| Module | Role |
|---|---|
| `rom.py` | recognises the supported ROM |
| `recipe.py` | recipe format; rebuilds one file from the ROM and checks it |
| `vtree.py` | rebuilds the voxel generators' inputs from the ROM |
| `voxel.py` | runs the bundled generators (in child processes, or in-process for the web) |
| `pak.py` | writes and reads the data pack |
| `build.py` | ROM → data pack |
| `install.py` | SD card detection and installation |
| `cli.py`, `gui.py` | the two desktop front ends |
| `web.py` | entry point of the web builder (Pyodide in the browser) |
| `webmanifest.py` | `web-manifest.json`, the contract between a release and the website |

Privacy: the ROM is read into memory and never copied, uploaded or modified;
temporary files are removed even when a step fails.

## Web builder

The website runs this same package in the browser (Pyodide, in a Web Worker)
from the release's `Emerald3DS-WebPayload.zip`, through
`web.run_web_build(rom, payload, out_pak, progress, manifest)`. The only
difference with the desktop build is that the voxel generators run in-process
(`voxel.run_step_inprocess`): each script gets fresh sibling modules, argv,
working directory and import path, exactly as a new process would. Outputs are
checked against the same CRCs and ABI. `tools/build_web_payload.py --synthetic
DIR` writes a synthetic payload and "ROM" to exercise it without game data.

Tests: `python -m unittest discover -s builder/tests` (synthetic data only).
