#include "app/App.h"
#include "util/Log.h"
#include "util/Paths.h"
#include "util/StringConv.h"
#include "util/WinError.h"

#include <Version.h>

#include <nlohmann/json.hpp>

#include <Windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

#pragma comment(lib, "Dwmapi.lib")

namespace fs = std::filesystem;

namespace magnifier {

namespace {

constexpr wchar_t kAppClassName[] = L"MagnifierAppWnd_v1";
constexpr UINT    WM_APP_ACTION   = WM_APP + 10;
constexpr UINT    WM_APP_CMD      = WM_APP + 11;
constexpr UINT    WM_APP_FRAME    = WM_APP + 12;
constexpr UINT    WM_APP_UPDATE   = WM_APP + 13;  // updater finished (check or dl)

// Fallback tick period (ms) used when we cannot query the panel refresh
// (e.g. headless WinPE / RDP session with no display). 8 ms = 125 Hz is a
// safe middle-ground for any modern panel.
constexpr int     kFallbackTickPeriodMs   = 8;
// Throttle the ImGui settings window render to roughly display refresh so
// we don't burn CPU/GPU at 250 Hz when the user is just hovering a slider.
constexpr int     kSettingsRenderPeriodMs = 16;

} // namespace

App::App() = default;
App::~App() {
    RemovePidFile_();
    controller_.Stop();
    ipc_.Stop();
    hotkeys_.Shutdown();
    settings_.Hide();
    tray_.Shutdown();
    mag_.Shutdown();
    if (tick_event_) {
        ::CancelWaitableTimer(tick_event_);
        ::CloseHandle(tick_event_);
        tick_event_ = nullptr;
    }
    if (app_wnd_) ::DestroyWindow(app_wnd_);
}

bool App::Initialise(HINSTANCE hinst, const AppOptions& opts) {
    hinst_       = hinst;
    config_path_ = ConfigFilePath();

    // ---- config ----------------------------------------------------------
    auto loaded = LoadConfig(config_path_);
    cfg_        = std::move(loaded.config);
    first_run_  = !loaded.loaded_from_file;
    for (const auto& w : loaded.warnings) spdlog::warn("config: {}", w);

    log::Init(cfg_.advanced.log_level);
    spdlog::info("Loaded config from {}", WideToUtf8(config_path_.wstring()));

    // ---- UI message-only window -----------------------------------------
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &App::AppWndProc_;
    wc.hInstance     = hinst;
    wc.lpszClassName = kAppClassName;
    if (!::RegisterClassExW(&wc)) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            spdlog::error("App: RegisterClassExW failed: {}", LastErrorString(err));
            return false;
        }
    }
    app_wnd_ = ::CreateWindowExW(0, kAppClassName, L"MagnifierApp",
        0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hinst, this);
    if (!app_wnd_) {
        spdlog::error("App: CreateWindowExW failed: {}", LastErrorString());
        return false;
    }
    ::SetWindowLongPtrW(app_wnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    // ---- magnifier --------------------------------------------------------
    if (!mag_.Initialise(hinst, &state_)) {
        spdlog::error("Magnification API failed to initialise.");
        // Continue — tray + IPC still work; user gets a clear error.
    }
    mag_.SetMagnifyCursor(cfg_.lens.magnify_cursor);

    // Apply initial state model config.
    StateModel::Config sm_cfg{};
    sm_cfg.position_tau = cfg_.lens.position_tau;
    sm_cfg.zoom_tau     = cfg_.lens.zoom_tau;
    sm_cfg.zoom_min     = cfg_.zoom.min;
    sm_cfg.zoom_max     = cfg_.zoom.max;
    state_.SetConfig(sm_cfg);
    state_.SetLensSize({cfg_.lens.width, cfg_.lens.height});
    state_.SetTargetZoom(cfg_.zoom.initial);
    state_.SnapToTarget();

    // ---- router ----------------------------------------------------------
    router_ = std::make_unique<InputRouter>(*this, state_, cfg_.controller);

    // ---- hotkeys ---------------------------------------------------------
    hotkeys_.Initialise(hinst, [this](Action a) {
        // Already on UI thread (WM_HOTKEY).
        router_->OnAction(a);
    });
    ApplyHotkeys_();
    hotkeys_.SetLowLevelHook(cfg_.advanced.low_level_keyboard_hook);

    // ---- controller -------------------------------------------------------
    controller_.SetConfig(cfg_.controller);
    controller_.SetCallbacks(
        [this](const ControllerFrame& f) {
            {
                std::scoped_lock lk(frame_mu_);
                latest_frame_  = f;
                frame_pending_ = true;
            }
            if (app_wnd_) ::PostMessageW(app_wnd_, WM_APP_FRAME, 0, 0);
        },
        [this](Action a) {
            {
                std::scoped_lock lk(action_mu_);
                action_queue_.push(a);
            }
            if (app_wnd_) ::PostMessageW(app_wnd_, WM_APP_ACTION, 0, 0);
        });
    if (cfg_.controller.enabled) {
        controller_.Start();
        controller_running_ = true;
    }

    // ---- IPC --------------------------------------------------------------
    ipc_.Start(hinst, cfg_.ipc.pipe_name, cfg_.ipc.http_port,
        [this](const Command& c) -> std::string {
            return OnIpcCommand(c);
        });

    // ---- tray -------------------------------------------------------------
    tray_.Initialise(hinst, [this](Action a) {
        router_->OnAction(a);
    });
    // Real tooltip text is set by RefreshTrayTooltip_() once the hotkeys
    // table is fully populated (just below, after the welcome toast block).

    // ---- settings window --------------------------------------------------
    settings_.SetConfig(cfg_);
    settings_.SetApplySink([this](const Config& new_cfg) { ApplyConfig_(new_cfg); });
    settings_.SetUpdateController(
        /*check*/ [this] { StartUpdateCheck_(true); },
        /*download*/ [this] { StartUpdateDownload_(); },
        /*install*/ [this] { StartUpdateInstall_(); });
    settings_.SetUpdateStatus(update_status_);

    // ---- updater ----------------------------------------------------------
    ApplyUpdateSettings_();
    if (cfg_.update.check_on_startup && !cfg_.update.owner.empty()) {
        StartUpdateCheck_(false);
    }

    // ---- tick timer (high-resolution waitable timer) ---------------------
    // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION (Win10 1803+) gives us sub-ms
    // accuracy independent of the system scheduler tick. Auto-reset so
    // each MsgWaitForMultipleObjectsEx wait consumes exactly one fire.
    // We arm it with RefreshTickRate_() so the period matches the actual
    // monitor refresh - on a 60 Hz panel we tick every ~16 ms instead of
    // 4 ms, which eliminates the low-zoom ghosting users reported on
    // high-refresh displays (the previous fixed 4 ms cadence drifted
    // against DWM's vsync, causing the mag control to be resampled mid-
    // frame).
    tick_event_ = ::CreateWaitableTimerExW(
        nullptr, nullptr,
        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (!tick_event_) {
        // Fallback: ordinary waitable timer (~ms resolution, still better
        // than coalesced WM_TIMER).
        tick_event_ = ::CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    RefreshTickRate_();
    if (!tick_event_) {
        spdlog::warn("CreateWaitableTimerExW failed; falling back to WM_TIMER");
    }
    last_tick_  = std::chrono::steady_clock::now();
    last_settings_render_ = last_tick_;

    // ---- pid file ---------------------------------------------------------
    WritePidFile_();

    // ---- startup command --------------------------------------------------
    if (opts.startup_command) {
        // Apply locally - we are the running instance.
        OnIpcCommand(*opts.startup_command);
    }
    (void)opts.start_minimized;   // tray-only by default; nothing to suppress.

    // ---- tray hover summary + first-run welcome toast --------------------
    // Tooltip mirrors the currently-bound hotkeys so a quick hover answers
    // "what's the toggle key again?" without opening Settings. On a fresh
    // install (no config.toml present yet) we also fire a one-shot tray
    // balloon naming the two most important shortcuts; persisting that
    // first-run flag is implicit in the act of writing the default config
    // file, so subsequent launches stay quiet.
    RefreshTrayTooltip_();
    if (first_run_) {
        std::wstring toggle = L"(unbound)";
        std::wstring settings = L"(unbound)";
        if (auto it = cfg_.hotkeys.find(Action::ToggleLens);
            it != cfg_.hotkeys.end() && it->second.is_bound()) {
            toggle = Utf8ToWide(it->second.to_human());
        }
        if (auto it = cfg_.hotkeys.find(Action::ShowSettings);
            it != cfg_.hotkeys.end() && it->second.is_bound()) {
            settings = Utf8ToWide(it->second.to_human());
        }
        std::wstring body =
            L"Toggle lens:  " + toggle + L"\r\n" +
            L"Settings:     " + settings + L"\r\n" +
            L"Right-click the tray icon for the full menu.";
        tray_.ShowBalloon(L"Magnifier is running", body);
    }

    spdlog::info("Magnifier ready.");
    return true;
}

int App::Run() {
    // Main loop pumps two event sources: the high-resolution waitable timer
    // (drives the magnifier at ~250 Hz) and the window-message queue. We do
    // NOT use WM_TIMER — it's the lowest-priority message and Windows
    // coalesces queued instances, which produced the visible cursor-trail
    // ghosting users reported.
    HANDLE handles[1] = { tick_event_ };
    const DWORD handle_count = tick_event_ ? 1u : 0u;

    while (!quit_requested_.load(std::memory_order_acquire)) {
        // Drain every pending message first so input always wins races
        // against the tick — critical for hotkey/IPC responsiveness.
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit_requested_.store(true, std::memory_order_release);
                break;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (quit_requested_.load(std::memory_order_acquire)) break;

        // Wait for the next timer fire OR new input. INFINITE is safe
        // because the waitable timer is periodic so we always wake up.
        const DWORD r = ::MsgWaitForMultipleObjectsEx(
            handle_count, handles,
            handle_count ? INFINITE : 16u,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);

        if (handle_count && r == WAIT_OBJECT_0) {
            OnTick_();
        } else if (r == WAIT_TIMEOUT) {
            // Fallback path when CreateWaitableTimerExW failed at startup.
            OnTick_();
        }
        // WAIT_OBJECT_0 + handle_count means messages woke us; they'll be
        // drained at the top of the next iteration.
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Window proc — drives the timer and pumps the cross-thread queues.
// ----------------------------------------------------------------------------
LRESULT CALLBACK App::AppWndProc_(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<App*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return ::DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_APP_ACTION: {
            std::queue<Action> drained;
            {
                std::scoped_lock lk(self->action_mu_);
                std::swap(drained, self->action_queue_);
            }
            while (!drained.empty()) {
                self->router_->OnAction(drained.front());
                drained.pop();
            }
            return 0;
        }
        case WM_APP_CMD: {
            std::queue<Command> drained;
            {
                std::scoped_lock lk(self->cmd_mu_);
                std::swap(drained, self->cmd_queue_);
            }
            while (!drained.empty()) {
                self->OnIpcCommand(drained.front());
                drained.pop();
            }
            return 0;
        }
        case WM_APP_FRAME: {
            ControllerFrame frame;
            bool have = false;
            {
                std::scoped_lock lk(self->frame_mu_);
                if (self->frame_pending_) {
                    frame = self->latest_frame_;
                    have = true;
                    self->frame_pending_ = false;
                }
            }
            if (have) {
                const auto now = std::chrono::steady_clock::now();
                const float dt = std::chrono::duration<float>(
                    now - self->last_tick_).count();
                self->router_->OnControllerFrame(frame, dt);
                // Mirror into the Settings -> Diagnostics tab. Cheap; only
                // copies a tiny struct under a tiny mutex.
                self->settings_.SetDiagnostics(frame);
                self->settings_.SetWgiProbe(frame.wgi_gamepad_count,
                                            frame.wgi_raw_count,
                                            frame.wgi_first_raw_name);
            }
            return 0;
        }
        case WM_APP_UPDATE: {
            // Refresh the Settings window so the Updates tab reflects the
            // latest status (worker thread already wrote into update_status_
            // before posting this message).
            self->settings_.SetUpdateStatus(self->update_status_);
            // Show a tray balloon the first time a new version is detected
            // (wp == 1 from the background check callback).
            if (wp == 1 && self->update_status_.update_available &&
                self->update_status_.info) {
                std::wstring title = L"Magnifier update available";
                std::wstring text  = L"Version " + Utf8ToWide(self->update_status_.info->version) +
                                     L" is available. Open Settings \u2192 Updates to install.";
                self->tray_.ShowBalloon(title, text);
            }
            // Auto-download is opt-in and must run on the UI thread.
            if (wp == 1 && self->cfg_.update.auto_download &&
                self->update_status_.ok && self->update_status_.update_available &&
                self->update_status_.info &&
                !self->update_status_.info->msi_asset_url.empty() &&
                !self->update_downloading_.load() &&
                self->update_downloaded_msi_.empty()) {
                self->StartUpdateDownload_();
            }
            return 0;
        }

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            self->OnDisplayChange_();
            return 0;

        case WM_POWERBROADCAST:
            self->OnPowerEvent_(wp);
            return TRUE;

        case WM_DESTROY:
            self->quit_requested_.store(true, std::memory_order_release);
            ::PostQuitMessage(0);
            return 0;

        default:
            return ::DefWindowProcW(hwnd, msg, wp, lp);
    }
}

void App::OnTick_() {
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - last_tick_).count();
    last_tick_ = now;

    UpdateMouseFollow_();
    state_.Tick(dt);
    mag_.Tick();

    // Pin the next tick to the panel's vblank. Without this, our timer
    // period (e.g. 4 ms on a 240 Hz panel) drifts against DWM's actual
    // composition cadence, so every few seconds we feed MagSetWindowSource
    // a new rectangle in the middle of a scan-out and the user sees a
    // smeared ghost trail behind fast cursor moves. DwmFlush blocks until
    // the next DWM composition starts; cheap, well-supported on Win10+,
    // and returns immediately if DWM is unavailable. Skipping it when no
    // magnification mode is active avoids the (small) wakeup cost while
    // the app is idle in the tray.
    if (state_.GetSnapshot().mode != MagMode::Off) {
        ::DwmFlush();
    }

    if (settings_.IsVisible()) {
        // Throttle the (relatively heavy) ImGui+D3D11 render to ~60 Hz
        // independent of the magnifier tick rate.
        if (now - last_settings_render_ >=
                std::chrono::milliseconds(kSettingsRenderPeriodMs)) {
            settings_.Render();
            last_settings_render_ = now;
        }
    }
}

void App::OnDisplayChange_() {
    // Re-seed bounds from the current virtual desktop dimensions.
    ScreenBounds b;
    b.left   = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    b.top    = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    b.right  = b.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    b.bottom = b.top  + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    state_.SetBounds(b);
    spdlog::info("Display change: virtual screen now {}x{}", b.width(), b.height());
    // A monitor was added / removed / changed mode - re-pick the right
    // refresh rate. Cheap (one EnumDisplaySettings call).
    RefreshTickRate_();
}

void App::OnPowerEvent_(WPARAM event) {
    if (event == PBT_APMSUSPEND) {
        spdlog::info("System suspending — pausing controller poll.");
        if (controller_running_) controller_.Stop();
    } else if (event == PBT_APMRESUMEAUTOMATIC || event == PBT_APMRESUMESUSPEND) {
        spdlog::info("System resumed.");
        if (controller_running_) controller_.Start();
    }
}

// ----------------------------------------------------------------------------
// Mode helpers
// ----------------------------------------------------------------------------
void App::SetMode(MagMode mode) {
    spdlog::info("Mode -> {}",
        mode == MagMode::Off ? "Off" :
        mode == MagMode::Lens ? "Lens" : "Fullscreen");

    mag_.SetMode(mode);
    // Plain arrow keys pan the lens — but ONLY while lens mode is active,
    // so we don't hijack typing in other apps. The permanent ctrl+alt+arrow
    // bindings (registered via ApplyBindings) remain always-on.
    hotkeys_.SetTransientPanKeys(mode == MagMode::Lens);
    if (mode != MagMode::Off) {
        last_mode_ = mode;
        ::SetPriorityClass(::GetCurrentProcess(),
            cfg_.general.active_priority == "above_normal"
                ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
    } else {
        ::SetPriorityClass(::GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
    }

    RefreshTrayTooltip_();
}

void App::ToggleMode(MagMode mode) {
    const auto curr = state_.GetSnapshot().mode;
    SetMode(curr == mode ? MagMode::Off : mode);
}

void App::RecenterOnCursor() {
    POINT pt{};
    if (::GetCursorPos(&pt)) {
        state_.SetTargetCenter(static_cast<float>(pt.x), static_cast<float>(pt.y));
        state_.SnapToTarget();
    }
}

void App::ResizeLens(int dw, int dh) {
    cfg_.lens.width  = std::clamp(cfg_.lens.width  + dw, 160, 3840);
    cfg_.lens.height = std::clamp(cfg_.lens.height + dh,  90, 2160);
    state_.SetLensSize({cfg_.lens.width, cfg_.lens.height});
}

void App::JumpToNextMonitor() {
    // Simple impl: cycle through monitors via EnumDisplayMonitors and snap
    // the lens center to the next one.
    struct Ctx { std::vector<RECT> rects; } ctx;
    ::EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR, HDC, LPRECT r, LPARAM lp) -> BOOL {
            reinterpret_cast<Ctx*>(lp)->rects.push_back(*r);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.rects.empty()) return;

    const auto snap = state_.GetSnapshot();
    int idx = 0;
    for (size_t i = 0; i < ctx.rects.size(); ++i) {
        const auto& r = ctx.rects[i];
        if (snap.center_x >= r.left && snap.center_x < r.right &&
            snap.center_y >= r.top  && snap.center_y < r.bottom) {
            idx = static_cast<int>(i);
            break;
        }
    }
    idx = (idx + 1) % static_cast<int>(ctx.rects.size());
    const auto& r = ctx.rects[idx];
    state_.SetTargetCenter(
        static_cast<float>((r.left + r.right)  / 2),
        static_cast<float>((r.top  + r.bottom) / 2));
    state_.SnapToTarget();
}

void App::SetControllerEnabled(bool enabled) {
    cfg_.controller.enabled = enabled;
    if (enabled && !controller_running_) {
        controller_.Start();
        controller_running_ = true;
    } else if (!enabled && controller_running_) {
        controller_.Stop();
        controller_running_ = false;
    }
}

void App::ReloadConfig() {
    spdlog::info("Reloading config from {}", WideToUtf8(config_path_.wstring()));
    auto loaded = LoadConfig(config_path_);
    for (const auto& w : loaded.warnings) spdlog::warn("config: {}", w);
    ApplyConfig_(loaded.config);
}

void App::ShowSettings() { settings_.SetConfig(cfg_); settings_.Show(); }
void App::HideSettings() { settings_.Hide(); }

void App::RequestQuit(bool force) {
    spdlog::info("Quit requested (force={}).", force);
    quit_requested_.store(true, std::memory_order_release);
    if (force) {
        // Skip orderly teardown.
        ::TerminateProcess(::GetCurrentProcess(), 0);
    } else {
        ::PostMessageW(app_wnd_, WM_DESTROY, 0, 0);
    }
}

// ----------------------------------------------------------------------------
// IPC command dispatch
// ----------------------------------------------------------------------------
std::string App::OnIpcCommand(const Command& c) {
    using nlohmann::json;
    // If we're not on the UI thread, marshal.
    const DWORD wnd_tid = app_wnd_ ? ::GetWindowThreadProcessId(app_wnd_, nullptr) : 0;
    if (wnd_tid && wnd_tid != ::GetCurrentThreadId()) {
        {
            std::scoped_lock lk(cmd_mu_);
            cmd_queue_.push(c);
        }
        ::PostMessageW(app_wnd_, WM_APP_CMD, 0, 0);
        return R"({"ok":true,"queued":true})";
    }

    switch (c.kind) {
        case CmdKind::EnterLens:          SetMode(MagMode::Lens);       break;
        case CmdKind::EnterFullscreen:    SetMode(MagMode::Fullscreen); break;
        case CmdKind::TurnOff:            SetMode(MagMode::Off);        break;
        case CmdKind::Toggle:             ToggleMode(last_mode_);       break;
        case CmdKind::SetZoom:
            if (c.f_value) state_.SetTargetZoom(*c.f_value);
            break;
        case CmdKind::ZoomDelta:
            if (c.f_value) state_.NudgeTargetZoom(*c.f_value);
            break;
        case CmdKind::SetLensSize:
            if (c.i_value && c.i_value2) {
                cfg_.lens.width  = *c.i_value;
                cfg_.lens.height = *c.i_value2;
                state_.SetLensSize({cfg_.lens.width, cfg_.lens.height});
            }
            break;
        case CmdKind::SetMonitorIndex:
            JumpToNextMonitor();
            break;
        case CmdKind::ReloadConfig:       ReloadConfig();               break;
        case CmdKind::SwitchProfile:      /* TODO: profile support */   break;
        case CmdKind::EnableController:   SetControllerEnabled(true);   break;
        case CmdKind::DisableController:  SetControllerEnabled(false);  break;
        case CmdKind::ShowSettings:       ShowSettings();               break;
        case CmdKind::HideSettings:       HideSettings();               break;
        case CmdKind::GetStatus:          return BuildStatusJson_();
        case CmdKind::Quit:               RequestQuit(false);           break;
        case CmdKind::ForceQuit:          RequestQuit(true);            break;
        case CmdKind::Noop:               break;
    }
    return R"({"ok":true})";
}

// ----------------------------------------------------------------------------
// Apply helpers
// ----------------------------------------------------------------------------
void App::ApplyConfig_(const Config& cfg) {
    const bool controller_changed =
        cfg.controller.enabled != cfg_.controller.enabled;
    const bool ll_hook_changed =
        cfg.advanced.low_level_keyboard_hook != cfg_.advanced.low_level_keyboard_hook;
    const bool hotkeys_changed = cfg.hotkeys != cfg_.hotkeys;

    cfg_ = cfg;

    StateModel::Config sm{};
    sm.position_tau = cfg_.lens.position_tau;
    sm.zoom_tau     = cfg_.lens.zoom_tau;
    sm.zoom_min     = cfg_.zoom.min;
    sm.zoom_max     = cfg_.zoom.max;
    state_.SetConfig(sm);
    state_.SetLensSize({cfg_.lens.width, cfg_.lens.height});
    mag_.SetMagnifyCursor(cfg_.lens.magnify_cursor);

    if (hotkeys_changed) ApplyHotkeys_();
    if (ll_hook_changed) hotkeys_.SetLowLevelHook(cfg_.advanced.low_level_keyboard_hook);
    if (controller_changed) SetControllerEnabled(cfg_.controller.enabled);
    controller_.SetConfig(cfg_.controller);
    router_->SetControllerConfig(cfg_.controller);

    ApplyUpdateSettings_();

    SaveConfig(config_path_, cfg_);
    // Re-bound hotkeys -> tooltip text changes too.
    RefreshTrayTooltip_();
}

void App::ApplyHotkeys_() {
    auto conflicts = hotkeys_.ApplyBindings(cfg_.hotkeys);
    for (const auto& c : conflicts) {
        spdlog::warn("Hotkey conflict: action={} binding={} ({})",
            std::string(ToString(c.action)),
            c.binding.to_human(), c.reason);
    }
}

void App::UpdateMouseFollow_() {
    if (!cfg_.lens.follow_mouse) return;
    if (state_.GetSnapshot().mode == MagMode::Off) return;
    if (router_ && router_->ControllerOwnsCursor()) return;

    POINT pt{};
    if (::GetCursorPos(&pt)) {
        state_.SetTargetCenter(static_cast<float>(pt.x), static_cast<float>(pt.y));
        // Bypass position easing for the mouse: any lag here is visible as
        // a ghost trail behind fast cursor moves. Zoom still eases via the
        // normal Tick() path because that's animation, not tracking.
        state_.SnapCenterToTarget();
    }
}

void App::WritePidFile_() const {
    std::ofstream out(PidFilePath(), std::ios::binary | std::ios::trunc);
    if (out) out << ::GetCurrentProcessId();
}

void App::RemovePidFile_() const {
    std::error_code ec;
    fs::remove(PidFilePath(), ec);
}

std::string App::BuildStatusJson_() const {
    using nlohmann::json;
    json j;
    j["ok"]      = true;
    j["version"] = kVersionString;
    j["pid"]     = static_cast<int>(::GetCurrentProcessId());

    const auto s = state_.GetSnapshot();
    j["mode"]   = s.mode == MagMode::Off ? "off"
                : s.mode == MagMode::Lens ? "lens" : "fullscreen";
    j["zoom"]   = s.zoom;
    j["center"] = json::array({s.center_x, s.center_y});
    j["lens"]   = json::object({{"w", s.lens.width}, {"h", s.lens.height}});
    j["controller_enabled"] = cfg_.controller.enabled;
    j["controller_present"] = ControllerPoll::AnyControllerConnected();
    j["capture_exclusion_supported"] = MagController::SupportsCaptureExclusion();
    return j.dump();
}

// ----------------------------------------------------------------------------
// Updater plumbing
// ----------------------------------------------------------------------------
void App::ApplyUpdateSettings_() {
    Updater::Settings s;
    s.enabled         = cfg_.update.check_on_startup || !cfg_.update.owner.empty();
    s.auto_download   = cfg_.update.auto_download;
    s.owner           = cfg_.update.owner;
    s.repo            = cfg_.update.repo;
    s.token           = cfg_.update.token;
    s.current_version = kVersionString;
    updater_.SetSettings(s);
}

void App::StartUpdateCheck_(bool from_user) {
    if (cfg_.update.owner.empty()) {
        spdlog::info("Updater: owner not configured; skipping check.");
        update_status_ = {};
        update_status_.error           = "Configure GitHub owner/repo in Settings \xe2\x86\x92 Updates.";
        update_status_.current_version = kVersionString;
        if (app_wnd_) ::PostMessageW(app_wnd_, WM_APP_UPDATE, 0, 0);
        return;
    }
    spdlog::info("Updater: checking (from_user={})", from_user);
    updater_.CheckAsync([this, from_user](UpdateCheckResult res) {
        // Worker thread — marshal to UI by storing + posting. The UI
        // handler reads update_status_ after PeekMessage drains us, so the
        // happens-before edge across the PostMessage call is enough.
        update_status_ = std::move(res);
        if (app_wnd_) {
            // wp = 1 for background checks (triggers tray balloon + maybe
            // auto-download); wp = 0 for user-initiated checks (just refresh
            // the Settings UI).
            ::PostMessageW(app_wnd_, WM_APP_UPDATE, from_user ? 0 : 1, 0);
        }
    });
}

void App::StartUpdateDownload_() {
    if (!update_status_.ok || !update_status_.info ||
        update_status_.info->msi_asset_url.empty()) return;
    if (update_downloading_.exchange(true)) return;
    update_dl_bytes_.store(0);
    update_dl_total_.store(update_status_.info->msi_asset_size);

    const auto dest_dir = (LocalAppDataDir() / L"updates").wstring();
    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);

    UpdateInfo info = *update_status_.info;
    updater_.DownloadMsiAsync(std::move(info), dest_dir,
        [this](bool ok, std::wstring path, std::string err) {
            update_downloading_.store(false);
            if (ok) {
                update_downloaded_msi_ = std::move(path);
                spdlog::info("Updater: download complete: {}",
                             WideToUtf8(update_downloaded_msi_));
            } else {
                spdlog::warn("Updater: download failed: {}", err);
                update_status_.error = "Download failed: " + err;
            }
            if (app_wnd_) ::PostMessageW(app_wnd_, WM_APP_UPDATE, 0, 0);
        },
        [this](int64_t got, int64_t total) {
            update_dl_bytes_.store(got);
            if (total > 0) update_dl_total_.store(total);
            // Don't post on every chunk — the UI re-reads atomics each frame.
        });
}

void App::StartUpdateInstall_() {
    if (update_downloaded_msi_.empty()) return;
    spdlog::info("Updater: launching installer {}", WideToUtf8(update_downloaded_msi_));
    // Launch msiexec with a visible passive progress bar so the user sees
    // *something* happening, then quit. The installer will MajorUpgrade us.
    if (Updater::LaunchInstaller(update_downloaded_msi_, /*silent*/ true)) {
        RequestQuit(/*force*/ false);
    } else {
        update_status_.error = "Failed to launch msiexec.";
        if (app_wnd_) ::PostMessageW(app_wnd_, WM_APP_UPDATE, 0, 0);
    }
}

// ----------------------------------------------------------------------------
// Tick-rate auto-tuning
// ----------------------------------------------------------------------------
// Resolves the refresh rate of the monitor under the lens centre (or, when
// the lens is idle, under the cursor) and re-arms the high-resolution
// waitable timer so we tick once per panel refresh. Older builds used a
// fixed 4 ms period which over-fed the magnification control on 60/120 Hz
// panels and drifted against DWM's vsync on 240 Hz, producing the low-zoom
// ghost trails users reported. Combined with DwmFlush() in OnTick_ this
// pins the magnifier loop to the panel's scan-out cadence.
void App::RefreshTickRate_() {
    if (!tick_event_) return;

    int hz = 0;
    POINT pt{};
    HMONITOR mon = nullptr;
    if (::GetCursorPos(&pt)) {
        mon = ::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    }
    if (!mon) mon = ::MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
    if (mon) {
        MONITORINFOEXW mi{};
        mi.cbSize = sizeof(mi);
        if (::GetMonitorInfoW(mon, &mi)) {
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (::EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
                dm.dmDisplayFrequency > 1) {
                hz = static_cast<int>(dm.dmDisplayFrequency);
            }
        }
    }
    if (hz <= 0) hz = 60;   // safe default

    // Wake slightly before vblank so DwmFlush() does the final pin. Cap
    // at 2 ms minimum so an exotic 500 Hz display still leaves room for
    // other work between ticks.
    int period = static_cast<int>(std::lround(1000.0 / hz)) - 1;
    if (period < 2)  period = 2;
    if (period > 33) period = 33;   // never tick slower than ~30 Hz
    if (hz == refresh_hz_ && period == tick_period_ms_) {
        return;   // already armed correctly
    }

    refresh_hz_     = hz;
    tick_period_ms_ = period;

    LARGE_INTEGER due{};
    due.QuadPart = -10000LL * period;   // first fire after one period
    ::SetWaitableTimer(tick_event_, &due, period, nullptr, nullptr, FALSE);
    spdlog::info("Tick rate: {} Hz panel -> {} ms period", hz, period);
}

// ----------------------------------------------------------------------------
// Tray tooltip
// ----------------------------------------------------------------------------
// Builds "Magnifier - <mode> | toggle: Ctrl+Alt+Z | settings: Ctrl+Alt+S"
// from the currently-applied hotkey bindings so users get a hint on hover
// without opening Settings. The tray API caps NIF_TIP at 127 wide chars;
// we truncate gracefully if a user binds a wildly long key combo.
void App::RefreshTrayTooltip_() {
    const auto snap = state_.GetSnapshot();
    const wchar_t* mode_label =
        snap.mode == MagMode::Off       ? L"idle" :
        snap.mode == MagMode::Lens      ? L"lens" :
                                          L"full-screen";

    auto key_for = [&](Action a) -> std::wstring {
        auto it = cfg_.hotkeys.find(a);
        if (it == cfg_.hotkeys.end() || !it->second.is_bound()) return L"-";
        return Utf8ToWide(it->second.to_human());
    };

    std::wstring tip = L"Magnifier - ";
    tip += mode_label;
    tip += L"  |  toggle: " + key_for(Action::ToggleLens);
    tip += L"  |  settings: " + key_for(Action::ShowSettings);
    if (tip.size() > 120) tip.resize(120);   // leave room for ellipsis
    tray_.SetTooltip(tip);
}

} // namespace magnifier
