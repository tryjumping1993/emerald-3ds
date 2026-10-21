# Thor verification

## Prebuilt scenery setup — 2026-10-21

An API 36 x86-64 emulator completed fresh setup using the APK's five bundled
scenery files in a 13.86-second instrumentation test, including Python startup.
The previous full-generation test took 717 seconds on this emulator profile.
This is an emulator measurement, not a Thor timing; the Thor was not visible
over ADB during this check. The release engine remains ARMv7.

The test replaced both scenery generation and input reconstruction with
functions that fail if called, provided no usable existing pack, and still
assembled and installed a fully verified matching pack. Save/settings markers
survived and the import cache was removed. Host tests also reject wrong ROMs
and missing/corrupt bundled scenery without replacing the previous output.

All 96 builder tests passed (five environment-dependent skips locally), all
12 release-tool tests passed, and Android debug unit tests passed. Packaging
checks require exactly the five matching outputs. Release auditing permits
those game-derived files only inside the APK after recipe verification;
other members retain ROM, pack and secret checks.

## Native startup packaging fix — 2026-10-20

On the connected AYN Thor, the published 0.4.2 APK failed before loading game
data: `CtrFs_Init` rejected `boot.txt` because the Windows checkout had
converted its newline to CRLF. The native reader uses binary mode and expects
`Emerald3DS RomFS v1\n` exactly. The generated pack was correct and matched
the independently rebuilt release pack byte-for-byte.

A signed packaging-only test update, preserving the released native library
and reconstruction recipe, changed the marker to LF. The Thor then logged
`RomFS boot marker PASS`, loaded the matching pack, initialized graphics/audio
and the file save driver, and rendered the game intro. A second cold start
also reached the intro. The pack SHA-256 remained unchanged. Temporary debug
logging was removed after collecting the evidence.

Engine staging now canonicalizes and validates the marker, Git pins its LF
checkout, and APK validation rejects CRLF markers. Regression tests reproduce
the Windows input and check the exact bytes read by native startup.

## On-device ROM builder — 2026-10-20

No Thor was connected. Android runtime tests use an API 36 x86-64 emulator
with the APK's Python 3.11/Pillow 9.2 builder and a locally staged clean ROM;
the ROM and generated pack are never included in an APK or test artifact.
The release engine remains ARMv7.

- Fresh setup rebuilt all five scenery files and the complete matching pack
  from the ROM on Android, verified the result, and installed it successfully.
  The 2 GiB emulator took 717 seconds. Save and settings test files survived;
  the previous dummy pack was replaced only after validation, and temporary
  build files were removed.
- The actual Android upgrade path rebuilt and atomically installed a compatible
  pack using verified existing scenery in 3.7 seconds. Save and settings test
  files were unchanged, and setup's temporary files were removed.
- Builder tests cover ROM rejection, incompatible recipes, verified scenery
  reuse and fallback, Android file-copy permissions, and consistent terrain
  summation on Python 3.11 and 3.12.
- A full terrain rebuild under Python 3.11/Pillow 9.2 matches the release's
  terrain byte-for-byte after the summation correction.
- The signed ARMv7 APK passes structural checks for the matching recipe, full
  generator closure, shared builder modules, Python runtime and Pillow.
  Recursive publication auditing with the local clean-ROM scan passes.
- Android save-storage, render-interpolation and terrace-foot regressions pass.

## Save and interpolation test build — 2026-10-20

No Thor was connected for this build. Hardware save/restart and visual
smoothness checks remain pending.

- The actual Android path adapter, file save writer and selected flash driver
  pass host checks for all 32 sectors, separate signature-byte commits,
  reloads, mismatch detection, bounds rejection and RTC trailer preservation.
- The actual voxel render functions pass host checks for previous/midpoint
  positions, stopping without prediction, unchanged simulation/camera buffers,
  separate speed and interpolation controls, scene cuts and sprite replacement.
- Existing GLES draw and shim lock regressions pass. Builder tests: 89 run,
  2 skipped, no failures. Release version tests: 2 passed.

The test APK requires its matching regenerated pack. Updating with the same
signing key and importing that pack keeps the existing save and settings.

