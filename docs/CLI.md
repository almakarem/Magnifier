# Magnifier CLI reference

Every option works both at startup (first launch) and as a remote command
(second launch — the arg is forwarded to the running instance over
WM_COPYDATA, then the launcher exits).

## Synopsis

```
Magnifier.exe [options]
```

## Mode commands

| Flag | Effect |
|---|---|
| `--lens`       | Enter lens mode (windowed magnifier follows the cursor) |
| `--fullscreen` | Enter full-screen zoom mode |
| `--off`        | Disable magnification (instance keeps running, tray icon stays) |
| `--toggle`     | Toggle between the last-used active mode and Off |

## Adjustments

| Flag | Argument | Effect |
|---|---|---|
| `--zoom`        | `<factor>` (1.0 – 16.0) | Set absolute zoom level |
| `--zoom-in`     | `[step]` (default from config, typically 0.25) | Zoom in by the step |
| `--zoom-out`    | `[step]` | Zoom out by the step |
| `--lens-size`   | `<WxH>` e.g. `800x450` | Resize the lens (lens mode only) |
| `--monitor`     | `<index>` | Pin fullscreen mode to a specific monitor |

## Lifecycle / control

| Flag | Effect | Forwarded? |
|---|---|---|
| `--reload-config`      | Re-read `config.toml` and apply | Yes |
| `--profile <name>`     | Switch to a named profile section | Yes |
| `--enable-controller`  | Turn controller input on | Yes |
| `--disable-controller` | Turn controller input off | Yes |
| `--status`             | Print the running instance's state as JSON, then exit | Yes (named pipe) |
| `--quit`               | Ask the running instance to shut down | Yes |
| `--force-quit`         | Terminate the running instance immediately | Yes |

## Startup

| Flag | Effect |
|---|---|
| `--start-minimized` | Don't focus / flash on launch — tray icon only |
| `--help, -h`        | Print this reference and exit |
| `--version, -v`     | Print the version string and exit |

## Examples

### Stream Deck — toggle lens

```
Magnifier.exe --toggle
```

### OBS hotkey — zoom in temporarily then turn off

```
Magnifier.exe --lens --zoom 3.0
... (some time later)
Magnifier.exe --off
```

### Scripted status query

```powershell
$json = Magnifier.exe --status | ConvertFrom-Json
if ($json.mode -eq 'lens') { ... }
```

### Force-kill a hung instance (rare)

```
Magnifier.exe --quit          # try graceful first
Start-Sleep 2
Magnifier.exe --force-quit    # nuclear option
```

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Success (or forwarded to running instance) |
| 2 | CLI parse error (unknown flag, bad value) |
| 3 | `--status` requested but no instance is running |
| 4 | Command forwarded but no acknowledgement received |
| 5 | App initialisation failed (see log file) |

## See also

- [HOTKEYS.md](HOTKEYS.md) — global hotkey reference and rebind syntax.
- [DESIGN.md](DESIGN.md#10-cli) — how the CLI integrates with the IPC layer.
