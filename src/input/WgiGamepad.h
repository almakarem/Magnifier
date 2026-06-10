#pragma once

#include <memory>
#include <string>

namespace magnifier {

// Lightweight wrapper over Windows.Gaming.Input.Gamepad (WinRT). Hides the
// cppwinrt headers from the rest of the codebase — they pull in megabytes
// of templated machinery that we don't want infecting every TU.
//
// Why this matters: XInput (the legacy API ControllerPoll used originally)
// only sees controllers that explicitly emulate the Xbox 360 protocol.
// DualSense / DualShock 4 / Switch Pro / generic HID gamepads paired
// over Bluetooth are invisible to it. Windows.Gaming.Input sees them all.
//
// Thread-safety: Construct on the polling thread. Read() may be called
// freely on that same thread. The WinRT event callbacks fire on internal
// worker threads — we synchronise our cached list of devices internally.
class WgiGamepad {
public:
    struct Reading {
        bool        present       = false;
        float       lx            = 0.0f;   // left  stick X, [-1, 1], +right
        float       ly            = 0.0f;   // left  stick Y, [-1, 1], +up
        float       rx            = 0.0f;   // right stick X, [-1, 1], +right
        float       ry            = 0.0f;   // right stick Y, [-1, 1], +up
        float       lt            = 0.0f;   // left  trigger, [0, 1]
        float       rt            = 0.0f;   // right trigger, [0, 1]
        unsigned    buttons       = 0;      // XInput-compatible button bits
        std::string device_name;            // e.g. "Xbox Wireless Controller"
    };

    WgiGamepad();
    ~WgiGamepad();

    WgiGamepad(const WgiGamepad&)            = delete;
    WgiGamepad& operator=(const WgiGamepad&) = delete;

    // True iff WinRT initialised and the API surface is available. If
    // false, Read() will always return Reading{ present = false } and the
    // caller should fall back to XInput.
    bool Available() const noexcept { return available_; }

    // Snapshot the first connected gamepad (the one that's been around
    // longest). Returns Reading{ present = false } if nothing is connected
    // or the API is unavailable.
    Reading Read();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool                  available_ = false;
};

} // namespace magnifier
