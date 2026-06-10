#pragma once

#include "input/Actions.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace magnifier {

// ---------------------------------------------------------------------------
// In-memory representation of the on-disk config.toml. All fields are
// initialised to safe defaults so the app can run even when the file is
// missing or partial.
// ---------------------------------------------------------------------------

struct GeneralConfig {
    bool        start_minimized   = false;
    bool        restore_last_mode = true;
    std::string active_priority   = "above_normal"; // "normal" | "above_normal"
};

struct LensConfig {
    int    width            = 640;
    int    height           = 360;
    int    border_thickness = 2;
    std::string border_color_hex = "#00E5FF";   // #RRGGBB
    bool   follow_mouse     = true;
    // When false (default) the magnifier control does not draw its own
    // cursor — the OS hardware cursor shows through the lens at the full
    // display refresh rate with no lag. Set true to draw a zoomed cursor
    // inside the magnified content (matches the system Magnifier app but
    // produces visible cursor ghosting because the mag control samples
    // cursor position at its own internal cadence, not at the hardware
    // mouse rate).
    bool   magnify_cursor   = false;
    float  position_tau     = 0.05f;
    float  zoom_tau         = 0.10f;
};

struct ZoomConfig {
    float initial      = 2.0f;
    float min          = 1.0f;
    float max          = 16.0f;
    float default_step = 0.25f;
};

enum class CaptureMode { Auto, ExcludeSelf, IncludeSelf };

struct CaptureConfig {
    CaptureMode mode = CaptureMode::Auto;
};

struct IpcConfig {
    std::string pipe_name = "MagnifierCtl";
    int         http_port = 0;   // 0 = disabled
};

struct AdvancedConfig {
    bool        low_level_keyboard_hook = false;
    std::string log_level = "info";
};

// ---------------------------------------------------------------------------
// Auto-update settings. The updater queries the GitHub Releases API for
// <owner>/<repo>; for *private* repos a fine-grained PAT with
// "Contents: Read" on the target repo must be set in `token`.
// ---------------------------------------------------------------------------
struct UpdateConfig {
    bool        check_on_startup = true;
    bool        auto_download    = false;     // download MSI as soon as found
    std::string owner;                        // GitHub user or org
    std::string repo            = "Magnifier";
    std::string token;                        // PAT (empty for public repos)
};

// Modifier flags compatible with RegisterHotKey's fsModifiers parameter.
struct HotkeyBinding {
    unsigned modifiers = 0;   // MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN
    unsigned vk        = 0;   // virtual-key code, 0 = unbound

    bool is_bound() const noexcept { return vk != 0; }
    std::string to_human() const;

    friend bool operator==(const HotkeyBinding&, const HotkeyBinding&) = default;
};

struct ControllerBindings {
    // The strings reference the engine vocabulary defined in
    // ControllerPoll: axes "ls_x" "ls_y" "rs_x" "rs_y" "lt" "rt",
    // buttons "a" "b" "x" "y" "lb" "rb" "back" "start" "ls" "rs"
    // "dpad_up" "dpad_down" "dpad_left" "dpad_right".
    std::string move_x            = "ls_x";
    std::string move_y            = "ls_y";
    std::string zoom_axis         = "rs_y";
    std::string toggle_lens       = "x";
    std::string toggle_fullscreen = "y";
    std::string turn_off          = "back";
    std::string recenter          = "ls";
};

struct ControllerConfig {
    bool   enabled              = true;
    float  deadzone             = 0.15f;
    float  curve                = 2.0f;
    float  move_speed           = 800.0f;
    float  zoom_speed           = 2.0f;
    float  idle_recenter_seconds = 1.5f;
    ControllerBindings bindings{};
};

struct Config {
    int             schema_version = 1;
    GeneralConfig   general{};
    LensConfig      lens{};
    ZoomConfig      zoom{};
    CaptureConfig   capture{};
    IpcConfig       ipc{};
    AdvancedConfig  advanced{};
    UpdateConfig    update{};
    ControllerConfig controller{};
    std::map<Action, HotkeyBinding> hotkeys;
};

// ---------------------------------------------------------------------------
// Disk I/O
// ---------------------------------------------------------------------------

struct LoadResult {
    Config              config;
    bool                loaded_from_file = false;
    std::vector<std::string> warnings;
};

// Parse a hotkey spec like "ctrl+alt+z" or "shift+f5".
// Returns nullopt on malformed input (e.g. unknown key name).
std::optional<HotkeyBinding> ParseHotkey(std::string_view spec);

// Load %APPDATA%\Magnifier\config.toml, creating it from the embedded
// default if missing. Never throws — errors are written to `warnings`
// and defaults are substituted for the offending fields.
LoadResult LoadConfig(const std::filesystem::path& path);

// Atomically write the config to disk (temp file + ReplaceFileW).
// Returns true on success.
bool SaveConfig(const std::filesystem::path& path, const Config& cfg);

} // namespace magnifier
