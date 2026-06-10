# Magnifier

A small, fast Windows overlay magnifier built for streamers, presenters, and
anyone who needs an on-demand zoom that follows the mouse.

> **Status**: v0.1 — works end-to-end (lens, fullscreen, controller, hotkeys,
> tray, CLI/IPC, settings UI). See [`docs/DESIGN.md`](docs/DESIGN.md) for the
> full architecture write-up.

---

## Features

- **Lens mode** — a transparent, click-through magnifier that follows the
  mouse cursor; configurable size and smoothing.
- **Fullscreen mode** — magnifies the entire active display; input is
  correctly routed (clicks land where you expect).
- **Hotkeys** — global, rebindable (`RegisterHotKey` by default, optional
  low-level keyboard hook for keys other apps swallow).
- **Controller support** — Xbox / PS-via-Steam-Input / generic XInput pad
  moves the lens and changes zoom. Hot-pluggable, no driver required.
- **System tray icon** — left-click toggles lens; right-click for menu.
- **ImGui settings window** — live editing of every tunable. Atomic save.
- **CLI + IPC** — `Magnifier.exe --lens`, `--zoom 3`, `--toggle`, `--status`,
  `--quit`, plus a named-pipe and optional HTTP loopback for OBS / StreamDeck
  / scripting integrations.
- **Streamer-friendly** — capture-exclude support on Windows 10 2004+ so the
  overlay does not appear in OBS unless you want it to.
- **Single-instance** — second launches forward their CLI arg to the already-
  running instance.
- **Resilient** — minidump on crash, atomic config save, named-pipe ACL
  restricted to the current user, log rotation.

---

## Quick start

1. Download the latest MSI or portable ZIP from the
   [Releases](https://github.com/local/magnifier/releases) page.
2. Install (MSI) or unzip somewhere (portable). Run `Magnifier.exe`.
3. A small magnifying-glass icon appears in the tray.
4. Default hotkeys:

   | Action | Hotkey |
   |---|---|
   | Toggle lens         | `Ctrl + Alt + Z` |
   | Toggle fullscreen   | `Ctrl + Alt + X` |
   | Turn off            | `Ctrl + Alt + Q` |
   | Zoom in / out       | `Ctrl + Alt + =` / `Ctrl + Alt + -` |
   | Recenter on cursor  | `Ctrl + Alt + C` |
   | Reload config       | `Ctrl + Alt + R` |
   | Settings            | `Ctrl + Alt + ,` |

   Every hotkey is rebindable in **Settings → Hotkeys**, or by editing
   `%APPDATA%\Magnifier\config.toml`.

---

## Command-line examples

```powershell
# Start in lens mode, minimised to tray.
Magnifier.exe --lens --start-minimized

# From OBS or a Stream Deck — toggle the lens on/off.
Magnifier.exe --toggle

# Bump the zoom up by 0.25x.
Magnifier.exe --zoom-in

# Resize the lens to 800x450.
Magnifier.exe --lens-size 800x450

# Ask the running instance for its current state, as JSON.
Magnifier.exe --status

# Shut down (graceful → force).
Magnifier.exe --quit
Magnifier.exe --force-quit
```

Full reference: [`docs/CLI.md`](docs/CLI.md).

---

## Building from source

Prerequisites: **Visual Studio 2019/2022 Build Tools** with the C++ workload
(MSVC ≥ 14.29), **CMake 3.20+**, **Ninja**.

```powershell
# From a 'x64 Native Tools' developer prompt:
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
build\Magnifier.exe
```

Or use the included helper script (auto-locates VS):

```powershell
scripts\build.cmd
```

Run the tests:

```powershell
build\tests\magnifier_tests.exe
```

---

## Files written by Magnifier

| Path | Purpose |
|---|---|
| `%APPDATA%\Magnifier\config.toml`           | User config (atomic save) |
| `%LOCALAPPDATA%\Magnifier\logs\magnifier.log` | Rotating log (3 × 5 MB) |
| `%LOCALAPPDATA%\Magnifier\crashes\*.dmp`    | Minidumps on crash |
| `%LOCALAPPDATA%\Magnifier\Magnifier.pid`    | PID of the running instance |

---

## Known limitations

- **Exclusive-fullscreen games**: the Magnification API cannot draw over
  truly exclusive-fullscreen surfaces. Use the game's borderless or windowed
  mode (most modern games default to borderless).
- **Anti-cheat**: enabling the low-level keyboard hook may be flagged by
  some kernel-level anti-cheat. The hook is OFF by default and is only
  needed for hotkeys that the foreground game intercepts before the OS
  routes them to `RegisterHotKey`.
- **HDR**: tone mapping is intentionally not applied — the magnifier shows
  the raw post-composition pixels.

See [`docs/DESIGN.md`](docs/DESIGN.md#known-limitations) for the full list.

---

## License

MIT — see [`LICENSE`](LICENSE).
