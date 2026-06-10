#pragma once

#include "mag/StateModel.h"

#include <Windows.h>

#include <atomic>
#include <string>

namespace magnifier {

// Wraps the Win32 Magnification API. Owns the layered host window and the
// magnifier control. Single-threaded — all calls (including Tick) must happen
// on the UI / message-pump thread that created the host window.
class MagController {
public:
    MagController();
    ~MagController();

    MagController(const MagController&)            = delete;
    MagController& operator=(const MagController&) = delete;

    // Initialise the Magnification API and create the host window.
    // Returns false if magnification.dll isn't available (very old Windows
    // builds without it). Idempotent.
    bool Initialise(HINSTANCE hinst, StateModel* state);

    // Tear down host window + uninitialise. Safe to call multiple times.
    void Shutdown();

    // Switch active mode. Must be called from the UI thread.
    void SetMode(MagMode mode);

    // Apply the current StateModel snapshot to the live magnifier.
    // Called from the 60 Hz refresh timer.
    void Tick();

    // Returns true if the OS supports excluding the host window from
    // capture (W10 2004+).
    static bool SupportsCaptureExclusion();

    // Toggle whether OBS/screen-capture sees the magnified overlay.
    void SetExcludedFromCapture(bool exclude);

    // Choose between the OS hardware cursor (false, default — smooth) and
    // the Magnification API's in-content cursor (true — zoomed but lags on
    // fast mouse moves). Recreates the mag child window if the value
    // changes after Initialise().
    void SetMagnifyCursor(bool enable);

    // Native handles (for IpcServer / settings).
    HWND HostHwnd()  const { return host_; }
    HWND MagHwnd()   const { return mag_;  }

private:
    bool RegisterHostClass_(HINSTANCE hinst);
    bool CreateHostWindow_(HINSTANCE hinst);
    bool CreateMagChild_();
    void ApplyLens_(const StateModel::Snapshot& snap);
    void ApplyFullscreen_(const StateModel::Snapshot& snap);
    void DestroyHost_();

    static LRESULT CALLBACK HostProc_(HWND, UINT, WPARAM, LPARAM);

    StateModel*       state_      = nullptr;
    HINSTANCE         hinst_      = nullptr;
    HWND              host_       = nullptr;
    HWND              mag_        = nullptr;
    bool              mag_initialised_ = false;
    MagMode           current_    = MagMode::Off;
    bool              excluded_from_capture_ = true;
    // Whether the mag control draws its own zoomed cursor (MS_SHOWMAGNIFIED-
    // CURSOR). Default off: the OS hardware cursor shows through with zero
    // lag, which is what users almost always want.
    bool              magnify_cursor_ = false;
    // True while we hold a 1 ms multimedia-timer period via timeBeginPeriod
    // (needed so WM_TIMER fires on a stable 16 ms cadence and the lens
    // doesn't visibly stutter).
    bool              hires_timer_ = false;
    // Last source-rect dimensions we pushed. Both MagSetWindowSource and
    // MagSetWindowTransform are derived from these so that
    //   src_w * eff_zoom_x == window_w   (exact, no sub-pixel oscillation)
    // and we can skip the transform call when these don't change.
    int               last_src_w_  = -1;
    int               last_src_h_  = -1;
};

} // namespace magnifier
