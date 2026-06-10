#include "mag/MagController.h"
#include "util/Log.h"
#include "util/WinError.h"

// Magnification API
#include <magnification.h>
#pragma comment(lib, "Magnification.lib")

// timeBeginPeriod / timeEndPeriod for raising the system timer resolution
// while a magnification mode is active.
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <cmath>

namespace magnifier {

namespace {

constexpr wchar_t kHostClassName[] = L"MagnifierHostWindow_v1";

// WDA_EXCLUDEFROMCAPTURE was added in W10 2004. Some SDK headers still lack
// the constant; redefine defensively.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

bool OsBuildAtLeast(DWORD build) {
    // Use RtlGetVersion via ntdll so manifest declarations don't lie to us.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOEXW);
    static const RtlGetVersionFn fn = []() -> RtlGetVersionFn {
        HMODULE m = ::GetModuleHandleW(L"ntdll.dll");
        return m ? reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(m, "RtlGetVersion"))
                 : nullptr;
    }();
    if (!fn) return false;
    RTL_OSVERSIONINFOEXW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    return fn(&info) == 0 && info.dwBuildNumber >= build;
}

// Compute the virtual screen bounds spanning all monitors.
ScreenBounds VirtualScreenBounds() {
    ScreenBounds b;
    b.left   = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    b.top    = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    b.right  = b.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    b.bottom = b.top  + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return b;
}

} // namespace

MagController::MagController() = default;

MagController::~MagController() {
    Shutdown();
}

bool MagController::Initialise(HINSTANCE hinst, StateModel* state) {
    if (mag_initialised_) return true;
    hinst_ = hinst;
    state_ = state;

    if (!::MagInitialize()) {
        spdlog::error("MagInitialize failed: {}", LastErrorString());
        return false;
    }
    mag_initialised_ = true;

    if (!RegisterHostClass_(hinst)) {
        Shutdown();
        return false;
    }
    if (!CreateHostWindow_(hinst)) {
        Shutdown();
        return false;
    }
    if (!CreateMagChild_()) {
        Shutdown();
        return false;
    }

    // Seed bounds with the virtual desktop.
    if (state_) state_->SetBounds(VirtualScreenBounds());

    spdlog::info("MagController initialised (host hwnd={})",
                 reinterpret_cast<void*>(host_));
    return true;
}

void MagController::Shutdown() {
    DestroyHost_();
    if (hires_timer_) {
        ::timeEndPeriod(1);
        hires_timer_ = false;
    }
    if (mag_initialised_) {
        ::MagUninitialize();
        mag_initialised_ = false;
    }
    state_ = nullptr;
}

bool MagController::RegisterHostClass_(HINSTANCE hinst) {
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = 0;
    wc.lpfnWndProc   = &MagController::HostProc_;
    wc.hInstance     = hinst;
    wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
    // Opaque black background. The host is a layered window used purely
    // as a render surface for the WC_MAGNIFIER child; any area the child
    // doesn't cover (e.g. during a brief size mismatch on DPI change or
    // before the first Tick after a monitor reconfig) MUST be a defined
    // colour or the user sees uninitialised layered bits, which DWM
    // composites as white. Black is the correct fallback because the
    // Magnification API also paints black for off-screen source pixels.
    wc.hbrBackground = reinterpret_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kHostClassName;
    if (!::RegisterClassExW(&wc)) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            spdlog::error("RegisterClassExW failed: {}", LastErrorString(err));
            return false;
        }
    }
    return true;
}

