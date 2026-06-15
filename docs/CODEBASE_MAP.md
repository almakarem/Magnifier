# Magnifier — Codebase Map

A navigation guide to the source tree. Pairs with [`DESIGN.md`](DESIGN.md)
(the *why*); this doc is the *where*. Generated from a full read of the
~6.9 KLOC C++20 codebase (39 source files under `src/`).

---

## 1. One-paragraph summary

Magnifier is a **single-binary, Windows-only, C++20** screen magnifier. The
`.exe` is both the GUI app **and** its own CLI: re-invoking it with an IPC
flag (`--toggle`, `--zoom 4`…) forwards the command to the already-running
instance over a named pipe / `WM_COPYDATA` and exits. Everything that touches
OS state runs on a **single UI thread**; worker threads (controller poll,
pipe accept, optional HTTP) marshal back via `PostMessage`. The single mutable
cross-thread object is `StateModel`. Rendering is the **Win32 Magnification
API** (lens = layered `WC_MAGNIFIER` child window; full-screen =
`MagSetFullscreenTransform`). Settings UI is **Dear ImGui** on D3D11. Deps are
pulled by CMake FetchContent — no vcpkg/conan.

---

## 2. Directory layout

```
Magnifier/
├─ src/                  ← all application code (the map below)
│  ├─ main.cpp           ← wWinMain: CLI parse, single-instance gate, launch
│  ├─ app/               ← App: the orchestrator + Win32 message loop
│  ├─ mag/               ← magnification core (StateModel + MagController)
│  ├─ input/             ← hotkeys, controller backends, input routing
│  ├─ ipc/               ← CLI parsing + 3 IPC channels + JSON framing
│  ├─ config/            ← TOML load/save, schema, hotkey parsing
│  ├─ ui/                ← tray icon + ImGui settings window
│  ├─ update/            ← GitHub-Releases self-updater
│  └─ util/              ← logging, crash dumps, paths, string/version
├─ app/                  ← Win32 resources (.rc, .ico, manifest, resource.h)
├─ tests/                ← GoogleTest unit tests (5 suites, pure logic)
├─ docs/                 ← DESIGN, CLI, HOTKEYS, QA, this map
├─ packaging/            ← WiX MSI (.wxs) + portable default config.toml
├─ scripts/             ← build.cmd, release.cmd, make-icon.ps1
├─ .github/workflows/    ← CI: build+test, package ZIP+MSI, release on v* tag
└─ CMakeLists.txt        ← FetchContent deps, magnifier_core lib + exe + tests
```

**Build shape:** `CMakeLists.txt` compiles nearly everything into a static lib
`magnifier_core` (so it is unit-testable), then links a thin `Magnifier.exe`
(`main.cpp` + resources) and `magnifier_tests.exe` against it.

---

## 3. The dependency spine (who calls whom)

```
main.cpp
   │ constructs
   ▼
App  ──────────────── owns every subsystem, runs the message loop ───────────┐
   │ routes Actions/frames           │ sends Commands          │ renders      │
   ▼                                 ▼                          ▼              ▼
InputRouter ──mutates──► StateModel ──snapshot──► MagController        SettingsWindow
   ▲          ▲                                   (Win32 Mag API)      TrayIcon
   │ Actions  │ frames
HotkeyManager │
ControllerPoll┘ (XInput + WgiGamepad backends, own thread)
IpcServer ───Commands──► App::OnIpcCommand ──► cmd_queue_ ──► UI thread drains
Updater (GitHub Releases, own threads)        ConfigStore (TOML ⇄ Config)
```

**Single dispatch surface:** every input source — hotkey, controller button,
IPC command, tray menu — collapses onto either the `Action` enum
(`input/Actions.h`) or the `Command` struct (`ipc/Commands.h`). `InputRouter`
applies `Action`s; `App::OnIpcCommand` applies `Command`s.

---

## 4. Module-by-module reference

Sizes are `.cpp` line counts; ★ marks the files to read first.

### `app/` — orchestration
| File | Lines | Role |
|---|---|---|
| ★ `App.cpp` / `App.h` | 879 | The hub. Owns all subsystems (see `App.h:92-149`). Runs the Win32 loop, drives the high-res waitable timer pinned to the monitor refresh (`RefreshTickRate_`), drains the cross-thread `cmd_queue_`/`action_queue_`/`latest_frame_`, applies config, handles `WM_DISPLAYCHANGE`/`WM_DPICHANGED`/`WM_POWERBROADCAST`, owns update orchestration + PID file + tray tooltip. **Start here.** |

