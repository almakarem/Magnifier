#pragma once

#include "input/Actions.h"

#include <Windows.h>
#include <shellapi.h>

#include <functional>

namespace magnifier {

// System-tray notification-area icon + popup menu. All work happens on the
// UI thread (the window proc lives on the calling thread).
class TrayIcon {
public:
    using Sink = std::function<void(Action)>;

    TrayIcon()  = default;
    ~TrayIcon() { Shutdown(); }

    TrayIcon(const TrayIcon&)            = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool Initialise(HINSTANCE hinst, Sink sink);
    void Shutdown();

    // Update the icon tooltip / status text shown on hover.
    void SetTooltip(const std::wstring& text);

    // Show a transient balloon notification (used for first-run hints).
    void ShowBalloon(const std::wstring& title, const std::wstring& text);

private:
    static LRESULT CALLBACK WndProc_(HWND, UINT, WPARAM, LPARAM);
    void ShowMenu_();

    HINSTANCE         hinst_   = nullptr;
    HWND              wnd_     = nullptr;
    Sink              sink_;
    NOTIFYICONDATAW   nid_{};
    bool              added_   = false;
    UINT              wm_taskbar_created_ = 0;
};

} // namespace magnifier
