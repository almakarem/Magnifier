#include "mag/StateModel.h"

#include <gtest/gtest.h>

#include <cmath>

using namespace magnifier;

TEST(StateModelEaseStep, SnapWhenTauZero) {
    EXPECT_FLOAT_EQ(10.0f, StateModel::EaseStep(0.0f, 10.0f, 0.016f, 0.0f));
    EXPECT_FLOAT_EQ(-5.0f, StateModel::EaseStep(100.0f, -5.0f, 0.001f, 0.0f));
}

TEST(StateModelEaseStep, ConvergesToTarget) {
    float cur = 0.0f;
    const float target = 100.0f;
    for (int i = 0; i < 1000; ++i) {
        cur = StateModel::EaseStep(cur, target, 0.016f, 0.05f);
    }
    EXPECT_NEAR(cur, target, 0.001f);
}

TEST(StateModelEaseStep, FrameRateIndependent) {
    // Two different dt schedules over the same total time should arrive at
    // approximately the same value (true for the exact exponential formula).
    auto run = [](float dt, int steps) {
        float v = 0.0f;
        for (int i = 0; i < steps; ++i) {
            v = StateModel::EaseStep(v, 1.0f, dt, 0.1f);
        }
        return v;
    };
    const float v_a = run(0.010f, 100);    // 1.0 s total
    const float v_b = run(0.020f,  50);    // 1.0 s total
    EXPECT_NEAR(v_a, v_b, 1e-5f);
}

TEST(StateModel, ZoomTargetClampedToConfigRange) {
    StateModel sm({/*pos_tau*/ 0.0f, /*zoom_tau*/ 0.0f, /*min*/ 1.5f, /*max*/ 4.0f});
    sm.SetTargetZoom(0.5f);
    sm.SnapToTarget();
    EXPECT_FLOAT_EQ(sm.GetSnapshot().zoom, 1.5f);
    sm.SetTargetZoom(99.0f);
    sm.SnapToTarget();
    EXPECT_FLOAT_EQ(sm.GetSnapshot().zoom, 4.0f);
}

TEST(StateModel, CenterClampedToBounds) {
    // The lens center must be reachable across the entire virtual desktop,
    // including the extreme corners, even at high zoom. Source rects that
    // poke past a screen edge are acceptable — the Magnification API just
    // renders black there.
    StateModel sm({/*pos_tau*/ 0.0f, /*zoom_tau*/ 0.0f, 1.0f, 16.0f});
    ScreenBounds b{0, 0, 1000, 1000};
    sm.SetBounds(b);
    sm.SetTargetZoom(4.0f);
    sm.SetTargetCenter(0.0f, 0.0f);
    sm.SnapToTarget();
    auto snap = sm.GetSnapshot();
    EXPECT_FLOAT_EQ(snap.center_x, 0.0f);
    EXPECT_FLOAT_EQ(snap.center_y, 0.0f);

    // Beyond the right/bottom edge — clamped to the edge, not pushed inward.
    sm.SetTargetCenter(99999.0f, 99999.0f);
    sm.SnapToTarget();
    snap = sm.GetSnapshot();
    EXPECT_FLOAT_EQ(snap.center_x, 1000.0f);
    EXPECT_FLOAT_EQ(snap.center_y, 1000.0f);
}

TEST(StateModel, NegativeDtDoesNotCrash) {
    StateModel sm({0.05f, 0.10f, 1.0f, 4.0f});
    sm.SetBounds({0, 0, 1920, 1080});
    sm.SetTargetCenter(960.0f, 540.0f);
    sm.Tick(-1.0f);   // should be a no-op
    SUCCEED();
}
