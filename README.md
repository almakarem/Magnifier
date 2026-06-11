# Magnifier

A low-latency screen magnifier for Windows. Built for streamers, accessibility
users, photographers, retro-gaming, pixel art, and anyone who has ever
shouted at the built-in Windows Magnifier for stuttering, blurring, or
collapsing the desktop on launch.

> **Status:** pre-1.0 (current release **v0.1.4**). API surface, config
> keys, and default hotkeys may shift between minor versions. Patch
> bumps are bug fixes only.

---

## Why not just use Windows Magnifier?

The shipped Magnifier (`magnify.exe`) is fine for occasional reading, but it
has three structural problems that this project sidesteps:

1. **Lens mode jitters** because the system magnifier ticks on the
   regular Windows timer, which is coalesced at low load. This project
   uses a high-resolution waitable timer pinned to the display's
   refresh rate, then calls `DwmFlush()` once per tick so the
   magnified frame composites in the same vsync interval that produced
   it. Result: no smear trails, even at 240 Hz.
2. **Full-screen mode hijacks the desktop** and on multi-monitor setups
   it routinely picks the wrong display. This project keeps the lens
   confined to the monitor your cursor is on and lets you move it
   between displays without re-entering the mode.
3. **You cannot rebind a single hotkey** without registry surgery.
   Here every action is rebindable from a UI tab, with conflict
   detection for the GPU rotation defaults that Intel / AMD ship.

If you want pure accessibility (high-contrast colour inversion,
keyboard navigation reading, screen-reader integration) the built-in
Magnifier is still the better choice. If you want a fast, low-latency
zoom tool with a controller-friendly UX, keep reading.

---

## Features

- **Lens mode** — a movable rectangle that magnifies whatever is under
  it. Follows the mouse by default; recenter / nudge from the keyboard
  or a game controller.
- **Full-screen mode** — magnifies the entire desktop with the cursor
  as the anchor.
- **Configurable smoothing** — independent time-constants for position
  and zoom (`position_tau`, `zoom_tau`) so you can dial in everything
  from "instant" to "buttery cinematic".
- **Refresh-rate aware** — auto-detects the monitor's `dmDisplayFrequency`
  (re-queried on `WM_DISPLAYCHANGE`) and ticks the magnifier loop once
  per panel refresh. Pinning to vsync via `DwmFlush()` eliminates the
  low-zoom ghost trails common on 240 Hz panels.
- **Controller support** across three back-ends:
  - **XInput** for Xbox 360 / Xbox One / Xbox Series pads and most
    third-party Xbox-style controllers.
  - **Windows.Gaming.Input** (WGI) for the modern UWP path — picks
    up Bluetooth Xbox controllers, Sony DualSense (over USB), and
    most Steam Input virtual gamepads.
  - **Raw HID** for everything else, including DualShock 4 / DualSense
    not running through DS4Windows.
