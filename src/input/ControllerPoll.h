#pragma once

#include "config/ConfigStore.h"
#include "input/Actions.h"
#include "input/WgiGamepad.h"

#include <Windows.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace magnifier {

// Snapshot of analog axis state for a single tick. All values normalised to
// [-1, 1] (sticks) or [0, 1] (triggers), with the configured deadzone +
// response curve already applied.
struct ControllerFrame {
    bool  present     = false;
    float ls_x        = 0.0f;
    float ls_y        = 0.0f;
    float rs_x        = 0.0f;
    float rs_y        = 0.0f;
    float lt          = 0.0f;
    float rt          = 0.0f;
    unsigned buttons  = 0;     // XInput button bitfield
    // Diagnostics — populated for the Settings -> Diagnostics tab. NOT
    // used by gameplay logic; the magnifier just needs the floats above.
    std::string backend;       // "WGI" or "XInput" or ""
    std::string device_name;   // e.g. "Xbox Wireless Controller"
    // WGI enumeration counts (best-effort, may be 0 if WGI unavailable).
    unsigned    wgi_gamepad_count = 0;
    unsigned    wgi_raw_count     = 0;
    std::string wgi_first_raw_name;
};

// Polls XInput controllers on a dedicated thread (~125 Hz) and dispatches:
//   - per-tick analog state via OnFrame (for move/zoom)
//   - one-shot Actions via OnAction (for buttons mapped to actions)
// Both callbacks are invoked from the poll thread. The App should marshal
// them onto the UI thread (we use PostMessage).
class ControllerPoll {
public:
    using FrameSink  = std::function<void(const ControllerFrame&)>;
    using ActionSink = std::function<void(Action)>;

    ControllerPoll() = default;
    ~ControllerPoll() { Stop(); }

    ControllerPoll(const ControllerPoll&)            = delete;
    ControllerPoll& operator=(const ControllerPoll&) = delete;

    void SetConfig(const ControllerConfig& cfg);
    void SetCallbacks(FrameSink frame, ActionSink action);

    void Start();
    void Stop();

    bool IsRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    // Returns true if at least one XInput controller is currently connected.
    static bool AnyControllerConnected();

private:
    void PollLoop_();

    std::atomic<bool>  running_{false};
    std::atomic<bool>  enabled_{true};
    std::thread        thread_;

    // Config snapshot — re-read on each poll iteration (cheap).
    mutable std::mutex cfg_mu_;
    ControllerConfig   cfg_;

    FrameSink  on_frame_;
    ActionSink on_action_;

    // Modern gamepad backend (WGI). Constructed lazily inside PollLoop_
    // so winrt::init_apartment happens on the poll thread, not on the
    // UI thread (the UI thread is typically STA and would conflict).
    std::unique_ptr<WgiGamepad> wgi_;
};

} // namespace magnifier
