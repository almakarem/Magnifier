#pragma once

#include "config/ConfigStore.h"
#include "input/Actions.h"

#include <Windows.h>

#include <functional>
#include <map>
#include <vector>

namespace magnifier {

// Registers global hotkeys with the OS and dispatches Action events back
// to a user-supplied sink. Owns no thread of its own — receives WM_HOTKEY
// on a message-only window it creates internally.
class HotkeyManager {
public:
    using Sink = std::function<void(Action)>;

    HotkeyManager()  = default;
    ~HotkeyManager();

    HotkeyManager(const HotkeyManager&)            = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    bool Initialise(HINSTANCE hinst, Sink sink);
    void Shutdown();

    // Replace the active hotkey set. Returns a list of bindings that the OS
    // refused to register (e.g. because they conflict with another app).
    struct Conflict {
        Action        action;
        HotkeyBinding binding;
        std::string   reason;
    };
    std::vector<Conflict> ApplyBindings(const std::map<Action, HotkeyBinding>& bindings);

    // Enable/disable the optional low-level keyboard hook (off by default).
    // Returns the effective state.
    bool SetLowLevelHook(bool enabled);
    bool LowLevelHookEnabled() const noexcept { return ll_hook_ != nullptr; }

    // Plain (unmodified) arrow keys for panning the lens, registered
    // *only* while lens mode is active so they don't hijack typing in
    // other apps. Pass `true` when entering Lens mode, `false` when
    // leaving. Safe to call repeatedly; idempotent.
    void SetTransientPanKeys(bool active);

private:
    static LRESULT CALLBACK WndProc_(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK LowLevelKbdProc_(int, WPARAM, LPARAM);

    void UnregisterAll_();

    HINSTANCE hinst_     = nullptr;
    HWND      msg_wnd_   = nullptr;
    Sink      sink_;

    struct Slot { int id; Action action; HotkeyBinding binding; };
    std::vector<Slot> slots_;

    // IDs used for the transient (lens-only) plain arrow-key hotkeys.
    // Allocated lazily inside SetTransientPanKeys.
    int transient_ids_[4] = {0, 0, 0, 0};   // L, R, U, D
    bool transient_active_ = false;

    HHOOK     ll_hook_   = nullptr;
    std::map<Action, HotkeyBinding> ll_bindings_;
    static HotkeyManager* ll_instance_;   // for the low-level callback
};

} // namespace magnifier
