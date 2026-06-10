# Magnifier hotkey reference

All hotkeys are **global** (work even when another app is focused) and
**rebindable** in the settings UI or by editing
`%APPDATA%\Magnifier\config.toml`.

## Defaults

| Action | Default hotkey | Action name (TOML key) |
|---|---|---|
| Toggle lens mode      | `Ctrl + Alt + Z` | `toggle_lens` |
| Toggle fullscreen     | `Ctrl + Alt + X` | `toggle_fullscreen` |
| Turn magnification off| `Ctrl + Alt + Q` | `turn_off` |
| Zoom in               | `Ctrl + Alt + =` | `zoom_in` |
| Zoom out              | `Ctrl + Alt + -` | `zoom_out` |
| Reset zoom            | `Ctrl + Alt + 0` | `zoom_reset` |
| Lens size up          | `Ctrl + Alt + ]` | `lens_size_up` |
| Lens size down        | `Ctrl + Alt + [` | `lens_size_down` |
| Recenter on cursor    | `Ctrl + Alt + C` | `recenter` |
| Next monitor          | `Ctrl + Alt + M` | `next_monitor` |
| Enable controller     | (unbound)        | `enable_controller` |
| Disable controller    | (unbound)        | `disable_controller` |
| Reload config         | `Ctrl + Alt + R` | `reload_config` |
| Show settings         | `Ctrl + Alt + ,` | `show_settings` |
| Quit Magnifier        | (unbound)        | `quit` |

## Rebinding

### Via the settings window

`Right-click the tray icon → Settings → Hotkeys`, click a row, press the
desired chord, press *Apply*. Conflicts (another app already owns the
chord) are listed inline.

### Via `config.toml`

```toml
[hotkeys]
toggle_lens       = "ctrl+alt+z"
toggle_fullscreen = "ctrl+alt+x"
turn_off          = "ctrl+alt+q"
zoom_in           = "ctrl+alt+="
zoom_out          = "ctrl+alt+-"
zoom_reset        = "ctrl+alt+0"
lens_size_up      = "ctrl+alt+]"
lens_size_down    = "ctrl+alt+["
recenter          = "ctrl+alt+c"
next_monitor      = "ctrl+alt+m"
reload_config     = "ctrl+alt+r"
show_settings     = "ctrl+alt+,"
```

Then either `Magnifier.exe --reload-config` or pick *Reload config* from
the tray menu.

## Hotkey grammar

```
<spec>      ::=  <modifier>+ <key>
<modifier>  ::=  "ctrl" | "alt" | "shift" | "win"
<key>       ::=  letter | digit | named-key
```

Letters and digits are case-insensitive (`Z`, `z`, `0`).

### Named keys

```
Function:   f1 … f24
Navigation: left right up down home end pageup pagedown
Editing:    insert delete backspace tab return enter escape space
Numpad:     numpad0 … numpad9 numpadadd numpadsub numpadmul numpaddiv
            numpaddot numpadenter
Symbols:    + - = [ ] \ ; ' , . /
            (use the symbol literally, except '+' which separates tokens —
             write 'plus' to use the plus key itself)
```

## Optional low-level keyboard hook

By default Magnifier uses `RegisterHotKey`, which fails silently if another
process has already grabbed the same chord (some games eat keys before
RegisterHotKey sees them). For those cases you can opt-in to a low-level
keyboard hook:

```toml
[advanced]
low_level_keyboard_hook = true
```

**Caveats**

- **Anti-cheat**: some kernel-mode anti-cheat (EAC, BattlEye, Vanguard)
  flag any process that installs a low-level hook. The hook is **off by
  default** for this reason.
- A log line `Low-level keyboard hook ENABLED` is emitted whenever the
  hook is active so the user is aware.
- Hooked chords are *consumed* — the underlying app does not see them.

## Programmatic alternative

Anything a hotkey can do is also available via the
[CLI](CLI.md) — handy for Stream Deck / OBS / AutoHotKey integrations
where you'd rather bind your own hotkey software-side.
