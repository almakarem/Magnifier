#include "input/HotkeyManager.h"
#include "util/Log.h"
#include "util/WinError.h"

namespace magnifier {

HotkeyManager* HotkeyManager::ll_instance_ = nullptr;

namespace {
constexpr wchar_t kClassName[]  = L"MagnifierHotkeyWnd_v1";
constexpr wchar_t kWindowName[] = L"MagnifierHotkeySink";
constexpr int    kFirstHotkeyId  = 0xB000;
// IDs for lens-mode-only plain arrow keys. Far above the per-bindings range
// so they can never collide.
constexpr int    kTransientIdL   = 0xC001;
constexpr int    kTransientIdR   = 0xC002;
constexpr int    kTransientIdU   = 0xC003;
constexpr int    kTransientIdD   = 0xC004;
} // namespace

HotkeyManager::~HotkeyManager() {
    Shutdown();
}

bool HotkeyManager::Initialise(HINSTANCE hinst, Sink sink) {
    if (msg_wnd_) return true;
    hinst_ = hinst;
    sink_  = std::move(sink);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &HotkeyManager::WndProc_;
    wc.hInstance     = hinst;
    wc.lpszClassName = kClassName;
    if (!::RegisterClassExW(&wc)) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            spdlog::error("HotkeyManager: RegisterClassExW failed: {}",
                          LastErrorString(err));
            return false;
        }
    }
    msg_wnd_ = ::CreateWindowExW(0, kClassName, kWindowName,
        0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hinst, this);
    if (!msg_wnd_) {
        spdlog::error("HotkeyManager: CreateWindowExW failed: {}", LastErrorString());
        return false;
    }
    ::SetWindowLongPtrW(msg_wnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

void HotkeyManager::Shutdown() {
    SetLowLevelHook(false);
    SetTransientPanKeys(false);
    UnregisterAll_();
    if (msg_wnd_) {
        ::DestroyWindow(msg_wnd_);
        msg_wnd_ = nullptr;
    }
}

void HotkeyManager::UnregisterAll_() {
    if (!msg_wnd_) return;
    for (const auto& s : slots_) {
        ::UnregisterHotKey(msg_wnd_, s.id);
    }
    slots_.clear();
    // Note: transient (lens-mode) pan keys are NOT touched here because
    // ApplyBindings calls us mid-session — yanking the arrow keys out from
    // under the user while lens mode is active would surprise them. They
    // are released by SetTransientPanKeys(false) or Shutdown().
}

std::vector<HotkeyManager::Conflict>
HotkeyManager::ApplyBindings(const std::map<Action, HotkeyBinding>& bindings) {
    std::vector<Conflict> conflicts;
    UnregisterAll_();
    if (!msg_wnd_) return conflicts;

    int next_id = kFirstHotkeyId;
    for (const auto& [act, bind] : bindings) {
        if (!bind.is_bound()) continue;
        const int id = next_id++;
        // Allow keyboard autorepeat for the pan actions so holding the
        // arrow key produces smooth panning. All other actions are one-
        // shot (NOREPEAT prevents accidental rapid retoggling of modes).
        const bool autorepeat =
            act == Action::PanLeft  || act == Action::PanRight ||
            act == Action::PanUp    || act == Action::PanDown;
        const UINT mods = bind.modifiers | (autorepeat ? 0u : MOD_NOREPEAT);
        if (!::RegisterHotKey(msg_wnd_, id, mods, bind.vk)) {
            conflicts.push_back({act, bind, LastErrorString()});
            continue;
        }
        slots_.push_back({id, act, bind});
    }
    ll_bindings_ = bindings;   // used by the LL hook if enabled
    return conflicts;
}

void HotkeyManager::SetTransientPanKeys(bool active) {
    if (!msg_wnd_) return;
    if (active == transient_active_) return;
    if (active) {
        // Plain (no-modifier) arrow keys, autorepeat enabled so holding
        // the key pans smoothly. They're scoped to lens mode by App, so
        // they shouldn't interfere with text editing in other apps.
        const struct { int id; UINT vk; } keys[] = {
            { kTransientIdL, VK_LEFT  },
            { kTransientIdR, VK_RIGHT },
            { kTransientIdU, VK_UP    },
            { kTransientIdD, VK_DOWN  },
        };
        for (const auto& k : keys) {
            if (!::RegisterHotKey(msg_wnd_, k.id, 0u, k.vk)) {
                spdlog::warn("Lens-mode arrow hotkey registration failed "
                             "(vk=0x{:02x}): {}", k.vk, LastErrorString());
            }
        }
        spdlog::info("Lens mode: plain arrow keys registered for panning.");
    } else {
        ::UnregisterHotKey(msg_wnd_, kTransientIdL);
        ::UnregisterHotKey(msg_wnd_, kTransientIdR);
        ::UnregisterHotKey(msg_wnd_, kTransientIdU);
        ::UnregisterHotKey(msg_wnd_, kTransientIdD);
        spdlog::info("Lens mode exited: plain arrow keys released.");
    }
    transient_active_ = active;
}

bool HotkeyManager::SetLowLevelHook(bool enabled) {
    if (enabled && !ll_hook_) {
        ll_instance_ = this;
        ll_hook_ = ::SetWindowsHookExW(WH_KEYBOARD_LL,
                                       &HotkeyManager::LowLevelKbdProc_,
                                       hinst_, 0);
        if (!ll_hook_) {
            spdlog::error("SetWindowsHookExW(WH_KEYBOARD_LL) failed: {}",
                          LastErrorString());
            ll_instance_ = nullptr;
            return false;
        }
        spdlog::info("Low-level keyboard hook ENABLED (anti-cheat risk).");
    } else if (!enabled && ll_hook_) {
        ::UnhookWindowsHookEx(ll_hook_);
        ll_hook_ = nullptr;
        ll_instance_ = nullptr;
        spdlog::info("Low-level keyboard hook disabled.");
    }
    return ll_hook_ != nullptr;
}

LRESULT CALLBACK HotkeyManager::WndProc_(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_HOTKEY) {
        auto* self = reinterpret_cast<HotkeyManager*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self) {
            const int id = static_cast<int>(wp);
            // Transient (lens-mode-only) plain arrow keys first.
            switch (id) {
                case kTransientIdL: if (self->sink_) self->sink_(Action::PanLeft);  return 0;
                case kTransientIdR: if (self->sink_) self->sink_(Action::PanRight); return 0;
                case kTransientIdU: if (self->sink_) self->sink_(Action::PanUp);    return 0;
                case kTransientIdD: if (self->sink_) self->sink_(Action::PanDown);  return 0;
                default: break;
            }
            for (const auto& s : self->slots_) {
                if (s.id == id && self->sink_) {
                    self->sink_(s.action);
                    break;
                }
            }
        }
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK HotkeyManager::LowLevelKbdProc_(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && ll_instance_ &&
        (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {

        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);

        // Build current modifier state from GetAsyncKeyState.
        unsigned mods = 0;
        if (::GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
        if (::GetAsyncKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
        if (::GetAsyncKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;
        if ((::GetAsyncKeyState(VK_LWIN) | ::GetAsyncKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;

        for (const auto& [act, bind] : ll_instance_->ll_bindings_) {
            if (!bind.is_bound()) continue;
            if (bind.vk == k->vkCode && bind.modifiers == mods) {
                if (ll_instance_->sink_) ll_instance_->sink_(act);
                // Consume the keystroke so it doesn't reach the foreground app.
                return 1;
            }
        }
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

} // namespace magnifier
