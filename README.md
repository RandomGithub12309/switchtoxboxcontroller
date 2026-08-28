# SwitchProXInput

Turns a **Nintendo Switch Pro Controller connected over USB** into a virtual
**Xbox 360 (XInput) controller** on Windows, so it works in games that only
support XInput. Ships with a dark, custom-drawn settings app, a system-tray
icon, and an optional "start with Windows" entry.

```
   Switch Pro Controller            SwitchProXInput             Your game
   (USB HID device)        ──▶  reads HID reports,  ──▶  sees a standard
                                 feeds ViGEmBus          Xbox 360 pad
```

![GUI](docs/gui-preview.png)

## Requirements

- **Windows 10 / 11, 64-bit**
- **ViGEmBus driver** (the virtual gamepad driver). Install once:
  1. Download the latest installer from
     <https://github.com/nefarius/ViGEmBus/releases> (e.g. `ViGEmBus_1.22.0_x64_x86_arm64.exe`)
  2. Run it and **reboot**.
  3. Sanity check: Device Manager → *System devices* → `ViGEm Bus Device`.
- A Nintendo Switch Pro Controller + USB cable (the official USB-C cable or
  any data-capable cable).

> **Downloads:** every release ships a `SHA256SUMS.txt`. Verify your copy
> before running (PowerShell):
> `Get-FileHash .\SwitchProXInput.exe -Algorithm SHA256` — compare against
> the value in the file.

## Quick start

**Ready-to-run download:** [`release/SwitchProXInput-v1.2.0-win64.zip`](release/SwitchProXInput-v1.2.0-win64.zip)
contains the 64-bit exe, `ViGEmClient.dll`, the config file, this README and
`SHA256SUMS.txt`. (GitHub: *Code → Download ZIP* works too, the files sit at
the repo root as well.)

1. Install the ViGEmBus driver (above).
2. Unzip so `SwitchProXInput.exe`, `ViGEmClient.dll` and
   `SwitchProXInput.ini` sit in one folder.
