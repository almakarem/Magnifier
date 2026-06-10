#include "input/ControllerPoll.h"
#include "util/Log.h"

#include <Windows.h>
#include <Xinput.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>

#pragma comment(lib, "Xinput.lib")

namespace magnifier {

namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(8);   // ~125 Hz

float ApplyDeadzone(float v, float dz) {
    const float a = std::abs(v);
    if (a <= dz) return 0.0f;
    const float scaled = (a - dz) / (1.0f - dz);
    return std::copysign(std::clamp(scaled, 0.0f, 1.0f), v);
}

float ApplyCurve(float v, float exponent) {
    if (exponent <= 1.0f) return v;
    return std::copysign(std::pow(std::abs(v), exponent), v);
}

float Normalise(SHORT raw) {
    // XInput returns -32768..32767. Map to -1..1, clamping the asymmetric
    // negative range to avoid 1.00003 spikes.
    const float f = static_cast<float>(raw) / 32767.0f;
    return std::clamp(f, -1.0f, 1.0f);
}

unsigned MapXInputButton(std::string_view name) {
    if (name == "a")          return XINPUT_GAMEPAD_A;
    if (name == "b")          return XINPUT_GAMEPAD_B;
    if (name == "x")          return XINPUT_GAMEPAD_X;
    if (name == "y")          return XINPUT_GAMEPAD_Y;
    if (name == "lb")         return XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (name == "rb")         return XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (name == "back")       return XINPUT_GAMEPAD_BACK;
    if (name == "start")      return XINPUT_GAMEPAD_START;
    if (name == "ls")         return XINPUT_GAMEPAD_LEFT_THUMB;
    if (name == "rs")         return XINPUT_GAMEPAD_RIGHT_THUMB;
    if (name == "dpad_up")    return XINPUT_GAMEPAD_DPAD_UP;
    if (name == "dpad_down")  return XINPUT_GAMEPAD_DPAD_DOWN;
    if (name == "dpad_left")  return XINPUT_GAMEPAD_DPAD_LEFT;
    if (name == "dpad_right") return XINPUT_GAMEPAD_DPAD_RIGHT;
    return 0;
}

struct EdgeMap {
    unsigned mask;
    Action   action;
};

} // namespace

void ControllerPoll::SetConfig(const ControllerConfig& cfg) {
    std::scoped_lock lk(cfg_mu_);
    cfg_ = cfg;
    enabled_.store(cfg.enabled, std::memory_order_release);
}

void ControllerPoll::SetCallbacks(FrameSink frame, ActionSink action) {
    on_frame_  = std::move(frame);
    on_action_ = std::move(action);
}

void ControllerPoll::Start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) return;
    thread_ = std::thread(&ControllerPoll::PollLoop_, this);
}

void ControllerPoll::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;
    if (thread_.joinable()) thread_.join();
}

bool ControllerPoll::AnyControllerConnected() {
    XINPUT_STATE st{};
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (::XInputGetState(i, &st) == ERROR_SUCCESS) return true;
    }
    return false;
}