- **Rebindable hotkeys** for every action, with conflict warnings
  surfaced inline (e.g. if you bind `Ctrl+Alt+Left` and your Intel /
  AMD GPU has display rotation enabled, you'll see a yellow banner).
- **Local IPC over a named pipe** so `magnifier --toggle` from any
  shell controls the already-running instance instead of spawning a
  duplicate.
- **Tray-first UX** with a hover tooltip that surfaces the current
  mode + key shortcuts, a right-click menu for every action, and a
  one-shot welcome balloon on first run.
- **Self-updater** that polls GitHub Releases anonymously, downloads
  the new MSI, and re-launches `msiexec /i /passive`. One network
  request, no telemetry, no PAT used. Disable from Settings → Updates.
- **Crash dumps** automatically written to
  `%LOCALAPPDATA%\Magnifier\crashes\` on any unhandled exception, so
  you can attach a `.dmp` to a bug report instead of describing the
  problem from memory.

---

## Install

### MSI (recommended)

1. Download the latest `Magnifier-x64.msi` from the
   [Releases page](https://github.com/almakarem/Magnifier/releases).
2. Double-click it. The installer is silent-capable
   (`msiexec /i Magnifier-x64.msi /passive`) and creates a Start Menu
   entry.
3. Launch from the Start Menu, or run `Magnifier.exe` from
   `%ProgramFiles%\AMSA\Magnifier\`.

### Portable ZIP

1. Download `Magnifier-x64-portable.zip` from the Releases page.
2. Unzip anywhere (USB stick, network share, etc.).
3. Run `Magnifier.exe`. Config + logs land in `%LOCALAPPDATA%\Magnifier\`
   (the portable variant does not write to `Program Files`).

### SmartScreen warning

The release binaries are not currently EV-code-signed, so Windows may
show "Windows protected your PC" the first time you run them. Click
**More info → Run anyway**. The binary is being submitted to Microsoft
for SmartScreen analysis so it can be white-labeled across users; the
warning typically disappears after a few hundred organic downloads or
after Microsoft completes the analysis.

If you would rather verify before running, use either of:

```powershell
Get-FileHash .\Magnifier-x64.msi -Algorithm SHA256
```

```powershell
# Confirm digital signature (when signed)
Get-AuthenticodeSignature .\Magnifier-x64.msi
```

The SHA-256 hash of each release artifact is included in the release
notes on the GitHub Releases page.

---

## Quick start

After install, Magnifier sits silently in the tray. The defaults are:

| Action | Default hotkey | What it does |
|---|---|---|
| **Toggle lens** | `Ctrl+Alt+Z` | Show / hide the lens rectangle. |
| **Toggle full-screen** | `Ctrl+Alt+F` | Switch the whole desktop into magnified mode. |
| **Turn off** | `Ctrl+Alt+X` | Exit any active magnification mode. |
| **Show settings** | `Ctrl+Alt+S` | Open the Settings window. |
| **Zoom in** | `Ctrl+Alt++` | Increase zoom by one step. |
| **Zoom out** | `Ctrl+Alt+-` | Decrease zoom by one step. |
| **Lens size up** | `Ctrl+Alt+]` | Grow the lens rectangle. |
| **Lens size down** | `Ctrl+Alt+[` | Shrink the lens rectangle. |
| **Recenter** | `Ctrl+Alt+C` | Snap the lens / zoom back to the cursor. |
| **Pan left / right / up / down** | unbound | Move the lens without using the mouse. |
| **Reload config** | unbound | Re-read `config.toml` from disk. |
| **Quit** | unbound | Exit the application. |

Every binding above is rebindable from **Settings → Hotkeys**. The
"unbound" actions are intentional defaults to avoid colliding with
common keyboard shortcuts; assign them to whatever you like.

> **GPU rotation conflict:** Intel and AMD display drivers default to
> reserving `Ctrl+Alt+Arrow` for screen rotation. If you bind any
> `pan_*` action to those combos the Hotkeys tab will show a yellow
> warning banner — fix it from your GPU control panel or pick a
> different combo.

---

## Settings reference

The Settings window opens with `Ctrl+Alt+S` (or right-click the tray
icon → **Settings…**). It's organised by tab. Every change persists
immediately to `config.toml`.

### General

- **Start with Windows** — registers a per-user `Run` key entry.
  Stored under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
  Safe to toggle off; does not require admin.
- **High process priority** — sets `ABOVE_NORMAL_PRIORITY_CLASS` while
  a mode is active and reverts to `NORMAL_PRIORITY_CLASS` when off.
  Concern: on heavily loaded machines this can briefly starve background
  audio / capture threads. Leave off if you record OBS with software
  encoders.

### Lens

- **Width / Height** — the lens rectangle in screen pixels.
- **Follow mouse** — lens center tracks the cursor. Off = use
  `pan_*` hotkeys or the controller to move it.
- **Magnify cursor** — when on, the cursor itself is magnified (Win32
  `MAG_SHOWMAGNIFIEDCURSOR`). When off, the magnified surface shows
  the desktop but the cursor stays at OS-native size — useful for
  precision pixel work where you want to see exactly which pixel the
  hotspot is on.
- **Position smoothing (τ)** — exponential time-constant in seconds
  for lens position easing. `0.0` = instant; `0.15` = cinematic.
- **Zoom smoothing (τ)** — same idea for zoom changes triggered by
  `zoom_in` / `zoom_out`.

### Zoom

- **Minimum zoom / Maximum zoom** — clamps the user-configurable
  zoom range. Clamping is enforced both for the slider and for
  programmatic `--zoom` IPC calls.
- **Default zoom** — applied when entering a magnification mode.
- **Step** — used by `zoom_in` / `zoom_out` and `--zoom-in` /
  `--zoom-out` CLI flags.

### Controller

- **Enable controller** — master switch for all three back-ends.
- **Back-end priority** — XInput is polled first because it is the
  lowest-latency path; WGI fills in for modern Bluetooth controllers;
  raw HID catches devices the first two ignore.
- **Deadzones** — separate values for stick X / Y and trigger axes.
- **Cursor speed** — multiplier for the virtual cursor when the
  controller owns it (suppresses follow-mouse while a stick is held).

The Controller tab will list "0 of N devices" if `controller.enabled`
is `false`; flip the switch and the count updates within a poll cycle
(~16 ms).

### Capture / OBS

- **Window class hint** — surfaces the underlying class name
  (`MagnifierWindow`) so OBS / streaming tools can target Magnifier
  with **Window Capture (BitBlt)** or **Window Capture (Windows 10)**.
- Concern: WGC (Windows.Graphics.Capture) on some GPU drivers shows
  a yellow border around captured windows. If that's a problem,
  switch OBS to **Display Capture** of the monitor Magnifier is on
  and crop.

### Advanced

- **Use low-level keyboard hook** — replaces `RegisterHotKey` with a
  `WH_KEYBOARD_LL` hook. Required in some corporate environments
  where `RegisterHotKey` is blocked. Concern: low-level hooks are
  flagged by some AV vendors; expect a one-time SmartScreen / AV
  prompt the first time you toggle this on.
- **Reset all settings** — wipes `config.toml` back to factory
  defaults and re-bakes the embedded defaults next launch.

### Updates

- **Check now** — runs the updater on-demand. The only network call
  the app makes: an anonymous `GET https://api.github.com/repos/almakarem/Magnifier/releases/latest`.
- **Auto-check on startup** — same call, run once a day, cached.
- **Source repository** is read-only and pinned to
  `https://github.com/almakarem/Magnifier`. Forks should fork the
  repo and rebuild; we deliberately removed the editable Owner /
  Repo / PAT fields in 0.1.4 to avoid users pointing the updater at
  arbitrary repos.

### Hotkeys

- One row per action. Click the binding cell to capture a new combo
  (Esc cancels). Conflicting bindings — both *between Magnifier
  actions* and against known *GPU shortcuts* — surface inline.
- "Show settings" is in the table; rebind it freely. The default is
  `Ctrl+Alt+S`.

### Diagnostics

- **Hotkey state** — currently-armed combos, last fire time, conflict
  count.
- **Controller state** — number of XInput slots in use, number of
  WGI gamepads, raw HID devices.
- **Reading guide** — short text explaining what each number means
  and what "expected" looks like.

### About

- Author bio, source link, donations (Ko-fi, PayPal), contact channels
  (Discord, X, email), acknowledgements, and the MIT license
  attribution.

---

## Controller support in detail

Magnifier polls three back-ends concurrently and merges their state.
This is intentional: a single physical controller often shows up on
more than one path (e.g. an Xbox pad over Bluetooth appears in WGI
but not XInput), and there is no robust de-duplication API. We rely
on the user to plug in one controller at a time and trust the
back-end priority.

| Back-end | Best for | Latency | Setup |
|---|---|---|---|
| XInput 1.4 | Xbox / Xbox-style wired | lowest | none |
| Windows.Gaming.Input | Bluetooth Xbox, DualSense (USB) | low | none |
| Raw HID | DS4 / DualSense without DS4Windows | medium | none |

**DS4Windows users:** keep DS4Windows running. It re-exposes your
DS4 / DualSense as a virtual XInput device, which Magnifier picks
up via the XInput back-end (lowest latency path). No extra config.

---

## Command line

`Magnifier.exe` is also a CLI. When invoked with any IPC flag below,
it forwards to the running instance over a named pipe and exits;
when invoked without flags it launches normally.

```text
General:
  -h, --help                 Print help and exit.
  -v, --version              Print version and exit.
  --start-minimized          Start in the tray with no balloon.

Mode control (IPC):
  --lens                     Enter lens mode.
  --fullscreen               Enter full-screen mode.
  --off                      Exit current mode.
  --toggle                   Toggle lens on / off.
  --status                   Print JSON status to stdout.

Zoom and lens (IPC):
  --zoom <factor>            Set zoom to <factor> (clamped).
  --zoom-in [step]           Increase zoom (default step from config).
  --zoom-out [step]          Decrease zoom (default step from config).
  --lens-size <w>x<h>        Set lens dimensions.

Process control (IPC):
  --quit                     Graceful shutdown.
  --force-quit               Skip cleanup; for stuck instances only.
  --reload-config            Re-read config.toml from disk.

Controller:
  --enable-controller        Turn controller polling on.
  --disable-controller       Turn controller polling off.
```

Example: bind a stream-deck button to `Magnifier.exe --zoom 4` to
flick to 4× magnification on demand without giving the deck focus
of your input app.

---

## Files Magnifier writes

| Path | Contents | When |
|---|---|---|
| `%LOCALAPPDATA%\Magnifier\config.toml` | All settings | On any setting change. |
| `%LOCALAPPDATA%\Magnifier\logs\magnifier.log` | Rotating log file | Continuously; rotated at 5 MB, 3 files kept. |
| `%LOCALAPPDATA%\Magnifier\crashes\*.dmp` | Mini-dumps | Only on unhandled exception. |
| `%LOCALAPPDATA%\Magnifier\magnifier.pid` | PID of the running instance | At startup; removed on graceful exit. |
| `\\.\pipe\Magnifier-IPC-<user-sid>` | Named pipe | While the app is running. |

Uninstalling via the MSI does **not** wipe `%LOCALAPPDATA%\Magnifier\`
(your settings survive across upgrades). Delete the folder manually
to factory-reset.

---

## Privacy & telemetry

Magnifier makes exactly **one** kind of outbound network request, and
only if you keep the updater enabled:

- `GET https://api.github.com/repos/almakarem/Magnifier/releases/latest`
  — public endpoint, no authentication, no PAT, no cookies. Response
  is parsed locally; the only thing transmitted is the standard
  GitHub `User-Agent`.

That's it. There is:

- **No analytics** of any kind. No usage pings, no error reporting
  service, no Sentry, no GA, no Mixpanel.
- **No background telemetry.** The updater check runs at most once
  per day and only when enabled.
- **No third-party SDKs** that phone home.
- **No PAT** is requested or stored anywhere. The Updates tab is
  read-only on the repository identity for exactly this reason.

If you want to verify, the relevant code is `src/update/Updater.cpp`
and `src/ipc/IpcServer.cpp` (local-only named pipe, ACL'd to the
current user).

---

## Known limitations

- **Exclusive full-screen games** bypass the Win32 Magnification API.
  Use borderless full-screen / windowed mode.
- **HDR.** The Magnification API runs in SDR. On HDR-enabled displays
  the magnified surface looks dimmer than the source. There is no
  workaround at the API level today.
- **Anti-cheat.** Some kernel-mode anti-cheats (Vanguard, EAC in
  some titles, BattlEye in some titles) flag any process injecting a
  capture surface. Magnifier doesn't inject — it asks DWM via the
  Magnification API — but you may still hit a false positive. If
  this affects you, quit Magnifier before launching the game.
- **Per-monitor DPI scaling.** Settings inherits the launching
  monitor's scale factor and does not re-scale if you drag it across
  displays at different DPI. The magnification view itself is DPI-
  aware.

---

## Build from source

Requirements: Visual Studio 2019+ (or Build Tools), CMake 3.20+,
PowerShell 5+, Windows 10 SDK 10.0.19041+. WiX is optional, only
needed for the MSI step.

```cmd
git clone https://github.com/almakarem/Magnifier.git
cd Magnifier
scripts\build.cmd
build\tests\magnifier_tests.exe --gtest_brief=1
```

The build script bootstraps all dependencies via CMake's FetchContent
(spdlog, tomlplusplus, nlohmann/json, imgui, googletest). No vcpkg
or conan needed.

To package an MSI:

```cmd
scripts\release.cmd
```

This produces `artifacts\Magnifier-x64.msi` and a matching portable
zip. WiX 3.14+ must be on `PATH`.

### Custom app icon

The build is icon-optional. To bake the app icon into the executable:

1. Place a square PNG (≥256×256, transparent background recommended)
   at `app/icon.png`.
2. Run `pwsh scripts/make-icon.ps1`. This produces
   `app/Magnifier.ico` (multi-resolution 16/24/32/48/64/128/256
   PNG-encoded entries).
3. Re-run `cmake -S . -B build` (CMake re-checks `EXISTS app/Magnifier.ico`
   and defines `HAVE_ICON` for both targets).
4. Re-run `scripts\build.cmd`.

Without `app/Magnifier.ico`, the build still succeeds and the tray
falls back to `IDI_APPLICATION`.

---

## License

MIT — see [LICENSE](LICENSE).

> Copyright (c) 2026 AMSA (https://github.com/almakarem)
>
> Permission is hereby granted, free of charge, to any person
> obtaining a copy of this software and associated documentation
> files (the "Software"), to deal in the Software without
> restriction, including without limitation the rights to use,
> copy, modify, merge, publish, distribute, sublicense, and/or
> sell copies of the Software…

Acknowledgements: this project links against [spdlog](https://github.com/gabime/spdlog),
[tomlplusplus](https://github.com/marzer/tomlplusplus),
[nlohmann/json](https://github.com/nlohmann/json),
[Dear ImGui](https://github.com/ocornut/imgui),
and [GoogleTest](https://github.com/google/googletest), all under
their respective permissive licenses.

---

## Author and contact

**AMSA**

- GitHub: [@almakarem](https://github.com/almakarem)
- X / Twitter: [@AlmakaremA](https://x.com/AlmakaremA)
- Email: <AMSAbualmakarem@gmail.com>
- Discord: `AMSA`

### Donations

If Magnifier saves you time or makes streaming / accessibility work
easier, a tip is hugely appreciated and goes directly toward the
EV code-signing certificate that will retire the SmartScreen warning:

- [Ko-fi](https://ko-fi.com/almakarem)
- [PayPal](https://paypal.me/almakarem)
