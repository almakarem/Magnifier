# Magnifier — Design & Architecture

This document explains everything the user asked for at the start of the
project: what the app does, why each subsystem was chosen, how the pieces
fit together, and where the tricky bits live. It is the single canonical
reference — read it before changing any of the headers under `src/`.

---

## 1. Goals (verbatim from the brief)

1. **Behave like Microsoft Magnifier** — full-screen zoom *and* lens mode.
2. **Overlay everything**, follow the mouse cursor as the lens centre.
3. **Accept any connected controller** (Xbox, PS, generic PC) to move the
   lens or change zoom.
4. **Reliable** — never crash the host, low resource use.
5. **Streamer-friendly** — Twitch / YouTube usable, with **rebindable**
   hotkeys.
6. **Borderless + full-screen game compatible**.
7. **CLI** to launch directly into lens or full-screen mode, plus
   shutdown / force-shutdown.
8. **One `.md` that explains everything** ← *this file*.

Open questions answered during clarification:

| Question | Decision |
|---|---|
| Distribution form | Both MSI (WiX) and portable ZIP (CPack) |
| UIAccess elevation | Skip — we run as standard user |
| Settings UI | Dear ImGui (D3D11), modal-free |
| Telemetry | Off; logs are local only |
| Language / runtime | **C++20** (MSVC), no .NET, no Electron |
| Fullscreen-game strategy | Safe path only — borderless / windowed |

---

## 2. High-level architecture

```
                      ┌────────────────────────────┐
                      │       Win32 message pump   │
                      │   (App on UI thread, ~60Hz)│
                      └────────────────────────────┘
                       ▲         ▲          ▲
                       │ actions │ commands │ frames
                       │         │          │
   ┌─────────┐  ┌──────┴───┐ ┌───┴──────┐ ┌─┴────────────┐
   │ Hotkeys │  │ TrayIcon │ │ IpcServer│ │ ControllerPoll│
   │ Manager │  │          │ │  (pipe/  │ │  (XInput,     │
   │(WM_HOT- │  │(shell32) │ │  HTTP/   │ │   ~125Hz      │
   │ KEY)    │  │          │ │  WM_COPY)│ │   worker)     │
   └─────────┘  └──────────┘ └──────────┘ └───────────────┘
        │              │            │             │
        └──────────────┴── Action / Command ──────┘
                            │
                   ┌────────▼────────┐
                   │   InputRouter   │  ←─── translates Actions / frames
                   │  (UI thread)    │       into StateModel mutations
                   └────────┬────────┘
                            │
                   ┌────────▼────────┐
                   │   StateModel    │  ←─── current + target {zoom, centre,
                   │  (mutex-guard)  │       lens size, mode, bounds}
                   └────────┬────────┘
                            │ snapshot
                   ┌────────▼────────┐
                   │  MagController  │  ←─── Win32 Magnification API
                   │  (UI thread)    │       (host HWND + WC_MAGNIFIER child,
                   │                 │        or MagSetFullscreenTransform)
                   └─────────────────┘
```

Everything that touches OS state lives on the UI thread. Workers (controller
poll, named-pipe accept, optional HTTP) marshal back via `PostMessage`.

---

## 3. Threading model & invariants

| Thread | Owns | Reads / writes |
|---|---|---|
| **UI** (the wWinMain thread) | All HWNDs, the magnifier child, hotkey registration, `SetTimer` ticks, `IpcServer::WndProc_`, `App` mutations | `StateModel` (via its mutex), `cfg_`, the cross-thread queues |
| **Controller poll** | One `std::thread` started by `ControllerPoll::Start()` | XInputGetState; writes `latest_frame_` & `action_queue_` then `PostMessage`-wakes the UI |
| **Pipe accept** | One `std::thread` per running app | `CreateNamedPipeW`/`ReadFile`; calls `Sink(Command)` (runs on this thread!) — App marshals via `cmd_queue_` |
| **HTTP serve** (optional) | One `std::thread` if `http_port > 0` | `http.sys` request loop; same sink contract as pipe |
| **spdlog async** | spdlog-owned worker | Drains the log message queue |

Invariants enforced by code:

1. `MagController` methods only run on the UI thread (asserted by host-HWND
   ownership).
2. `StateModel` is the **only** mutable cross-thread piece, and every member
   is protected by a single `std::mutex`.
3. Workers never call into `App` directly — they push a `Command` or
   `Action` onto the queue and post `WM_APP_CMD` / `WM_APP_ACTION` /
   `WM_APP_FRAME`. The drain happens on the UI thread.

Why this matters: it eliminates the entire class of bugs where a controller
poll mid-tick races against a hotkey or IPC command. There is exactly one
writer to the magnifier control.

---

## 4. The Magnification API in two modes

### 4.1 Lens mode

Implemented in `MagController::ApplyLens_`:

1. Create a **layered, topmost, transparent, tool-window** host HWND
   (`MagnifierHostWindow_v1`).
2. Create a child window of class `WC_MAGNIFIER` filling the host.
3. Each tick:
   - Position the host so it is centred on the **mouse cursor** (or the
     controller-owned target), clamped to the virtual desktop.
   - `MagSetWindowSource(magHwnd, sourceRect)` — sourceRect is the
     unmagnified pixels we want to draw, computed from
     `centre` and `lens.size / zoom`.
   - `MagSetWindowTransform(magHwnd, { zoom })` — the zoom matrix.
4. The host's `WndProc` returns `HTTRANSPARENT` from `WM_NCHITTEST` so
   clicks pass through.
5. **Critical**: `MagSetWindowFilterList(MW_FILTERMODE_EXCLUDE, [hostHwnd])`
   to prevent the magnifier from sampling its own output (feedback loop).

### 4.2 Fullscreen mode

Implemented in `MagController::ApplyFullscreen_`:

1. `MagSetFullscreenTransform(scale, xOffset, yOffset)` — applies a global
   zoom to the entire desktop.
2. `MagSetInputTransform(TRUE, &sourceRect, &destRect)` — this is the
   **non-obvious** call that makes mouse clicks land where you think. The
   OS routes input events from `destRect` (what the user clicks) back to
   `sourceRect` (what was actually drawn there pre-magnification).
3. We also call `SetThreadDpiAwarenessContext(PERMONITORV2)` so the
   coordinates are physical pixels on multi-DPI rigs.

### 4.3 Capture exclusion (W10 2004+)

`SetWindowDisplayAffinity(hostHwnd, WDA_EXCLUDEFROMCAPTURE)` — when present,
OBS sees the underlying screen rather than the lens overlay. The feature is
**dynamically loaded** (`SupportsCaptureExclusion()` checks `RtlGetVersion`
for build ≥ 19041) so the binary stays compatible with 1809+.

---

## 5. State easing

`StateModel::EaseStep` is the only piece of math in the project that
deserves comment:

```cpp
static float EaseStep(float current, float target, float dt, float tau) {
    if (tau <= 0.0f || dt < 0.0f) return target;       // snap
    const float alpha = 1.0f - std::exp(-dt / tau);
    return current + (target - current) * alpha;
}
```

This is the closed-form **exponential decay** with time constant `tau`. It
is:

- **Frame-rate independent** — running at 30 Hz or 240 Hz arrives at the
  same value after the same wall time. (Naive `current = lerp(current,
  target, k)` does *not* have this property.)
- **Composable** — two steps of `dt/2` give exactly the same result as one
  step of `dt`, modulo float precision (tested in
  `tests/test_state_model.cpp`).
- **O(1)** — no integration, no history.

The same function eases position (`tau ≈ 0.05 s`) and zoom
(`tau ≈ 0.10 s`). Set `tau = 0` for instant snap (used by hotkey recenter
and unit tests).

---

## 6. Input subsystems

### 6.1 Hotkeys (`HotkeyManager`)

- Primary path: `RegisterHotKey(hwnd, id, mods | MOD_NOREPEAT, vk)`.
  Works for keys the foreground app does not eat.