## Previous verification — 2026-10-19

This records the local verification of the Thor port and its English release
payload. Build and installation commands are in [THOR_BUILD.md](THOR_BUILD.md)
and [THOR_INSTALLATION.md](THOR_INSTALLATION.md).

| Check | Result |
|---|---|
| Android native build, Kotlin, lint and release packaging | Passed, ARMv7 / NDK 27.3.13750724 |
| Production APK signature | APK Signature Scheme v2 verified |
| Builder tests | 89 tests, 2 skipped, no failures |
| Android pack-import JUnit tests | 2 passed |
| GLES host regression | Indexed voxel shader/layout, index formats, stream/VRAM offsets and return to 2D passed |
| Actual shim lock host regression | Static initializer, dynamic initialization and threaded contention passed |
| Host voxel/input/video/settings/clock checks | Passed |
| 3DS engine build and full `make verify` | Passed in the official devkitARM container |
| Windows standalone builder | Built; frozen CLI verifies the final pack and regenerates the intro image with matching SHA-256 |
| Windows and web release ZIPs, standalone APK | Strict recursive audits including the clean-ROM scan passed |

The Windows builder regenerated all five scenery outputs from the clean
English ROM. That pack matched its staged engine byte-for-byte. The final
native lock fix changed executable pointers, so a new recipe was generated
and checked against every final data entry. Scenery inputs, metadata and
output CRCs remained identical; reusing those already regenerated outputs
reconstructed the final staged pack exactly:

```text
Thor ABI:       07946566
Pack SHA-256:   67adc57f327510370c07674f0f7847b08813079c5b149bedf44c2915fec6af5b
APK SHA-256:    53c1a0e4b3301ec1ee78327b0b7a73ea52631b348a983d780bcdd294ef7ca4e0
Certificate:    64da29e6bb67af51c359402849a19aaed87ce06d50e1bdfef5f0527cc29f6141
```

The Thor recipe reconstructs 6,591 ROM-derived entries and five generated
entries, with 2,202 voxel inputs. It passes the existing literal budgets:
20,086 unexplained bytes overall, including 19,872 bytes classified as
game-data padding and **zero** unexplained bytes of game objects. Initialized
game tables and PIC const tables are in the private pack, outside the APK.

## Device checks

On an attached AYN Thor, the northeast black polygons are gone. Checks cover
Littleroot exterior and house interior, Rustboro, Slateport and a Route 101
voxel battle at 4x resolution. Normal rendering stays around 59.6 frames/s,
with two pictures per tick and swaps averaging 8.38 ms at 120 Hz. Map-load
stalls no longer switch the pacing mode.

The bottom screen dims to 20% after idle. The remap page swaps A/B, persists
across restart and resets to defaults. The system document picker imports a
valid pack. Save and settings hashes match their pre-test backups. Changing
speed to 1.5x and back now saves successfully and keeps rendering; this caught
and fixed the shim's incompatible static lock initializer.

Presentation, companion Activity and inline routes were exercised. Screen
off pauses the engine and audio; wake resumes them and recovers 120 Hz.
Both windows now request the same refresh preference because a 60 Hz
companion vote could keep the top panel at 60 Hz after wake on this firmware.
The bottom compositor still presents only when needed.

## 3DS comparison

With build provenance and the deliberately random link ID held fixed, the
3DS executable segments, loader relocation tables and SMDH match commit
`4debcc9` byte-for-byte. The data ABI is unchanged (`7e05a663`). The complete
3DSX differs in RomFS: the shared symbol-index correction adds six valid
external relocation sites to `gamedata.rel` (24 bytes).
Full-container byte identity is therefore not claimed.

Sanitized host tests used a `gcc -no-pie` wrapper under WSL, with
`ASAN_OPTIONS=handle_segv=0:detect_leaks=0`, to avoid sanitizer runtime crashes
observed with the default PIE configuration. Address/undefined instrumentation
remained enabled. No physical 3DS run was performed.

Long-duration battery drain, every weather condition, physical hinge events,
other firmware and panels that refuse the requested mode remain broader
hardware coverage. The device tests were USB-powered and do not establish
battery-life figures. These local release assets have not been published.
