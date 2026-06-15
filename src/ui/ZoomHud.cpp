#include "ui/ZoomHud.h"
#include "mag/MagController.h"   // SupportsCaptureExclusion()

#include <cstdio>

// WDA_EXCLUDEFROMCAPTURE was added in W10 2004; some SDK headers still lack it.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace magnifier {

namespace {

constexpr wchar_t kClass[]      = L"MagnifierZoomHud_v1";
constexpr float   kHoldSeconds  = 0.9f;   // full-opacity dwell after last change
constexpr float   kFadeSeconds  = 0.4f;   // fade-out duration
constexpr int     kBaseW        = 120;    // logical px @ 96 DPI
constexpr int     kBaseH        = 56;

UINT DpiForWindow(HWND hwnd) {
    // GetDpiForWindow is available on our minimum target (Win10 1809). Guard
    // defensively anyway in case a shim strips it.
    using Fn = UINT (WINAPI*)(HWND);
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    if (auto fn = u32 ? reinterpret_cast<Fn>(
            ::GetProcAddress(u32, "GetDpiForWindow")) : nullptr) {
        const UINT d = fn(hwnd);
        if (d >= 72) return d;
    }
    return 96;
}

} // namespace

ZoomHud::~ZoomHud() {
    if (hwnd_) {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool ZoomHud::EnsureWindow_() {
    if (hwnd_) return true;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &ZoomHud::WndProc_;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    ::RegisterClassExW(&wc);   // ignore ERROR_CLASS_ALREADY_EXISTS

    hwnd_ = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        kClass, L"", WS_POPUP,
        0, 0, kBaseW, kBaseH,
        nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) return false;

    ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // Keep the badge out of OBS / screen capture, matching the lens overlay,
    // so streamers don't get a floating "3.0x" baked into their broadcast.
    if (MagController::SupportsCaptureExclusion()) {
        ::SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
    }
    return true;
}

void ZoomHud::Reposition_() {
    if (!hwnd_) return;

    POINT pt{};
    ::GetCursorPos(&pt);
    HMONITOR mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    RECT area{0, 0, 1920, 1080};
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (mon && ::GetMonitorInfoW(mon, &mi)) area = mi.rcMonitor;

    const UINT dpi = DpiForWindow(hwnd_);
    const int  w   = ::MulDiv(kBaseW, static_cast<int>(dpi), 96);
    const int  h   = ::MulDiv(kBaseH, static_cast<int>(dpi), 96);

    const int x = area.left + (area.right - area.left - w) / 2;
    const int y = area.top  + (area.bottom - area.top) / 10;   // ~10% down

    ::SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h,
                   SWP_NOACTIVATE | SWP_NOREDRAW);

    // Rounded-corner badge: clip the window to a round rect.
    const int r = h / 3;
    HRGN rgn = ::CreateRoundRectRgn(0, 0, w + 1, h + 1, r, r);
    ::SetWindowRgn(hwnd_, rgn, FALSE);   // window owns the region now
}

void ZoomHud::Flash(float zoom) {
    if (!enabled_) return;
    if (!EnsureWindow_()) return;

    wchar_t buf[16];
    swprintf(buf, _countof(buf), L"%.1f×", static_cast<double>(zoom)); // N.N×
    text_  = buf;
    hold_  = kHoldSeconds;
    alpha_ = 1.0f;

    Reposition_();
    ::SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
    if (!visible_) {
        ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        visible_ = true;
    }
    ::InvalidateRect(hwnd_, nullptr, FALSE);
    ::UpdateWindow(hwnd_);
}

void ZoomHud::Tick(float dt) {
    if (!visible_ || !hwnd_) return;
    if (hold_ > 0.0f) {
        hold_ -= dt;
        return;
    }
    alpha_ -= dt / kFadeSeconds;
    if (alpha_ <= 0.0f) {
        Hide();
        return;
    }
    ::SetLayeredWindowAttributes(
        hwnd_, 0, static_cast<BYTE>(alpha_ * 255.0f), LWA_ALPHA);
}

void ZoomHud::Hide() {
    if (hwnd_ && visible_) ::ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
    hold_    = 0.0f;
    alpha_   = 0.0f;
}

void ZoomHud::Paint_(HDC dc, const RECT& rc) {
    // Dark badge background (the round-rect region clips the corners).
    HBRUSH bg = ::CreateSolidBrush(RGB(20, 22, 28));
    ::FillRect(dc, &rc, bg);
    ::DeleteObject(bg);

    // Cyan accent frame, matching the brand / lens border colour.
    HBRUSH frame = ::CreateSolidBrush(RGB(0, 229, 255));
    ::FrameRect(dc, &rc, frame);
    ::DeleteObject(frame);

    const int h = rc.bottom - rc.top;
    HFONT font = ::CreateFontW(
        -(h * 5 / 12), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old_font = ::SelectObject(dc, font);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, RGB(240, 245, 250));
    RECT t = rc;
    ::DrawTextW(dc, text_.c_str(), -1, &t,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SelectObject(dc, old_font);
    ::DeleteObject(font);
}

LRESULT CALLBACK ZoomHud::WndProc_(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<ZoomHud*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_PAINT && self) {
        PAINTSTRUCT ps{};
        HDC dc = ::BeginPaint(hwnd, &ps);
        RECT rc{};
        ::GetClientRect(hwnd, &rc);
        self->Paint_(dc, rc);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace magnifier