- Optional path: `WH_KEYBOARD_LL` low-level hook. Off by default — anti-
  cheat in some games flags any LL hook, and we don't want to break the
  user's other apps. Opt-in via `[advanced] low_level_keyboard_hook =
  true`; an entry is logged so the user knows it's active.
- Conflicts (another app already owns a chord) are returned to the caller
  as `vector<Conflict>` so the settings UI can show them.

### 6.2 Controller (`ControllerPoll`)

- XInput-only for v1 (covers Xbox controllers natively; DS4/DualSense via
  Steam Input or DS4Windows expose as XInput). The class is sealed enough
  to add a WinRT `Windows.Gaming.Input.RawGameController` backend later
  without changing the public interface.
- Polled at 125 Hz on its own thread. Each iteration:
  1. `XInputGetState(0..3)` — uses first connected controller.
  2. Normalise sticks `SHORT → [-1, 1]`, apply deadzone & power curve.
  3. Build a `ControllerFrame` and `PostMessage(WM_APP_FRAME)`.
  4. Edge-detect button presses; for buttons mapped to actions (`toggle_lens`
     etc), `PostMessage(WM_APP_ACTION)` after queueing the `Action`.
- "Snap-back to cursor" is implemented in `InputRouter`: once the right
  stick is centred and `idle_recenter_seconds` have elapsed, the lens
  re-acquires the mouse.

### 6.3 InputRouter

The translator. Owns no state of its own beyond the idle timer; receives
`Action` enums and `ControllerFrame` ticks, mutates the `StateModel`,
calls back into `App` for one-shot side effects (ShowSettings, Quit…).

---

## 7. IPC

Three channels, **same `Sink` contract**, no overlap:

### 7.1 WM_COPYDATA — single-instance forwarding

- Hidden message-only window class `MagnifierIpcWnd_v1`,
  title `MagnifierIpcSink_v1`, message magic = `0x4D41474D` (`'MAGM'`).
- `IpcServer::SendToRunningInstance(cmd, timeout_ms)` uses
  `SendMessageTimeoutW(..., SMTO_ABORTIFHUNG, ...)` — bounded blocking,
  cannot wedge if the target is hung.
- Used by `main.cpp` when a second invocation detects a running instance.

### 7.2 Named pipe `\\.\pipe\MagnifierCtl`

- Created with `PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_WAIT`.
- **Owner-only ACL** via SDDL string `D:(A;;GA;;;OW)`. Builds a
  `SECURITY_ATTRIBUTES` with `ConvertStringSecurityDescriptorToSecurityDescriptorW`.
- Protocol: newline-delimited JSON. `JsonLineReader` accumulates partial
  reads, strips `\r`, drops over-length lines (default 64 KB) without
  crashing. Tested in `tests/test_json_framing.cpp`.

### 7.3 HTTP loopback (optional)

- `http.sys` via `HttpInitialize` / `HttpCreateRequestQueue` / etc.
- Binds **only** to `http://127.0.0.1:<port>/`, never `0.0.0.0`.
- Disabled by default (`ipc.http_port = 0`). Enable per-user.
- Same JSON sink as the pipe; replies with the response body.

---

## 8. Configuration

- File: `%APPDATA%\Magnifier\config.toml`, TOML 1.0 via `tomlplusplus`.
- **Atomic write**: `<config>.tmp.<pid>` then `ReplaceFileW(..., REPLACEFILE_IGNORE_MERGE_ERRORS)`.
  Power failure during save can never produce a torn file.
- **Schema versioning** — `schema_version = 1` at the top. Future bumps
  trigger a backup of the old file before migration.
- Default config is **embedded as a constexpr string** in
  `ConfigStore.cpp` so the binary is self-contained even when the file is
  deleted.
- Hotkey vocabulary: `ctrl`, `alt`, `shift`, `win` modifiers + a 100-entry
  key table covering a-z, 0-9, F1-F24, named keys, OEM punctuation.

---

## 9. Reliability