bool MagController::CreateHostWindow_(HINSTANCE hinst) {
    const auto vb = VirtualScreenBounds();

    // Layered + topmost + transparent to clicks.
    // WS_CLIPCHILDREN is REQUIRED so the magnifier child paints in the host
    // area without being overdrawn by the layered surface's own paints.
    const DWORD ex_style = WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                           WS_EX_TOOLWINDOW;
    const DWORD style    = WS_POPUP | WS_CLIPCHILDREN;

    host_ = ::CreateWindowExW(
        ex_style, kHostClassName, L"Magnifier",
        style,
        vb.left, vb.top, vb.width(), vb.height(),
        nullptr, nullptr, hinst, this);
    if (!host_) {
        spdlog::error("CreateWindowExW (host) failed: {}", LastErrorString());
        return false;
    }
    // Store `this` so HostProc_ can dispatch WM_SIZE / WM_DPICHANGED back
    // to the controller. CreateWindowExW passes `this` as lpParam; we set
    // GWLP_USERDATA here (vs in WM_NCCREATE) so the layered-window setup
    // below sees a fully-wired window.
    ::SetWindowLongPtrW(host_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // Fully opaque layered (we use it as a render surface, not for alpha).
    ::SetLayeredWindowAttributes(host_, 0, 255, LWA_ALPHA);

    // Hide the host from screen-capture by default if supported.
    if (SupportsCaptureExclusion()) {
        SetExcludedFromCapture(true);
    }
    return true;
}

bool MagController::CreateMagChild_() {
    RECT rc{};
    ::GetClientRect(host_, &rc);
    // WS_VISIBLE is REQUIRED at creation time for the Magnification control.
    // Without it the child never paints and the lens shows black.
    // MS_SHOWMAGNIFIEDCURSOR is only added when the user opts in — the OS
    // hardware cursor renders at display refresh rate with zero lag, while
    // the in-content cursor only updates on the mag control's internal
    // composition tick and visibly ghosts behind fast mouse movement.
    DWORD style = WS_CHILD | WS_VISIBLE;
    if (magnify_cursor_) style |= MS_SHOWMAGNIFIEDCURSOR;
    mag_ = ::CreateWindowExW(
        0, WC_MAGNIFIERW, L"MagnifierChild",
        style,
        rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
        host_, nullptr, hinst_, nullptr);
    if (!mag_) {
        spdlog::error("CreateWindowExW (WC_MAGNIFIER) failed: {}",
                      LastErrorString());
        return false;
    }
    // Always exclude our own host from the magnification source — prevents
    // a positive-feedback loop where the mag control sees itself.
    HWND filtered[] = { host_ };
    ::MagSetWindowFilterList(mag_, MW_FILTERMODE_EXCLUDE, 1, filtered);
    return true;
}

void MagController::DestroyHost_() {
    if (mag_)  { ::DestroyWindow(mag_);  mag_  = nullptr; }
    if (host_) { ::DestroyWindow(host_); host_ = nullptr; }
}

void MagController::SetMode(MagMode mode) {
    if (mode == current_) return;

    // Leaving a mode: hide + clear residual fullscreen transform.
    if (current_ == MagMode::Fullscreen) {
        ::MagSetFullscreenTransform(1.0f, 0, 0);
    }
    if (current_ != MagMode::Off && host_) {
        ::ShowWindow(host_, SW_HIDE);
    }

    // Drop the 1 ms timer when no mode is active; raise it again on entry.
    // Without this, WM_TIMER fires on the default ~15.6 ms scheduler tick,
    // which gives visibly jittery lens tracking.
    if (current_ != MagMode::Off && mode == MagMode::Off && hires_timer_) {
        ::timeEndPeriod(1);
        hires_timer_ = false;
    }
    if (current_ == MagMode::Off && mode != MagMode::Off && !hires_timer_) {
        if (::timeBeginPeriod(1) == TIMERR_NOERROR) hires_timer_ = true;
    }

    current_ = mode;
    if (state_) state_->SetMode(mode);

    // Force the next ApplyLens_ to re-push the transform regardless of the
    // cached value (otherwise after a Shutdown/Re-Initialise the driver
    // forgets it but we still skip the call).
    last_src_w_ = -1;
    last_src_h_ = -1;

    if (mode == MagMode::Lens && host_) {
        // Resize host to lens size first (Tick will keep it placed).
        if (state_) {
            const auto s = state_->GetSnapshot();
            ::SetWindowPos(host_, HWND_TOPMOST,
                static_cast<int>(s.center_x) - s.lens.width / 2,
                static_cast<int>(s.center_y) - s.lens.height / 2,
                s.lens.width, s.lens.height,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
        } else {
            ::ShowWindow(host_, SW_SHOWNOACTIVATE);
        }
    } else if (mode == MagMode::Fullscreen && host_) {
        // Fullscreen mode doesn't use our host window for compositing —
        // MagSetFullscreenTransform applies directly. Keep host hidden.
        ::ShowWindow(host_, SW_HIDE);
    }
}

void MagController::Tick() {
    if (!mag_initialised_ || !state_) return;
    const auto snap = state_->GetSnapshot();
    switch (snap.mode) {
        case MagMode::Off:        break;
        case MagMode::Lens:       ApplyLens_(snap);       break;
        case MagMode::Fullscreen: ApplyFullscreen_(snap); break;
    }
}

void MagController::ApplyLens_(const StateModel::Snapshot& snap) {
    if (!host_ || !mag_) return;

    const int hw = snap.lens.width;
    const int hh = snap.lens.height;
    const int cx = static_cast<int>(std::lround(snap.center_x));
    const int cy = static_cast<int>(std::lround(snap.center_y));

    // ---- Choose an integer source-rect size that matches the requested
    // zoom AS CLOSELY AS POSSIBLE, then derive everything else from it.
    //
    // We round to the nearest *even* integer for two reasons:
    //   1. With even src_w, `cx - src_w/2` is exactly the cursor pixel,
    //      i.e. the rect is symmetric around the cursor. For odd src_w
    //      the integer-division biases the rect by half a pixel; as the
    //      eased zoom drifts through values where src_w toggles parity
    //      the centre jumps by half a source pixel — four-ish display
    //      pixels of visible left/right jitter at high zoom.
    //   2. The Magnification API performs its own sub-pixel filtering
    //      and is happiest when src/dst is a clean ratio.
    auto round_even = [](float v) {
        int n = static_cast<int>(std::lround(v));
        if (n < 2) n = 2;
        if (n & 1) ++n;
        return n;
    };
    const int src_w = round_even(hw / snap.zoom);
    const int src_h = round_even(hh / snap.zoom);

    // The transform MUST be derived from the integer source size, not from
    // the raw zoom target. Otherwise src_w * transform != window_w and the
    // mag control resamples by the fractional difference every frame —
    // that's the "shake" users see while zooming. With this derivation
    //   src_w * eff_zoom_x == hw    (exactly)
    // so each frame is internally consistent and the only motion the user
    // sees comes from the eased zoom stepping through discrete src sizes.
    const float eff_zoom_x = static_cast<float>(hw) / static_cast<float>(src_w);
    const float eff_zoom_y = static_cast<float>(hh) / static_cast<float>(src_h);

    // Push the transform only when the integer source size changes —
    // otherwise the transform is byte-for-byte identical to last frame.
    if (src_w != last_src_w_ || src_h != last_src_h_) {
        MAGTRANSFORM tx{};
        tx.v[0][0] = eff_zoom_x;
        tx.v[1][1] = eff_zoom_y;
        tx.v[2][2] = 1.0f;
        ::MagSetWindowTransform(mag_, &tx);
        last_src_w_ = src_w;
        last_src_h_ = src_h;
    }

    RECT src{};
    src.left   = cx - src_w / 2;
    src.top    = cy - src_h / 2;
    src.right  = src.left + src_w;
    src.bottom = src.top  + src_h;
    ::MagSetWindowSource(mag_, src);

    // Position host so the cursor is at its centre. We deliberately allow
    // the host to fall partially off the virtual desktop so the cursor can
    // reach screen corners — the Magnification API renders black for any
    // off-screen source pixels, which is the standard behaviour users
    // expect from a magnifier.
    const int x = cx - hw / 2;
    const int y = cy - hh / 2;

    // Move the host. NOZORDER avoids a per-frame topmost recalc by DWM
    // (the window is already in the topmost group from SetMode()), and we
    // drop SHOWWINDOW because the window is already visible.
    ::SetWindowPos(host_, nullptr, x, y, hw, hh,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING);
    ::SetWindowPos(mag_,  nullptr, 0, 0, hw, hh,
        SWP_NOACTIVATE | SWP_NOZORDER);

    // Force the magnification control to repaint its buffer synchronously
    // this tick. InvalidateRect alone just queues WM_PAINT — the mag
    // control might not actually resample the desktop until the next
    // message dispatch, which is what produced the visible "ghost" trail
    // behind fast cursor moves. UpdateWindow forces the paint to happen
    // *now*, before SetWindowPos's frame is composited by DWM.
    ::InvalidateRect(mag_, nullptr, FALSE);
    ::UpdateWindow(mag_);
}

void MagController::ApplyFullscreen_(const StateModel::Snapshot& snap) {
    // Use the monitor the lens center is on, not always the primary monitor
    // — otherwise on multi-display setups the pan target is computed in the
    // wrong coordinate space and the user can't reach the edges of the
    // monitor they're actually looking at.
    const POINT centre_pt{ static_cast<LONG>(std::lround(snap.center_x)),
                           static_cast<LONG>(std::lround(snap.center_y)) };
    HMONITOR mon = ::MonitorFromPoint(centre_pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof(MONITORINFO) };
    if (!::GetMonitorInfoW(mon, &mi)) {
        mi.rcMonitor = { 0, 0,
            ::GetSystemMetrics(SM_CXSCREEN),
            ::GetSystemMetrics(SM_CYSCREEN) };
    }
    const int mon_cx = mi.rcMonitor.right  - mi.rcMonitor.left;
    const int mon_cy = mi.rcMonitor.bottom - mi.rcMonitor.top;

    // Offsets are in unmagnified coordinates relative to the magnified
    // monitor's upper-left corner.
    const int x_off = static_cast<int>(std::lround(
        snap.center_x - mi.rcMonitor.left - mon_cx / (2.0f * snap.zoom)));
    const int y_off = static_cast<int>(std::lround(
        snap.center_y - mi.rcMonitor.top  - mon_cy / (2.0f * snap.zoom)));

    ::MagSetFullscreenTransform(snap.zoom, x_off, y_off);

    // Input routing (W10 1703+). Tell the input pipeline that the region
    // shown on screen corresponds to the un-magnified source rectangle.
    RECT src{};
    src.left   = mi.rcMonitor.left + x_off;
    src.top    = mi.rcMonitor.top  + y_off;
    src.right  = src.left + static_cast<int>(std::lround(mon_cx / snap.zoom));
    src.bottom = src.top  + static_cast<int>(std::lround(mon_cy / snap.zoom));
    ::MagSetInputTransform(TRUE, &src, &mi.rcMonitor);
}

bool MagController::SupportsCaptureExclusion() {
    return OsBuildAtLeast(19041);   // Windows 10 version 2004
}

void MagController::SetExcludedFromCapture(bool exclude) {
    excluded_from_capture_ = exclude;
    if (!host_) return;
    if (!SupportsCaptureExclusion()) return;
    // SetWindowDisplayAffinity is in user32.dll on supported versions.
    using SetWDAFn = BOOL(WINAPI*)(HWND, DWORD);
    static const SetWDAFn fn = []() -> SetWDAFn {
        HMODULE m = ::GetModuleHandleW(L"user32.dll");
        return m ? reinterpret_cast<SetWDAFn>(::GetProcAddress(m, "SetWindowDisplayAffinity")) : nullptr;
    }();
    if (!fn) return;
    fn(host_, exclude ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
}

void MagController::SetMagnifyCursor(bool enable) {
    if (enable == magnify_cursor_ && mag_) return;
    magnify_cursor_ = enable;
    if (!mag_initialised_ || !host_) return;
    // Swap the child: the magnifier control's style cannot be changed in
    // place because MS_SHOWMAGNIFIEDCURSOR is honoured at CreateWindowEx
    // time. Destroy and recreate.
    if (mag_) {
        ::DestroyWindow(mag_);
        mag_ = nullptr;
    }
    CreateMagChild_();
    // The cached transform was tied to the old HWND; force re-push next tick.
    last_src_w_ = -1;
    last_src_h_ = -1;
    spdlog::info("MagController: magnify_cursor = {}", enable);
}

LRESULT CALLBACK MagController::HostProc_(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<MagController*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_DESTROY:
            return 0;
        case WM_NCHITTEST:
            // Click-through.
            return HTTRANSPARENT;
        case WM_SIZE: {
            // Keep the WC_MAGNIFIER child exactly aligned with the host's
            // client area at all times. Without this, when the host is
            // resized (mode change, DPI change, lens-size hotkey) the
            // child stays at its previous size and the user sees a band
            // of stale layered bits around the magnified region.
            if (self && self->mag_) {
                const int w = LOWORD(lp);
                const int h = HIWORD(lp);
                ::SetWindowPos(self->mag_, nullptr, 0, 0, w, h,
                    SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            // The mag control is repositioned/resized every tick by
            // ApplyLens_, so we just need to force the next tick to
            // re-push the transform (cached value is for the old DPI).
            if (self) {
                self->last_src_w_ = -1;
                self->last_src_h_ = -1;
                spdlog::info("MagController: DPI changed to {}", LOWORD(wp));
            }
            return 0;
        }
        default:
            return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

} // namespace magnifier
