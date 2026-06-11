#include "ui/TrayIcon.h"
#include "util/Log.h"
#include "util/WinError.h"

#ifdef HAVE_ICON
#include "resource.h"   // IDI_APP
#endif

namespace magnifier {

namespace {

constexpr wchar_t kTrayClassName[] = L"MagnifierTrayWnd_v1";
constexpr UINT    WM_TRAY_ICON     = WM_APP + 1;
constexpr UINT    kTrayUid         = 0x4D41474D;   // 'MAGM'

enum TrayMenuId : UINT {
    ID_TRAY_LENS = 100,
    ID_TRAY_FULLSCREEN,
    ID_TRAY_OFF,
    ID_TRAY_SETTINGS,
    ID_TRAY_RELOAD,
    ID_TRAY_QUIT,
};

} // namespace

bool TrayIcon::Initialise(HINSTANCE hinst, Sink sink) {
    if (wnd_) return true;
    hinst_ = hinst;
    sink_  = std::move(sink);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &TrayIcon::WndProc_;
    wc.hInstance     = hinst;
    wc.lpszClassName = kTrayClassName;
    if (!::RegisterClassExW(&wc)) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            spdlog::error("TrayIcon: RegisterClassExW failed: {}", LastErrorString(err));
            return false;
        }
    }
    wnd_ = ::CreateWindowExW(0, kTrayClassName, L"MagnifierTray",
        0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hinst, this);
    if (!wnd_) {
        spdlog::error("TrayIcon: CreateWindowExW failed: {}", LastErrorString());
        return false;
    }
    ::SetWindowLongPtrW(wnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // The shell broadcasts this if explorer restarts; we re-add on receipt.
    wm_taskbar_created_ = ::RegisterWindowMessageW(L"TaskbarCreated");

    nid_.cbSize           = sizeof(nid_);
    nid_.hWnd             = wnd_;
    nid_.uID              = kTrayUid;
    nid_.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = WM_TRAY_ICON;
    nid_.hIcon            = nullptr;
#ifdef HAVE_ICON
    // Try the embedded multi-resolution icon first; LoadImage picks the
    // 16x16 entry which is the correct size for the tray notification area.
    nid_.hIcon = static_cast<HICON>(::LoadImageW(
        hinst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXSMICON),
        ::GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
#endif
    if (!nid_.hIcon) {
        nid_.hIcon = ::LoadIcon(nullptr, IDI_APPLICATION);
    }
    std::wcscpy(nid_.szTip, L"Magnifier");

    if (!::Shell_NotifyIconW(NIM_ADD, &nid_)) {
        spdlog::error("Shell_NotifyIcon(NIM_ADD) failed: {}", LastErrorString());
        return false;
    }
    added_ = true;
    nid_.uVersion = NOTIFYICON_VERSION_4;
    ::Shell_NotifyIconW(NIM_SETVERSION, &nid_);
    return true;
}

void TrayIcon::Shutdown() {
    if (added_) {
        ::Shell_NotifyIconW(NIM_DELETE, &nid_);
        added_ = false;
    }
    if (wnd_) {
        ::DestroyWindow(wnd_);
        wnd_ = nullptr;
    }
}

void TrayIcon::SetTooltip(const std::wstring& text) {
    if (!added_) return;
    nid_.uFlags = NIF_TIP;
    std::wcsncpy(nid_.szTip, text.c_str(), _countof(nid_.szTip) - 1);
    nid_.szTip[_countof(nid_.szTip) - 1] = L'\0';
    ::Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void TrayIcon::ShowBalloon(const std::wstring& title, const std::wstring& text) {
    if (!added_) return;
    NOTIFYICONDATAW n = nid_;
    n.uFlags = NIF_INFO;
    std::wcsncpy(n.szInfoTitle, title.c_str(), _countof(n.szInfoTitle) - 1);
    std::wcsncpy(n.szInfo,      text.c_str(),  _countof(n.szInfo)      - 1);
    n.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    ::Shell_NotifyIconW(NIM_MODIFY, &n);
}

void TrayIcon::ShowMenu_() {
    HMENU menu = ::CreatePopupMenu();
    if (!menu) return;
    ::AppendMenuW(menu, MF_STRING, ID_TRAY_LENS,       L"Toggle &Lens");
    ::AppendMenuW(menu, MF_STRING, ID_TRAY_FULLSCREEN, L"Toggle &Full-screen");
    ::AppendMenuW(menu, MF_STRING, ID_TRAY_OFF,        L"Turn &Off");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS,   L"&Settings...");
    ::AppendMenuW(menu, MF_STRING, ID_TRAY_RELOAD,     L"&Reload Config");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT,       L"&Quit");

    POINT pt{};
    ::GetCursorPos(&pt);
    // Required so the menu dismisses if the user clicks elsewhere.
    ::SetForegroundWindow(wnd_);
    const int cmd = ::TrackPopupMenu(menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        pt.x, pt.y, 0, wnd_, nullptr);
    ::DestroyMenu(menu);

    if (!sink_) return;
    switch (cmd) {
        case ID_TRAY_LENS:       sink_(Action::ToggleLens);       break;
        case ID_TRAY_FULLSCREEN: sink_(Action::ToggleFullscreen); break;
        case ID_TRAY_OFF:        sink_(Action::TurnOff);          break;
        case ID_TRAY_SETTINGS:   sink_(Action::ShowSettings);     break;
        case ID_TRAY_RELOAD:     sink_(Action::ReloadConfig);     break;
        case ID_TRAY_QUIT:       sink_(Action::Quit);             break;
        default: break;
    }
}

LRESULT CALLBACK TrayIcon::WndProc_(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<TrayIcon*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (self && self->wm_taskbar_created_ != 0 && msg == self->wm_taskbar_created_) {
        // Explorer restarted — re-add our icon.
        if (!self->added_) {
            ::Shell_NotifyIconW(NIM_ADD, &self->nid_);
            self->added_ = true;
        }
        return 0;
    }

    if (msg == WM_TRAY_ICON && self) {
        const UINT event = LOWORD(lp);
        switch (event) {
            case WM_LBUTTONUP:
            case NIN_SELECT:
                if (self->sink_) self->sink_(Action::ToggleLens);
                return 0;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:
                self->ShowMenu_();
                return 0;
            default:
                return 0;
        }
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace magnifier
