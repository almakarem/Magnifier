#include "mag/StateModel.h"

#include <algorithm>
#include <cmath>

namespace magnifier {

void StateModel::SetConfig(Config cfg) {
    std::scoped_lock lk(m_);
    cfg_ = cfg;
    ClampTargets_();
}

StateModel::Config StateModel::GetConfig() const {
    std::scoped_lock lk(m_);
    return cfg_;
}

void StateModel::SetMode(MagMode m) {
    std::scoped_lock lk(m_);
    mode_ = m;
}

void StateModel::SetBounds(ScreenBounds b) {
    std::scoped_lock lk(m_);
    bounds_ = b;
    ClampTargets_();
}

void StateModel::SetLensSize(LensSize s) {
    std::scoped_lock lk(m_);
    lens_ = s;
}

void StateModel::SetTargetCenter(float x, float y) {
    std::scoped_lock lk(m_);
    cx_target_ = x;
    cy_target_ = y;
    ClampTargets_();
}

void StateModel::NudgeTargetCenter(float dx, float dy) {
    std::scoped_lock lk(m_);
    cx_target_ += dx;
    cy_target_ += dy;
    ClampTargets_();
}

void StateModel::SetTargetZoom(float z) {
    std::scoped_lock lk(m_);
    zoom_target_ = z;
    ClampTargets_();
}

void StateModel::NudgeTargetZoom(float dz) {
    std::scoped_lock lk(m_);
    zoom_target_ += dz;
    ClampTargets_();
}

void StateModel::SnapToTarget() {
    std::scoped_lock lk(m_);
    cx_curr_   = cx_target_;
    cy_curr_   = cy_target_;
    zoom_curr_ = zoom_target_;
}

void StateModel::SnapCenterToTarget() {
    std::scoped_lock lk(m_);
    cx_curr_ = cx_target_;
    cy_curr_ = cy_target_;
}

void StateModel::Tick(float dt) {
    if (dt <= 0.0f) return;
    std::scoped_lock lk(m_);
    cx_curr_   = EaseStep(cx_curr_,   cx_target_,   dt, cfg_.position_tau);
    cy_curr_   = EaseStep(cy_curr_,   cy_target_,   dt, cfg_.position_tau);
    zoom_curr_ = EaseStep(zoom_curr_, zoom_target_, dt, cfg_.zoom_tau);
}

StateModel::Snapshot StateModel::GetSnapshot() const {
    std::scoped_lock lk(m_);
    Snapshot s;
    s.mode     = mode_;
    s.zoom     = zoom_curr_;
    s.center_x = cx_curr_;
    s.center_y = cy_curr_;
    s.lens     = lens_;
    s.bounds   = bounds_;
    return s;
}

float StateModel::EaseStep(float current, float target, float dt, float tau) noexcept {
    if (tau <= 0.0f || dt < 0.0f) return target;     // snap
    // Frame-rate-independent exponential decay:
    //   current += (target - current) * (1 - exp(-dt / tau))
    const float alpha = 1.0f - std::exp(-dt / tau);
    return current + (target - current) * alpha;
}

void StateModel::ClampTargets_() {
    zoom_target_ = std::clamp(zoom_target_, cfg_.zoom_min, cfg_.zoom_max);

    // Only constrain the *center* to lie inside the virtual-screen bounds.
    // Older versions also kept the entire source rectangle inside bounds,
    // which made it impossible to reach the corners of the outer monitors
    // on multi-display setups — the cursor would be pinned half-a-source-
    // rect-width away from each edge. The Magnification API tolerates a
    // source rect that extends past the desktop (those pixels just render
    // black), which is far less surprising than a cursor that can't reach
    // the corner of its own screen.
    cx_target_ = std::clamp(cx_target_,
                            static_cast<float>(bounds_.left),
                            static_cast<float>(bounds_.right));
    cy_target_ = std::clamp(cy_target_,
                            static_cast<float>(bounds_.top),
                            static_cast<float>(bounds_.bottom));
}

} // namespace magnifier