3. Run `SwitchProXInput.exe`, plug in the controller, and start your game.
   The window shows driver and controller status live; games will see an
   "Xbox 360 Controller". Verify at [gamepad-tester.com](https://gamepad-tester.com).

## The app

The app is a regular desktop window: standard title bar (minimize, maximize,
close), resizable, with a dark theme on Windows 10/11. If the window is
smaller than the layout — on small screens or high DPI scaling — the content
scrolls (mouse wheel or the slim scrollbar) so every setting stays reachable.

- **Status panel** — ViGEmBus driver state (with a *Retry* button if the
  driver isn't installed yet) and per-controller connect/battery info.
  Hot-plug is fully supported, up to 4 pads.
- **Controller** — face-button layout, Capture/Home mapping, L/ZL + R/ZR swap.
- **Sticks** — deadzone and deflection-range sliders per stick, plus X/Y
  invert toggles. Changes apply live while you drag.
- **Anti-drift** — automatic stick-drift detection and correction, see
  [Stick drift fix](#stick-drift-fix). The card shows live what kind of
  drift was detected on each stick and what the app is doing about it.
- **Features** — HD rumble passthrough, number of virtual pads.
- **Controller IDs** — VID:PID list for third-party pads.
- **System** — *Start with Windows* (registry Run key, launches minimized),
  *Minimize to tray*, *Close to tray* (X hides instead of quitting).
- **Tray icon** — left-click/double-click reopens the window, right-click
  gives a menu (Open / Hide / Exit). A balloon explains it once on first hide.
- **Restore defaults** returns everything to factory values (and removes the
  autostart entry).

All settings are saved to `SwitchProXInput.ini` next to the exe; most apply
instantly (changing *Max controllers* or the device list restarts the
controller grab).

## Default button mapping

| Switch Pro Controller | Xbox 360 (XInput)        |
|-----------------------|--------------------------|
| Left stick / L3       | Left stick / LS          |
| Right stick / R3      | Right stick / RS         |
| D-pad                 | D-pad                    |
| A / B / X / Y         | **Position-matched**: the Xbox letter that sits in the same spot (see note below) |
| L / R                 | LB / RB                  |
| ZL / ZR               | LT / RT (digital → full scale) |
| **−** / **+**         | Back / Start             |
| Home                  | Guide (Xbox button)      |
| Capture               | unmapped (configurable)  |

> **Face-button note:** the Pro Controller's A/B and X/Y are in opposite
> corners compared to an Xbox pad. The default maps by **physical position**
> so on-screen Xbox prompts line up with your muscle memory. Pick
> "Same labels" in the Controller card to map by labels instead.

Sticks use a radial deadzone (10% default) and are scaled so full physical
deflection reaches the full XInput range. Triggers are digital because the
Pro Controller has no analog triggers — games see them fully pressed.

## Stick drift fix

Worn sticks drift, and the app detects and corrects both drift types
automatically — no manual measuring:

| Drift type | What it is | Automatic correction |
|---|---|---|
| **Center offset** (bias) | the resting position moved away from center, e.g. the stick reports 2150 instead of 2048 while untouched — the game walks/steers on its own | the true resting center is learned while the stick is untouched and the output is re-centered on it, continuously, so slowly worsening drift keeps being cancelled |
| **Jitter** (noise) | the resting value bounces around — random motion leaks through whenever a bounce exceeds the deadzone | the noise floor is measured and the deadzone is automatically raised just enough to swallow it (never above your slider unless needed) |

Detection works by watching the raw signal: a sample cluster whose two
halves agree on the same spot is "the stick is untouched" (real motion,
however slow, keeps travelling; jitter just scatters around a fixed point).
Center learning only happens while the stick is untouched — input you make
is never absorbed into the calibration — and a center must be confirmed by
about a second of real rest before it is trusted, so a deflection you hold
in-game is never mistaken for the rest position. If a pad is plugged in
while a stick is being held, the capture self-heals as soon as the stick is
released and left alone.

Worn sticks can also make the resting spot *jump* intermittently to one
side ("pulls left"). **Aggressive** strength adds *drift chase* for that:
if a stick settles somewhere far from the learned center and sits there
without moving for a couple of seconds, that spot is adopted as the new
rest position (and the adoption self-heals if it was a mistake). The
**Anti-drift** card names the pull direction — e.g. `drifts left`.

Everything is on by default (`[DriftFix]` in the INI): correction
**Enable**, **AutoDeadzone**, and **Strength** (`0` gentle / `1` balanced /
`2` aggressive — aggressive learns faster and keeps larger safety margins
for badly worn sticks). The **Anti-drift** card in the window shows per
stick what was detected — `clean`, `center offset (X …, Y …)`,
`noisy stick (± …)` or both — and what is being applied (`auto-centered`,
`auto deadzone N%`). With **Calibrate now** you can force a fresh capture
of the resting position at any time; just don't touch the sticks for a
second or two afterwards.

## Configuration file

Everything the GUI edits lives in `SwitchProXInput.ini`:

```ini
[General]
MaxControllers=4            ; virtual pads to create (1-4)
Devices=057E:2009           ; accepted VID:PID pairs, separated by ';'

[Mapping]
ButtonLayout=0              ; 0 = Xbox positions (default), 1 = same labels
CaptureButton=0             ; 0 none, 1 Back, 2 Start, 3 Guide
HomeButton=1                ; 0 none, 1 Guide
SwapShoulders=0             ; swap L<->ZL and R<->ZR
LeftDeadzone=10             ; stick deadzone in % of range (0-90)
RightDeadzone=10
LeftStickRange=1700         ; raw deflection (max 2048) that = full XInput
RightStickRange=1700
InvertLX=0                  ; the official Pro Controller over USB reports Y
InvertLY=0                  ; the standard way; set the Y flags to 1 only for
InvertRX=0                  ; pads that read up as down (some BT-mode clones)
InvertRY=0

[Features]
EnableRumble=1              ; pass game rumble to the HD rumble motors

[DriftFix]
Enable=1                    ; automatic stick-drift correction (see above)
AutoDeadzone=1              ; deadzone never drops below the measured noise
Strength=1                  ; 0 gentle, 1 balanced, 2 aggressive

[System]
Autostart=0                 ; run at logon, minimized in the tray
MinimizeToTray=1
CloseToTray=1
```

## Changelog

- **v1.2.0** — Automatic stick-drift fix: the app detects the type of drift
  on each stick (center offset / jitter / both) and its direction,
  continuously learns the true resting center, and applies corrections plus
  a noise-floor-based adaptive deadzone. Aggressive strength adds *drift
  chase* for sticks whose resting spot intermittently snaps to one side.
  New **Anti-drift** card with live detection status, correction strength
  (gentle/balanced/aggressive) and a **Calibrate now** button; settings
  persist in the new `[DriftFix]` INI section. Includes a ready-to-run
  build in `release/`, a windres-free resource builder (`tools/mkres.py`)
  and an optional CI workflow (`ci/build-release.workflow.yml` — move it to
  `.github/workflows/` to enable automatic release builds). Parser refactored
  (`parseButtons` / `applySticks` / `parseReportCorrected`) and unit tests
  added for the parser, the drift engine, and the full correction pipeline
  (`tests/`).
- **v1.1.1** — Window fixes: standard window frame with real minimize /
  maximize / close buttons and resizable edges (dark title bar on Win10/11),
  initial size clamped to the screen's work area, content scrolls (wheel +
  scrollbar) when the window is smaller than the layout. Package also gained
  `tools/sign.ps1` + `tools/make_cert.ps1` (Authenticode signing pipeline),
  optional auto-signing in `build.bat`, and a `SHA256SUMS.txt` for verifying
  downloads. See the Smart App Control / SmartScreen troubleshooting section.
- **v1.1.0** — GUI release: custom-drawn dark settings app with live status,
  tray icon (minimize/close-to-tray, balloon hint, context menu), "start with
  Windows" (HKCU Run key + `--minimized`), sliders/toggles/dropdowns for every
  setting with live apply, DPI-aware rendering, single-instance handling.
- **v1.0.2** — Y-axis direction fixed (USB reports Y the standard way; the
  inverted convention only applies to Bluetooth Joy-Con/full-state reports).
- **v1.0.1** — Stick decoding fixed (constant "down-left" drift at rest);
  parser extracted to `parse.h` with unit tests.
- **v1.0** — Initial release.

## Building from source

The app is plain C++17/Win32 with no build-time dependencies (ViGEm is
loaded at runtime). Any of these work:

- **MSVC** — from a *Developer Command Prompt*: run `build.bat`
- **MinGW-w64** — `build.bat` with `g++` on PATH
- **CMake** — `cmake -B build && cmake --build build --config Release`

`ViGEmClient.dll` is shipped prebuilt (compiled from the official MIT-licensed
ViGEmClient source in `vendor/`). To rebuild it yourself:

```
g++ -std=c++14 -O2 -shared -static-libgcc -static-libstdc++ \
    -DVIGEM_EXPORTS -DVIGEM_DYNAMIC -Ivendor/ViGEmClient/include \
    vendor/ViGEmClient/src/ViGEmClient.cpp -o ViGEmClient.dll -lsetupapi
```

**No `windres` / cross-compiling:** `tools/mkres.py` (pure Python 3)
produces the resource object `windres app.rc -O coff app_res.o` normally
makes (icon, manifest, VERSIONINFO):

```
python3 tools/mkres.py icon.ico app.manifest app_res.o --version 1.2.0.0
```

Add `app_res.o` to the compile line in place of the `windres` output. The
shipped `SwitchProXInput.exe` can also be rebuilt with Zig's C++ frontend
(`zig c++ -target x86_64-windows-gnu -O2 -static -mwindows ...`, same libs).

Unit tests for the parser and the drift engine (no Windows needed):

```
g++ -std=c++17 tests/parse_test.cpp   -I. -o parse_test   && ./parse_test
g++ -std=c++17 tests/drift_test.cpp   -I. -o drift_test   && ./drift_test
g++ -std=c++17 tests/pipeline_test.cpp -I. -o pipeline_test && ./pipeline_test
```

or via CMake: `cmake -B build -DSXPX_BUILD_TESTS=ON && cmake --build build && ctest --test-dir build`.

### Signing your build

`tools/sign.ps1` signs `SwitchProXInput.exe` and `ViGEmClient.dll` with
Authenticode (needs `signtool.exe` from the Windows SDK — "Signing Tools"
component, <https://developer.microsoft.com/windows/downloads/windows-sdk/>):

```
powershell -ExecutionPolicy Bypass -File tools\sign.ps1 -Thumbprint <THUMBPRINT>
powershell -ExecutionPolicy Bypass -File tools\sign.ps1 -PfPath cert.pfx -PfPassword <PASSWORD>
```

`build.bat` signs automatically if the `SXPX_THUMBPRINT` environment
variable is set. `tools/make_cert.ps1` creates a self-signed certificate for
testing your pipeline — note that **self-signed certificates do not satisfy
Smart App Control or SmartScreen**; only certificates from a public CA do.


## Troubleshooting

### Smart App Control / SmartScreen blocks the app

Windows **Smart App Control** (Windows 11) blocks *every* unsigned
application — there is no "Run anyway" for it, and no code change can avoid
it, because it checks the file's **signature and reputation**, not its
behaviour. This app is open source and shipped unsigned, so a fresh Windows
11 machine with Smart App Control enabled will refuse to start it. Your
options:

1. **Turn Smart App Control off** (the usual choice for open-source tools).
   *Settings → Privacy & security → Windows Security → App & browser
   control → Smart App Control settings → **Off***. Note: on Windows 11
   22H2/23H2 this is permanent until you reset or reinstall Windows; on
   24H2 and later it can be switched back on. Before you run anything
   unsigned, verify its checksum (see the note under *Requirements*).
2. **Keep it on and sign the app** with a certificate from a trusted CA.
   EV certificates get instant trust; standard (OV) certificates pass once
   they build reputation (a few weeks of normal downloads). Once you have a
   certificate, signing is one command — see *Signing your build* below.
3. **Submit the files for reputation** to Microsoft's security portal
   (<https://www.microsoft.com/en-us/wdsi/filesubmission>). This builds
   cloud reputation over time and also resolves antivirus false positives.

The separate **SmartScreen** prompt ("Windows protected your PC") *does*
have a bypass: **More info → Run anyway**. If that's what you see, no
settings need to change.

**"Driver not connected" in the status panel** — ViGEmBus isn't installed (or
the service didn't start after the reboot). Install it from
<https://github.com/nefarius/ViGEmBus/releases>, reboot, press *Retry*.

**SmartScreen warning on the exe** — the binary is unsigned (normal for open
source tools). "More info → Run anyway", or build it yourself.

**Game gets double inputs / wrong buttons** — if Steam is running, disable
*Settings → Controller → "Switch Pro Configuration Support"* so Steam doesn't
also grab the raw controller. In games with a native controller list, ignore
the raw "Pro Controller" entry and use the virtual Xbox 360 pad.

**Controller not detected** — try another cable/port. Only the official Pro
Controller ID (`057E:2009`) is recognized by default; add your pad's ID under
*Controller IDs* (e.g. `057E:2009;057E:200E`). Some docks/hubs mangle USB
enumeration.

**Stick feels too sensitive / not reaching full tilt** — drag the
*Left/Right stick range* sliders (lower = more sensitive) and the deadzones.

**My sticks drift (character walks / camera moves on its own)** — this is
what the **Anti-drift** card is for; correction is on by default. Leave the
sticks untouched for a second or two after plugging the pad in so their
resting position can be captured — the status line then shows what was
detected and applied. For a badly worn pad set *Correction strength* to
*Aggressive*, or press **Calibrate now** and don't touch the sticks until
the line reports the result. If the drift is extreme (the stick is
physically loose), the fix cancels the resting error but the stick's
usable range is still reduced — keep *Auto deadzone from noise* on.

**Up/down read swapped on the sticks** — the official Pro Controller over USB
reports Y the standard way, and the defaults match that. If your pad reports
Y inverted, switch the LY/RY toggles on.

**Rumble not working** — some games only rumble real XInput devices on player
1; also check the HD rumble toggle. Rumble passthrough writes the Switch
"rumble only" report (0x10) — on a few third-party pads this is ignored.

## How it works

- The Pro Controller is a plain USB **HID device** (`VID 057E / PID 2009`);
  over USB it streams full input reports (`0x30`, 64 bytes) at 60–120 Hz
  without any handshake. `engine.cpp` reads them with the raw Win32 HID API
  (`hid.dll` + SetupAPI device enumeration) using overlapped I/O, one thread
  per controller, plus a watchdog thread for hot-plug.
- Reports are parsed (`parse.h` — buttons, 12-bit stick data, battery); the
  decoded sticks run through the per-stick drift correctors (`drift.h` —
  resting-center learning, noise-floor tracking, drift-type classification),
  and the corrected values are mapped to `XUSB_REPORT`, which is pushed to
  the **ViGEmBus** driver through `ViGEmClient.dll` (loaded dynamically at
  runtime, no import lib).
- Games rumble requests come back through the ViGEm notification callback and
  are encoded to the Switch HD-rumble format (`0x10` output report).
- The GUI (`gui.cpp`) is a custom-drawn Win32 window (GDI double-buffering,
  no framework): cards, toggle switches, sliders, dropdown popups, tray icon
  and a registry-based autostart entry.

## Notes & credits

- Bluetooth is **not** supported (USB only, as requested); Bluetooth requires
  the "simple report mode" handshake and is much less reliable.
- If you don't need a standalone tool, alternatives include Steam's built-in
  Switch Pro support and BetterJoy.
- ViGEmClient.dll: built from [nefarius/ViGEmClient](https://github.com/nefarius/ViGEmClient)
  (MIT). Report layout/rumble encoding per
  [dekuNukem's Nintendo Switch reverse-engineering docs](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering)
  and BetterJoy (MIT). See `THIRD-PARTY-NOTICES.md`.
- Project license: MIT (see `LICENSE`).
