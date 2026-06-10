#pragma once

#include <cstdint>
#include <mutex>

namespace magnifier {

enum class MagMode : std::uint8_t {
    Off        = 0,
    Lens       = 1,
    Fullscreen = 2,
};

// Bounds the StateModel must clamp the lens / source rectangle to. In
// virtual-screen pixel space (so it works across monitors).
struct ScreenBounds {
    int left   = 0;
    int top    = 0;
    int right  = 1920;
    int bottom = 1080;

    constexpr int width()  const noexcept { return right  - left; }
    constexpr int height() const noexcept { return bottom - top;  }
};

struct LensSize {
    int width  = 640;
    int height = 360;
};

// The single source of truth for the magnifier's current and target state.
// Targets are set by inputs (mouse, hotkeys, controller, IPC) and the
// current values ease toward them on each tick. Thread-safe.
class StateModel {
public:
    struct Config {
        float position_tau = 0.05f;   // seconds; 0 disables easing
        float zoom_tau     = 0.10f;
        float zoom_min     = 1.0f;
        float zoom_max     = 16.0f;
    };

    StateModel() = default;
    explicit StateModel(Config cfg) : cfg_(cfg) {}

    void SetConfig(Config cfg);
    Config GetConfig() const;

    // ---- inputs ----------------------------------------------------------
    void SetMode(MagMode m);
    void SetBounds(ScreenBounds b);
    void SetLensSize(LensSize s);

    // Target center (in unmagnified virtual-screen pixels).
    void SetTargetCenter(float x, float y);
    void NudgeTargetCenter(float dx, float dy);

    // Target zoom factor (clamped to [zoom_min, zoom_max]).
    void SetTargetZoom(float z);
    void NudgeTargetZoom(float dz);

    // Snap current to target (no easing) — used on Recenter, mode toggles.
    void SnapToTarget();

    // Snap only the center (used by mouse-follow so the lens tracks the
    // cursor with zero latency while zoom continues to ease).
    void SnapCenterToTarget();

    // ---- per-tick update -------------------------------------------------
    // Advance the eased current values toward the targets. dt is in seconds.
    void Tick(float dt);

    // ---- read-only snapshot (used by MagController) ----------------------
    struct Snapshot {
        MagMode      mode    = MagMode::Off;
        float        zoom    = 1.0f;
        float        center_x = 0.0f;
        float        center_y = 0.0f;
        LensSize     lens    = {};
        ScreenBounds bounds  = {};
    };
    Snapshot GetSnapshot() const;

    // Pure helper: returns the eased value after applying exponential decay
    // with time-constant tau for elapsed dt. Public for unit testing.
    static float EaseStep(float current, float target, float dt, float tau) noexcept;

private:
    mutable std::mutex m_;
    Config cfg_{};
    MagMode mode_      = MagMode::Off;
    ScreenBounds bounds_{};
    LensSize lens_     = {};

    float zoom_target_ = 1.0f;
    float zoom_curr_   = 1.0f;
    float cx_target_   = 960.0f;
    float cy_target_   = 540.0f;
    float cx_curr_     = 960.0f;
    float cy_curr_     = 540.0f;

    void ClampTargets_();   // bounds-clamp center, range-clamp zoom
};

} // namespace magnifier
