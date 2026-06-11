# Changelog

All notable changes to **Magnifier** are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project follows a loose [Semantic Versioning](https://semver.org/spec/v2.0.0.html)
(pre-1.0: minor bumps may include breaking changes; patch bumps are fixes only).

## [Unreleased]

## [0.1.4] - 2026-06-11

### Added
- **MIT LICENSE** at the repository root. The codebase is now MIT-licensed
  to AMSA; redistribution and commercial use are permitted with attribution.
- **About tab** with author bio, donation links (Ko-fi, PayPal), contact
  channels (Discord, X, email), source link, acknowledgements, and the
  license attribution.
- **Updates tab** is now read-only: the source repository is fixed at
  `https://github.com/almakarem/Magnifier`, so the previous editable
  Owner / Repo / PAT fields were removed. The check itself is unchanged
  (one anonymous GET to `api.github.com`).
- **`show_settings` hotkey** with a default binding of `Ctrl+Alt+S`.
  Rebindable from Settings → Hotkeys like every other action. The
  `App::ShowSettings` action and the rebind row already existed;
  this just makes it discoverable out of the box.
- **First-run welcome toast.** On first launch (no `config.toml`
  present), Magnifier shows a tray balloon naming the current
  `toggle_lens` and `show_settings` hotkeys plus a hint about the
  right-click tray menu.
- **Tray hover tooltip** now shows the current mode plus the active
  `toggle_lens` and `show_settings` shortcuts, so you can confirm the
  key combo without opening Settings. Auto-updates after any rebind.
- **App icon support.** The .rc and tray load `IDI_APP` from
  `app/Magnifier.ico` when present; the build is conditional via the
  CMake `HAVE_ICON` definition so builds without the icon file still
  succeed. `scripts/make-icon.ps1` converts a square `app/icon.png`
  into a multi-resolution `.ico` (16/24/32/48/64/128/256, PNG-encoded).
- **`.github/ISSUE_TEMPLATE/`** with structured `bug.yml` and
  `feature.yml` forms plus `config.yml` to gate blank issues and add
  a direct-email contact link.
- **Exhaustive README rewrite** covering every Settings tab, every
  default hotkey, controller support tiers, GPU-rotation conflict
  guidance, CLI flags, the files Magnifier writes, and a privacy
  statement.

### Changed
- **Refresh-rate-aware tick loop.** The main tick now matches the
  refresh rate of the monitor under the cursor (re-queried on
  `WM_DISPLAYCHANGE`) instead of running at a fixed 250 Hz. Combined
  with a per-tick `DwmFlush()` while a magnification mode is active,
  this pins the magnifier loop to DWM's vsync. Fixes the low-zoom
  ghost trails reported on 240 Hz panels.
- **Default updater repository owner** is hardcoded to `almakarem` so
  fresh installs (and reset configs) check the official source.
- **Resource metadata.** Version info, MSI manifest, copyright, and
  product description all updated for AMSA / MIT.

### Fixed
- Crash-dump handler verified wired (was already present in
  `src/util/Crash.cpp` and installed from `src/main.cpp`); unhandled
  exceptions now produce a `.dmp` under `%LOCALAPPDATA%\Magnifier\crashes\`.

### Build
- `magnifier_core` now sees `app/` on its include path so generated
  resource headers resolve from any translation unit in the library.
- `CMakeLists.txt` project version bumped to **0.1.4**.

## [0.1.3] - 2026-06-10

### Added
- Donation link surfaced in the About tab.
- Updates tab strip-down (initial cut, finalised in 0.1.4).

### Fixed
- Diagnostics tab now lists Windows.Gaming.Input devices that respond
  before the polling thread enumerates them.

## [0.1.2] - 2026-06-09

### Added
- Self-updater that polls GitHub Releases, downloads the new MSI, and
  re-launches `msiexec /i /passive`.
- Crash dump writer (`MiniDumpWriteDump`) gated by
  `SetUnhandledExceptionFilter`.

### Changed
- Settings window render throttled to ~60 Hz when visible.

## [0.1.1] - 2026-06-08

### Added
- Yellow GPU-rotation conflict warning in the Hotkeys tab when the
  user has bound a combo that collides with Intel/AMD display
  rotation defaults (Ctrl+Alt+arrows).
- Configurable low-level keyboard hook toggle (Settings → Advanced)
  for users in environments that block `RegisterHotKey`.

### Fixed
- Pan hotkeys (`pan_left`/`pan_right`/`pan_up`/`pan_down`) now
  serialise correctly when written back to `config.toml`.

## [0.1.0] - 2026-06-07

Initial public release.

### Added
- Low-latency lens and full-screen magnification modes built on the
  Win32 Magnification API.
- Configurable smoothing (`position_tau`, `zoom_tau`), zoom range,
  lens dimensions, and follow-mouse behaviour.
- Hotkey-driven workflow with sensible Ctrl+Alt defaults for toggle,
  zoom, lens size, recenter, and quit.
- Controller support across XInput, Windows.Gaming.Input, and raw HID
  (DS4Windows-compatible) with per-axis deadzones and a virtual cursor.
- Local IPC (named pipe) for command-line control of a running instance.
- Tray icon with menu, balloon notifications, and minimal CPU/GPU
  footprint when idle.
- Settings window (ImGui + D3D11) with tabs for General, Lens, Zoom,
  Capture, Controller, IPC, Advanced, Updates, Hotkeys, Diagnostics,
  About.

[Unreleased]: https://github.com/almakarem/Magnifier/compare/v0.1.4...HEAD
[0.1.4]: https://github.com/almakarem/Magnifier/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/almakarem/Magnifier/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/almakarem/Magnifier/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/almakarem/Magnifier/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/almakarem/Magnifier/releases/tag/v0.1.0