void ControllerPoll::PollLoop_() {
    unsigned last_buttons = 0;
    DWORD    last_packet  = 0;
    DWORD    active_slot  = 0;
    bool     have_active  = false;

    // Construct the modern (WGI) backend on this thread so its WinRT
    // apartment initialisation doesn't fight the UI thread (which is
    // typically STA — RPC_E_CHANGED_MODE if we MTA-init it). WGI supports
    // DualSense / DualShock 4 / Switch Pro / generic Bluetooth gamepads
    // that XInput is blind to.
    try {
        wgi_ = std::make_unique<WgiGamepad>();
    } catch (...) {
        spdlog::warn("ControllerPoll: WGI backend construction threw; "
                     "continuing with XInput-only.");
        wgi_.reset();
    }
    const bool wgi_ok = wgi_ && wgi_->Available();
    spdlog::info("ControllerPoll: backends = {}{}{}",
                 wgi_ok ? "WGI" : "",
                 wgi_ok ? " + " : "",
                 "XInput");

    while (running_.load(std::memory_order_acquire)) {
        if (!enabled_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        ControllerConfig cfg;
        { std::scoped_lock lk(cfg_mu_); cfg = cfg_; }

        ControllerFrame frame{};

        // --- Modern backend first ----------------------------------------
        // Why: covers DualSense / DualShock 4 / Switch Pro / many third-
        // party Bluetooth pads that XInput can't see.
        WgiGamepad::Reading wgi_r{};
        if (wgi_) wgi_r = wgi_->Read();
        if (wgi_r.present) {
            frame.present     = true;
            frame.ls_x        =  ApplyCurve(ApplyDeadzone(wgi_r.lx, cfg.deadzone), cfg.curve);
            frame.ls_y        = -ApplyCurve(ApplyDeadzone(wgi_r.ly, cfg.deadzone), cfg.curve);
            frame.rs_x        =  ApplyCurve(ApplyDeadzone(wgi_r.rx, cfg.deadzone), cfg.curve);
            frame.rs_y        = -ApplyCurve(ApplyDeadzone(wgi_r.ry, cfg.deadzone), cfg.curve);
            frame.lt          = wgi_r.lt;
            frame.rt          = wgi_r.rt;
            frame.buttons     = wgi_r.buttons;
            frame.backend     = "WGI";
            frame.device_name = wgi_r.device_name;

            // Edge-detect button-mapped actions.
            const EdgeMap edges[] = {
                {MapXInputButton(cfg.bindings.toggle_lens),       Action::ToggleLens},
                {MapXInputButton(cfg.bindings.toggle_fullscreen), Action::ToggleFullscreen},
                {MapXInputButton(cfg.bindings.turn_off),          Action::TurnOff},
                {MapXInputButton(cfg.bindings.recenter),          Action::Recenter},
            };
            const unsigned now = wgi_r.buttons;
            const unsigned pressed = (~last_buttons) & now;
            for (const auto& e : edges) {
                if (e.mask != 0 && (pressed & e.mask) && on_action_) {
                    on_action_(e.action);
                }
            }
            last_buttons = now;
            have_active  = false;       // XInput state irrelevant
        } else {
            // --- XInput fallback (covers Xbox controllers) ---------------
            XINPUT_STATE state{};
            bool got = false;
            if (have_active && ::XInputGetState(active_slot, &state) == ERROR_SUCCESS) {
                got = true;
            } else {
                for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
                    if (::XInputGetState(i, &state) == ERROR_SUCCESS) {
                        if (!have_active || active_slot != i) {
                            spdlog::info("XInput controller detected on slot {}", i);
                        }
                        active_slot = i;
                        have_active = true;
                        got = true;
                        break;
                    }
                }
            }

            if (got) {
                frame.present  = true;
                frame.ls_x =  ApplyCurve(ApplyDeadzone(Normalise(state.Gamepad.sThumbLX), cfg.deadzone), cfg.curve);
                frame.ls_y = -ApplyCurve(ApplyDeadzone(Normalise(state.Gamepad.sThumbLY), cfg.deadzone), cfg.curve);
                frame.rs_x =  ApplyCurve(ApplyDeadzone(Normalise(state.Gamepad.sThumbRX), cfg.deadzone), cfg.curve);
                frame.rs_y = -ApplyCurve(ApplyDeadzone(Normalise(state.Gamepad.sThumbRY), cfg.deadzone), cfg.curve);
                frame.lt   = static_cast<float>(state.Gamepad.bLeftTrigger)  / 255.0f;
                frame.rt   = static_cast<float>(state.Gamepad.bRightTrigger) / 255.0f;
                frame.buttons     = state.Gamepad.wButtons;
                frame.backend     = "XInput";
                frame.device_name = "XInput controller (slot " + std::to_string(active_slot) + ")";

                const EdgeMap edges[] = {
                    {MapXInputButton(cfg.bindings.toggle_lens),       Action::ToggleLens},
                    {MapXInputButton(cfg.bindings.toggle_fullscreen), Action::ToggleFullscreen},
                    {MapXInputButton(cfg.bindings.turn_off),          Action::TurnOff},
                    {MapXInputButton(cfg.bindings.recenter),          Action::Recenter},
                };
                const unsigned now = state.Gamepad.wButtons;
                const unsigned pressed = (~last_buttons) & now;
                for (const auto& e : edges) {
                    if (e.mask != 0 && (pressed & e.mask) && on_action_) {
                        on_action_(e.action);
                    }
                }
                last_buttons = now;
                last_packet  = state.dwPacketNumber;
            } else {
                if (have_active) {
                    spdlog::info("XInput controller disconnected (was on slot {})", active_slot);
                }
                have_active  = false;
                last_buttons = 0;
                last_packet  = 0;
            }
        }

        if (on_frame_) on_frame_(frame);
        std::this_thread::sleep_for(kPollInterval);
    }
}

} // namespace magnifier
