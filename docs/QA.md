# Magnifier — Manual QA checklist

This is the smoke-test pass that has to be done on a real Windows machine
before tagging a release. Tests must pass on **Windows 10 1809+** and
**Windows 11**.

## Setup

- [ ] Fresh Windows 10 22H2 VM, 1080p single monitor.
- [ ] Fresh Windows 11 23H2 VM, dual monitor (1080p + 4K @ 150 % scaling).
- [ ] Xbox One controller (wired) and a DualSense routed via Steam Input.

## 1. Single-instance gate

- [ ] First launch: tray icon appears within 1 s.
- [ ] Second launch with no args: settings window opens (no second icon).
- [ ] `Magnifier.exe --quit` from a third shell: tray icon disappears, pid
      file is removed from `%LOCALAPPDATA%\Magnifier\`.

## 2. Lens mode

- [ ] `Ctrl+Alt+Z` shows the lens; cursor remains usable.
- [ ] Lens follows the mouse with no perceivable lag at 60 Hz.
- [ ] Clicks pass through to apps behind the lens.
- [ ] Lens does not appear inside itself (no feedback loop).
- [ ] Resize via `Ctrl+Alt+]` / `[` works; bounds-clamped.

## 3. Fullscreen mode

- [ ] `Ctrl+Alt+X` magnifies the active monitor.
- [ ] Click on a visible button hits the *button*, not the magnified
      position (input transform is correct).
- [ ] `Ctrl+Alt+0` resets to 1.0x; magnifier disables cleanly.

## 4. Controller

- [ ] Pad connected at launch: left-stick moves lens, right-stick changes
      zoom.
- [ ] Pad hot-plugged after launch: detected within ~1 s.
- [ ] After `idle_recenter_seconds` of zero-stick, lens re-locks to mouse.
- [ ] `Back` button turns magnifier off.

## 5. Hotkey rebinding

- [ ] Rebind toggle_lens to `Ctrl+Shift+L`, Apply, verify it works.
- [ ] Bind to a chord another app owns (e.g. `Win+Tab`) → conflict line
      shown in settings, no crash.

## 6. CLI

- [ ] `Magnifier.exe --lens` from a stopped state: launches in lens mode.
- [ ] `Magnifier.exe --zoom 5` while running: lens jumps to 5×.
- [ ] `Magnifier.exe --status` prints JSON; `mode` field is correct.
- [ ] `Magnifier.exe --bogus-flag` → exit code 2, help printed.

## 7. Streamer-mode

- [ ] OBS Window Capture of Notepad: lens overlay is **not** visible
      (capture exclusion working on W10 2004+).
- [ ] OBS Display Capture: lens overlay **is** visible by default; toggle
      `[capture] mode = "exclude_self"` in config + reload, no longer visible.

## 8. Games (borderless)

- [ ] Cyberpunk 2077 / any modern AAA in borderless: hotkey toggles lens
      over the game; no input lag introduced.
- [ ] Same game restarted in exclusive-fullscreen: magnifier intentionally
      does not draw (documented limitation).

## 9. Display reconfiguration

- [ ] Plug/unplug second monitor while magnifier is in fullscreen → no
      crash; bounds re-seeded.
- [ ] Change primary display DPI → magnifier survives.
- [ ] Laptop sleep → wake: controller poll resumes; no zombie threads.

## 10. Robustness

- [ ] Delete `config.toml`, relaunch → recreated from defaults, no
      warnings beyond a single info line.
- [ ] Corrupt `config.toml` (insert garbage) → app starts, warnings are
      logged, defaults substituted.
- [ ] Send a 1 MB payload to the named pipe → over-length lines dropped;
      app stays responsive.
- [ ] Crash test: `Magnifier.exe --crash` (debug builds) → minidump
      written to `%LOCALAPPDATA%\Magnifier\crashes\`, no Windows error
      dialog.

## 11. Uninstall

- [ ] MSI: uninstall removes program files, Start-Menu shortcut, and the
      optional Run-at-login registry entry. **Does not** delete
      `%APPDATA%\Magnifier\config.toml` (user data).
- [ ] Portable: deleting the folder removes the binary. User data lives
      in `%APPDATA%`/`%LOCALAPPDATA%` and survives.

## 12. Performance baseline

With lens mode at 2× on a 1080p screen:

- [ ] CPU < 3 % on idle desktop (Ryzen 7 5800X reference).
- [ ] GPU < 2 % integrated, < 1 % discrete.
- [ ] RSS < 80 MB after 1 hour with no leaks (verify via Task Manager).
