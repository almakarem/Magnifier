#include "input/InputRouter.h"
#include "app/App.h"
#include "mag/StateModel.h"
#include "util/Log.h"

#include <cmath>

namespace magnifier {

InputRouter::InputRouter(App& app, StateModel& state, const ControllerConfig& cfg)
    : app_(app), state_(state), cfg_(cfg) {}

void InputRouter::SetControllerConfig(const ControllerConfig& cfg) {
    cfg_ = cfg;
}

void InputRouter::OnAction(Action a) {
    spdlog::debug("InputRouter action: {}", std::string(ToString(a)));
    switch (a) {
        case Action::ToggleLens:        app_.ToggleMode(MagMode::Lens);                break;
        case Action::ToggleFullscreen:  app_.ToggleMode(MagMode::Fullscreen);          break;
        case Action::TurnOff:           app_.SetMode(MagMode::Off);                    break;
        case Action::ZoomIn:            state_.NudgeTargetZoom(+app_.DefaultZoomStep()); break;
        case Action::ZoomOut:           state_.NudgeTargetZoom(-app_.DefaultZoomStep()); break;
        case Action::ZoomReset:         state_.SetTargetZoom(app_.InitialZoom());      break;
        case Action::LensSizeUp:        app_.ResizeLens(+64, +36);                     break;
        case Action::LensSizeDown:      app_.ResizeLens(-64, -36);                     break;
        case Action::Recenter:          app_.RecenterOnCursor(); idle_ = 1e9f;         break;
        case Action::NextMonitor:       app_.JumpToNextMonitor();                      break;
        case Action::EnableController:  app_.SetControllerEnabled(true);               break;
        case Action::DisableController: app_.SetControllerEnabled(false);              break;
        case Action::ReloadConfig:      app_.ReloadConfig();                           break;
        case Action::ShowSettings:      app_.ShowSettings();                           break;
        case Action::Quit:              app_.RequestQuit(false);                       break;
        case Action::ForceQuit:         app_.RequestQuit(true);                        break;
        default: break;
    }
}

void InputRouter::OnControllerFrame(const ControllerFrame& f, float dt) {
    if (!f.present) return;

    const float mag_x = std::abs(f.ls_x);
    const float mag_y = std::abs(f.ls_y);
    const bool  any_stick = mag_x > 0.0f || mag_y > 0.0f || std::abs(f.rs_y) > 0.0f;

    if (any_stick) {
        idle_   = 0.0f;
        owning_ = true;
    } else {
        idle_ += dt;
        if (cfg_.idle_recenter_seconds > 0.0f &&
            idle_ > cfg_.idle_recenter_seconds) {
            owning_ = false;
        }
    }

    // Translate stick deflection into target-center movement.
    // ls_x / ls_y are already normalised to [-1, 1] with deadzone + curve.
    if (mag_x > 0.0f || mag_y > 0.0f) {
        const float dx = f.ls_x * cfg_.move_speed * dt;
        const float dy = f.ls_y * cfg_.move_speed * dt;
        state_.NudgeTargetCenter(dx, dy);
    }

    if (std::abs(f.rs_y) > 0.0f) {
        // Push up (negative y after our flip) = zoom in.
        const float dz = -f.rs_y * cfg_.zoom_speed * dt;
        state_.NudgeTargetZoom(dz);
    }
}

bool InputRouter::ControllerOwnsCursor() const noexcept {
    return owning_;
}

} // namespace magnifier