| Concern | Mitigation |
|---|---|
| Unhandled exception | `SetUnhandledExceptionFilter` writes a minidump (`MiniDumpWithThreadInfo \| WithUnloadedModules \| WithDataSegs`) to `%LOCALAPPDATA%\Magnifier\crashes\crash-YYYYMMDD-HHMMSS.dmp`. `SEM_NOGPFAULTERRORBOX` suppresses the Windows error dialog. |
| Log flooding | Rotating file sink, 5 MB × 3, `flush_every(5s)`, async logger. |
| Hung second instance | `SendMessageTimeoutW(SMTO_ABORTIFHUNG, 1500 ms)`. |
| Display reconfigured | `WM_DISPLAYCHANGE` & `WM_DPICHANGED` re-seed `ScreenBounds`. |
| Sleep / wake | `WM_POWERBROADCAST` pauses the controller poll on suspend, resumes on wake. |
| Magnifier feedback loop | `MagSetWindowFilterList(EXCLUDE, [hostHwnd])`. |
| Worker thread death | Sinks are `std::function<>`; a throw inside is logged and the worker continues to the next iteration. |
| Resource pin | `Magnifier` runs at default priority. Optional `general.active_priority = "above_normal"` raises priority while magnification is active and drops it back to normal on Off. |

---

## 10. CLI

See [`docs/CLI.md`](CLI.md) for the full grammar. Quick summary:

- **`Normal` mode** — argv contains commands that are applied at startup
  in *our* process (e.g. `--lens`). We then run the message loop.
- **`ForwardOnly` mode** — argv contains commands that must reach the
  running instance and the launcher should exit immediately (`--off`,
  `--quit`, `--status`, …).
- **Print-only modes** — `--help`, `--version`.
- **`ParseError`** — unknown flag or bad value; exit code 2.

`main.cpp` first checks `IpcServer::FindRunningInstance()`. If one is
present and we're in `ForwardOnly` (or have any startup command), forward
and exit. Otherwise we acquire `Local\MagnifierApp_SingleInstance_v1`
mutex; if it's already held by another process we forward & exit.

---

## 11. Build & packaging

- **CMake 3.20+, Ninja, MSVC ≥ 14.29** (VS 2019/2022 BuildTools).
- **FetchContent** pulls spdlog v1.14.1, tomlplusplus v3.4.0,
  nlohmann/json v3.11.3, Dear ImGui v1.90.9, GoogleTest v1.14.0.
  No vcpkg or system package manager required.
- Flags: `/W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /MP`
  + `/O2 /GL /Gy /LTCG /OPT:REF,ICF` in Release.
- Manifest declares: `asInvoker`, `PerMonitorV2` DPI, `GdiScaling`, UTF-8
  code page, longPathAware, Common Controls v6, Windows 10/11 supportedOS.
- Two artefacts:
  - **Portable ZIP** via CPack ZIP generator.
  - **MSI** via WiX 4 (`packaging/wix/Magnifier.wxs`) — Start Menu
    shortcut, optional `HKCU\…\Run` "Start at login" entry.
- CI: `.github/workflows/ci.yml` builds + tests on every push, packages
  ZIP+MSI on every commit, attaches them to a GitHub Release on a `v*`
  tag.

---

## 12. Known limitations

1. **True exclusive-fullscreen games** cannot be magnified. The
   Magnification API draws on top of the desktop compositor; an
   exclusive-fullscreen app owns the swap chain directly. Workaround: use
   borderless / windowed mode (the modern default in almost every game).
2. **Anti-cheat & LL hook**: enabling the low-level keyboard hook can
   trigger heuristics in kernel anti-cheat (EAC, BattlEye, Vanguard).
   The hook is off by default; we never inject DLLs.
3. **HDR** displays show post-tone-map content under the lens — we sample
   the same backbuffer the desktop sees.
4. **DualSense / DualShock 4** must be routed through Steam Input or
   DS4Windows to appear as XInput. A native `RawGameController` backend
   is on the roadmap (single-file change in `ControllerPoll.cpp`).
5. **Secure desktop** (UAC prompts, Ctrl+Alt+Del) — Windows hides the
   regular desktop and we cannot draw on the secure desktop without
   UIAccess elevation, which we deliberately do not request.

---

## 13. Roadmap

- WinRT `RawGameController` backend for generic / DualSense pads.
- Per-monitor profiles (settings keyed off the active output).
- Optional inverted / colour-blind LUT (was deferred as a v2 feature).
- ImGui in-overlay HUD ("zoom: 3.0x") fading out after a second.
- Voice-activated commands via `Windows.Media.SpeechRecognition`
  (push-to-talk only).