### `mag/` — the magnifier core
| File | Lines | Role |
|---|---|---|
| ★ `StateModel.cpp` / `.h` | 121 | Single source of truth: current+target `{zoom, center, lens size, mode, bounds}`, mutex-guarded. Frame-rate-independent exponential easing (`EaseStep`, see DESIGN §5). Inputs set targets; `Tick(dt)` eases current→target. The only mutable cross-thread object. |
| `MagController.cpp` / `.h` | 454 | Wraps the Win32 Magnification API. `ApplyLens_` (layered topmost `WC_MAGNIFIER` host child + filter-list anti-feedback + `WDA_EXCLUDEFROMCAPTURE`), `ApplyFullscreen_` (`MagSetFullscreenTransform` + `MagSetInputTransform`). UI-thread only. Consumes a `StateModel::Snapshot`. |

### `input/` — getting user intent in
| File | Lines | Role |
|---|---|---|
| `Actions.h` | 112 | The `Action` enum + `ToString`/`FromString`. The canonical action vocabulary every input source maps to. |
| `HotkeyManager.cpp` / `.h` | 205 | `RegisterHotKey` primary path; optional `WH_KEYBOARD_LL` low-level hook (opt-in, AV-sensitive). Reports binding conflicts as `vector<Conflict>` for the UI. |
| `ControllerPoll.cpp` / `.h` | 245 | Dedicated ~125 Hz poll thread. Merges **XInput** + **WGI** backends into one `ControllerFrame` (normalised axes, deadzone, response curve) + edge-detected button `Action`s. Emits via `FrameSink`/`ActionSink`. |
| `WgiGamepad.cpp` / `.h` | 307 | `Windows.Gaming.Input` (WinRT) backend — Bluetooth Xbox pads, DualSense (USB), `RawGameController` fallback. Lives on the poll thread (its own COM apartment). |
| `InputRouter.cpp` / `.h` | 99 | Translator. Turns `Action`s + per-tick `ControllerFrame`s into `StateModel` mutations; calls back into `App` for one-shot side effects (ShowSettings, Quit). Owns only the idle-recenter timer. |

