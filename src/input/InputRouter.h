#pragma once

#include "input/Actions.h"
#include "input/ControllerPoll.h"

#include <Windows.h>

namespace magnifier {

class App;       // forward
class StateModel;

// Translates Actions and per-tick analog input into mutations on the
// StateModel and one-shot calls into App. All methods must be invoked on
// the UI thread.
class InputRouter {
public:
    InputRouter(App& app, StateModel& state, const ControllerConfig& cfg);

    void SetControllerConfig(const ControllerConfig& cfg);

    // Dispatch a discrete action (from hotkey, controller button, IPC).
    void OnAction(Action a);

    // Apply continuous controller axes for the current tick.
    // `dt_seconds` is the time since the last frame call.
    void OnControllerFrame(const ControllerFrame& frame, float dt_seconds);

    // Returns how many seconds have passed since the controller last
    // produced non-zero stick input. Used for the "snap-back to cursor"
    // behaviour.
    float SecondsSinceLastStickActivity() const noexcept { return idle_; }

    // True if the user has nudged the lens with the controller and the
    // idle-snap-back timer hasn't elapsed yet. While this is true the
    // mouse-follow code should leave the StateModel center alone.
    bool ControllerOwnsCursor() const noexcept;

    // Hand cursor ownership back to mouse-follow immediately. Called
    // from App when it detects the physical mouse has moved - that's
    // the user's signal that they want the lens to follow the cursor
    // again, no idle-timer wait required. Without this, a single
    // keyboard pan or controller stick wiggle would latch the lens
    // away from the cursor until the app was restarted (the keyboard
    // pan path never cleared owning_, because OnControllerFrame is
    // the only place the idle counter advanced).
    void ReleaseCursorOwnership() noexcept;

private:
    App&        app_;
    StateModel& state_;
    ControllerConfig cfg_;
    float       idle_      = 1e9f;   // start "very idle"
    bool        owning_    = false;
};

} // namespace magnifier
