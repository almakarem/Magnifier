#pragma once

#include <optional>
#include <string>
#include <variant>

namespace magnifier {

// Parsed/typed IPC + CLI commands. Anything received over WM_COPYDATA, the
// named pipe, or HTTP is converted into one of these and dispatched onto the
// UI thread.

enum class CmdKind {
    Noop,

    EnterLens,
    EnterFullscreen,
    TurnOff,
    Toggle,                // toggles whatever was last active

    SetZoom,               // float zoom
    ZoomDelta,             // float delta (positive = in)
    SetLensSize,           // int width, int height
    SetMonitorIndex,       // int monitor index (-1 = follow mouse)

    ReloadConfig,
    SwitchProfile,         // string profile name

    EnableController,
    DisableController,

    ShowSettings,
    HideSettings,

    GetStatus,             // returns JSON status response
    Quit,
    ForceQuit,
};

struct Command {
    CmdKind kind = CmdKind::Noop;
    // Optional payloads (set only when relevant for `kind`).
    std::optional<float>        f_value;
    std::optional<int>          i_value;
    std::optional<int>          i_value2;
    std::optional<std::string>  s_value;
};

// JSON helpers — defined in IpcServer.cpp (they pull in nlohmann/json).
std::string    SerializeCommand(const Command& c);
std::optional<Command> DeserializeCommand(std::string_view json);

} // namespace magnifier