### `ipc/` — CLI + control channels
| File | Lines | Role |
|---|---|---|
| `Commands.h` | 53 | `CmdKind` enum + `Command` struct (typed payloads) — the IPC vocabulary. |
| `CliParser.cpp` / `.h` | 230 | argv → `CliResult` (`Normal` / `ForwardOnly` / `PrintHelp` / `PrintVersion` / `ParseError`) + optional startup `Command`. Owns `HelpText()`. |
| `IpcServer.cpp` / `.h` | 422 | Three channels, one `Sink` contract: **WM_COPYDATA** (single-instance forwarding), **named pipe** `\\.\pipe\MagnifierCtl` (owner-ACL'd, newline-JSON), optional **HTTP loopback** (`http.sys`, 127.0.0.1 only, off by default). Also `Serialize/DeserializeCommand` (nlohmann/json). |
| `JsonFraming.cpp` / `.h` | 30 | `JsonLineReader` — accumulates partial pipe reads, strips `\r`, drops over-length lines safely. |

### `config/` — persistence
| File | Lines | Role |
|---|---|---|
| ★ `ConfigStore.cpp` / `.h` | 564 | The full settings schema (`Config` and its sub-structs: General/Lens/Zoom/Capture/Ipc/Advanced/Update/Controller + hotkey map). `LoadConfig` (never throws; warnings + defaults), `SaveConfig` (atomic temp + `ReplaceFileW`), `ParseHotkey`. Embedded constexpr default TOML so the binary self-heals a deleted config. |

### `ui/` — surfaces
| File | Lines | Role |
|---|---|---|
| `TrayIcon.cpp` / `.h` | 178 | Shell_NotifyIcon tray: dynamic tooltip, right-click action menu, one-shot welcome balloon. |
| `SettingsWindow.cpp` / `.h` | 788 | Dear ImGui (D3D11) settings window. Tabs: General, Lens, Zoom, Controller, Capture/OBS, Advanced, Updates, Hotkeys (capture + conflict warnings), Diagnostics, About. Largest UI file. |

### `update/` — self-updater
| File | Lines | Role |
|---|---|---|
| `Updater.cpp` / `.h` | 548 | Anonymous `GET api.github.com/.../releases/latest` (WinHTTP), version compare, MSI download with progress, relaunch `msiexec /i /passive`. The app's only outbound network call. |

### `util/` — leaf utilities (no app deps)
| File | Lines | Role |
|---|---|---|
| `Log.cpp` / `.h` | 86 | spdlog async rotating file sink (`%LOCALAPPDATA%\Magnifier\logs`, 5 MB × 3). |
| `Crash.cpp` / `.h` | 82 | `SetUnhandledExceptionFilter` → minidump to `…\crashes\`. |
| `Paths.cpp` / `.h` | 65 | `%LOCALAPPDATA%\Magnifier\` path resolution. |
| `StringConv.cpp` / `.h` | 29 | UTF-8 ⇄ UTF-16 (`Utf8ToWide`/`WideToUtf8`). |
| `WinError.h` | 30 | `GetLastError` → readable string. |
| `Version.h.in` | — | CMake-templated version constants (`kVersionString`, `kProjectName`). |

---

## 5. Threading model (invariants you must not break)

| Thread | Owns | Notes |
|---|---|---|
| **UI** (`wWinMain`) | all HWNDs, MagController, hotkeys, timer ticks, config | the only writer to the magnifier control |
| **Controller poll** | one `std::thread` (~125 Hz) | writes `latest_frame_`/`action_queue_`, then `PostMessage`-wakes UI |
| **Pipe accept** | one `std::thread` | pushes `Command` to `cmd_queue_`; never calls `App` directly |
| **HTTP serve** | optional `std::thread` (`http_port>0`) | same sink contract |
| **spdlog async** | spdlog worker | drains log queue |

Rules: MagController is UI-thread-only; `StateModel` is the sole mutable
cross-thread object (single mutex); workers communicate **only** by queue +
`PostMessage` (`WM_APP_CMD`/`WM_APP_ACTION`/`WM_APP_FRAME`), drained on UI.

---

## 6. Key runtime flows

**Cold launch:** `main.cpp` → DPI context → `ParseCli` → (info short-circuits)
→ single-instance mutex → `App::Initialise` (load config, wire subsystems,
register hotkeys, start controller poll + IPC, arm timer) → `App::Run`.

**Second invocation (`Magnifier.exe --toggle`):** `ParseCli` returns
`ForwardOnly` (or mutex already held) → `IpcServer::SendToRunningInstance`
(WM_COPYDATA, bounded `SMTO_ABORTIFHUNG`) → process exits. `--status` instead
opens the pipe for a JSON reply.

**Per tick (UI thread):** waitable timer fires → drain queues → apply latest
controller frame → `StateModel::Tick(dt)` eases current→target →
`MagController` applies the snapshot → `DwmFlush()` pins to vsync.

**Settings change:** ImGui edits `Config` → `SaveConfig` (atomic) →
`App::ApplyConfig_` re-pushes to StateModel / hotkeys / controller live.

---

## 7. Tests (`tests/`, GoogleTest)

Pure-logic suites on the testable core (no Win32 UI):
`test_cli_parser`, `test_config_store`, `test_hotkey_parse`,
`test_json_framing`, `test_state_model` (notably verifies easing is
frame-rate-independent and composable). Run:
`build\tests\magnifier_tests.exe --gtest_brief=1`.

---

## 8. Where to make common changes

| You want to… | Touch |
|---|---|
| Add a user action | `input/Actions.h` (enum + strings) → bind in `InputRouter` / `ControllerPoll` / hotkey map |
| Add a CLI/IPC command | `ipc/Commands.h` (`CmdKind`) → `CliParser` → `IpcServer` (de/serialize) → `App::OnIpcCommand` |
| Add a config field | `config/ConfigStore.h` struct → load/save in `ConfigStore.cpp` → embedded default TOML → expose in `SettingsWindow.cpp` |
| Change magnify behaviour | `mag/MagController.cpp` (Win32 calls) and/or `mag/StateModel.cpp` (easing/clamping) |
| Add a controller backend | new file in `input/`, merge in `ControllerPoll::PollLoop_` (mirror `WgiGamepad`) |
| Add a settings tab | `ui/SettingsWindow.cpp` |

---

## 9. External dependencies (FetchContent, pinned in CMakeLists.txt)

spdlog 1.14.1 · tomlplusplus 3.4.0 · nlohmann/json 3.11.3 ·
Dear ImGui 1.90.9 (D3D11+Win32 backends) · GoogleTest 1.14.0.
System libs: Magnification, XInput, Dbghelp, Shell32, Comctl32, WinHTTP,
Httpapi, Runtimeobject/Windowsapp (WinRT), d3d11/dxgi.
