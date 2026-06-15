#pragma once

#include <Windows.h>

#include <string>

namespace magnifier {

// A small, fading "N.Nx" badge shown near the top of the active monitor when
// the zoom factor changes. It is a fully self-contained layered, click-through,
// topmost tool window — it does NOT touch the magnification pipeline, so it
// cannot destabilise the magnifier loop. All methods run on the UI thread.
class ZoomHud {
public:
    ZoomHud() = default;
    ~ZoomHud();

    ZoomHud(const ZoomHud&)            = delete;
    ZoomHud& operator=(const ZoomHud&) = delete;

    void SetEnabled(bool enabled) noexcept { enabled_ = enabled; }

    // Show (or refresh) the badge for `zoom` and reset the hold timer. Cheap
    // to call every tick while the zoom is changing — only re-creates the
    // window once and only re-shows it when hidden.
    void Flash(float zoom);

    // Advance the fade. Call once per app tick with the elapsed seconds.
    void Tick(float dt);

    // Hide immediately (e.g. when leaving a magnification mode).
    void Hide();

private:
    bool EnsureWindow_();
    void Reposition_();
    void Paint_(HDC dc, const RECT& rc);
    static LRESULT CALLBACK WndProc_(HWND, UINT, WPARAM, LPARAM);

    HWND         hwnd_    = nullptr;
    bool         enabled_ = true;
    bool         visible_ = false;
    float        hold_    = 0.0f;   // seconds left at full opacity
    float        alpha_   = 0.0f;   // 0..1 current opacity
    std::wstring text_;
};

} // namespace magnifier
