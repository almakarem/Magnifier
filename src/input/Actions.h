#pragma once

#include <cstdint>
#include <string_view>

namespace magnifier {

// Discrete user-triggerable actions. Hotkeys, controller buttons, IPC
// commands and tray menu items all map onto this enum so the application
// has a single dispatch surface.
enum class Action : std::uint16_t {
    None = 0,

    ToggleLens,
    ToggleFullscreen,
    TurnOff,

    ZoomIn,
    ZoomOut,
    ZoomReset,
    SetZoom,                // requires float arg

    LensSizeUp,
    LensSizeDown,
    SetLensSize,            // requires (int,int) arg

    Recenter,               // re-center on the mouse cursor
    NextMonitor,            // for full-screen mode

    EnableController,
    DisableController,

    ReloadConfig,
    SwitchProfile,          // requires string arg

    ShowSettings,
    Quit,
    ForceQuit,

    // Continuous (per-tick) movement / zoom from the controller. The router
    // does NOT enqueue these — it writes the latest values directly into the
    // StateModel each tick.
    _Count
};

constexpr std::string_view ToString(Action a) {
    switch (a) {
        case Action::None:             return "none";
        case Action::ToggleLens:       return "toggle_lens";
        case Action::ToggleFullscreen: return "toggle_fullscreen";
        case Action::TurnOff:          return "turn_off";
        case Action::ZoomIn:           return "zoom_in";
        case Action::ZoomOut:          return "zoom_out";
        case Action::ZoomReset:        return "zoom_reset";
        case Action::SetZoom:          return "set_zoom";
        case Action::LensSizeUp:       return "lens_size_up";
        case Action::LensSizeDown:     return "lens_size_down";
        case Action::SetLensSize:      return "set_lens_size";
        case Action::Recenter:         return "recenter";
        case Action::NextMonitor:      return "next_monitor";
        case Action::EnableController: return "enable_controller";
        case Action::DisableController:return "disable_controller";
        case Action::ReloadConfig:     return "reload_config";
        case Action::SwitchProfile:    return "switch_profile";
        case Action::ShowSettings:     return "show_settings";
        case Action::Quit:             return "quit";
        case Action::ForceQuit:        return "force_quit";
        default:                       return "unknown";
    }
}

// Inverse lookup; returns Action::None on failure.
inline Action ActionFromString(std::string_view name) {
    if (name == "toggle_lens")        return Action::ToggleLens;
    if (name == "toggle_fullscreen")  return Action::ToggleFullscreen;
    if (name == "turn_off")           return Action::TurnOff;
    if (name == "zoom_in")            return Action::ZoomIn;
    if (name == "zoom_out")           return Action::ZoomOut;
    if (name == "zoom_reset")         return Action::ZoomReset;
    if (name == "set_zoom")           return Action::SetZoom;
    if (name == "lens_size_up")       return Action::LensSizeUp;
    if (name == "lens_size_down")     return Action::LensSizeDown;
    if (name == "set_lens_size")      return Action::SetLensSize;
    if (name == "recenter")           return Action::Recenter;
    if (name == "next_monitor")       return Action::NextMonitor;
    if (name == "enable_controller")  return Action::EnableController;
    if (name == "disable_controller") return Action::DisableController;
    if (name == "reload_config")      return Action::ReloadConfig;
    if (name == "switch_profile")     return Action::SwitchProfile;
    if (name == "show_settings")      return Action::ShowSettings;
    if (name == "quit")               return Action::Quit;
    if (name == "force_quit")         return Action::ForceQuit;
    return Action::None;
}

} // namespace magnifier
