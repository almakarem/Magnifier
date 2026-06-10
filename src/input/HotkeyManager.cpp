#include "input/HotkeyManager.h"
#include "util/Log.h"
#include "util/WinError.h"

namespace magnifier {

HotkeyManager* HotkeyManager::ll_instance_ = nullptr;

namespace {
constexpr wchar_t kClassName[]  = L"MagnifierHotkeyWnd_v1";
constexpr wchar_t kWindowName[] = L"MagnifierHotkeySink";
constexpr int    kFirstHotkeyId = 0xB000;
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
        if (!::RegisterHotKey(msg_wnd_, id,
                              bind.modifiers | MOD_NOREPEAT,
                              bind.vk)) {
            conflicts.push_back({act, bind, LastErrorString()});
            continue;
        }
        slots_.push_back({id, act, bind});
    }
    ll_bindings_ = bindings;   // used by the LL hook if enabled
    return conflicts;
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
