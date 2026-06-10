#pragma once

#include "config/ConfigStore.h"
#include "input/Actions.h"
#include "input/ControllerPoll.h"
#include "input/HotkeyManager.h"
#include "input/InputRouter.h"
#include "ipc/IpcServer.h"
#include "mag/MagController.h"
#include "mag/StateModel.h"
#include "ui/SettingsWindow.h"
#include "ui/TrayIcon.h"
#include "update/Updater.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>

namespace magnifier {

struct AppOptions {
    bool                    start_minimized = false;
    std::optional<Command>  startup_command;
};

class App {
public:
    App();
    ~App();

    App(const App&)            = delete;
    App& operator=(const App&) = delete;

    // Wire all subsystems. Returns false on hard initialisation failure.
    bool Initialise(HINSTANCE hinst, const AppOptions& opts);

    // Run the Win32 message loop until WM_QUIT.
    int Run();

    // ---- methods invoked by InputRouter / IpcServer / TrayIcon ----------
    void SetMode(MagMode mode);
    void ToggleMode(MagMode mode);
    void RecenterOnCursor();
    void ResizeLens(int dw, int dh);
    void JumpToNextMonitor();
    void SetControllerEnabled(bool enabled);
    void ReloadConfig();
    void ShowSettings();
    void HideSettings();
    void RequestQuit(bool force);

    float DefaultZoomStep() const noexcept { return cfg_.zoom.default_step; }
    float InitialZoom()    const noexcept { return cfg_.zoom.initial; }
    int   LensPanStep()    const noexcept { return cfg_.lens.pan_step; }

    // The IpcServer Sink — runs on a worker thread; we marshal to UI.
    std::string OnIpcCommand(const Command& cmd);

private:
    static LRESULT CALLBACK AppWndProc_(HWND, UINT, WPARAM, LPARAM);

    void OnTick_();
    void OnDisplayChange_();
    void OnPowerEvent_(WPARAM event);
    void ApplyConfig_(const Config& cfg);
    void ApplyHotkeys_();
    void ApplyUpdateSettings_();
    void StartUpdateCheck_(bool from_user);
    void StartUpdateDownload_();
    void StartUpdateInstall_();
    void UpdateMouseFollow_();
    void WritePidFile_() const;
    void RemovePidFile_() const;
    std::string BuildStatusJson_() const;

    HINSTANCE                       hinst_      = nullptr;
    HWND                            app_wnd_    = nullptr;     // message-only
    HANDLE                          tick_event_ = nullptr;     // high-res waitable timer

    Config                          cfg_;
    std::filesystem::path           config_path_;

    StateModel                      state_;
    MagController                   mag_;
    HotkeyManager                   hotkeys_;
    ControllerPoll                  controller_;
    std::unique_ptr<InputRouter>    router_;
    IpcServer                       ipc_;
    TrayIcon                        tray_;
    SettingsWindow                  settings_;
    Updater                         updater_;

    // Latest update-check result (owned by UI thread). When `update_available`
    // is true and `info` is set, the Settings "Updates" tab offers a
    // download/install button.
    UpdateCheckResult               update_status_{};
    std::wstring                    update_downloaded_msi_;
    std::atomic<bool>               update_downloading_{false};
    std::atomic<int64_t>            update_dl_bytes_{0};
    std::atomic<int64_t>            update_dl_total_{0};

    std::atomic<bool>               quit_requested_{false};
    bool                            controller_running_ = false;
    std::chrono::steady_clock::time_point last_tick_;
    std::chrono::steady_clock::time_point last_settings_render_;

    MagMode                         last_mode_ = MagMode::Lens;   // for Toggle

    // Cross-thread command queue. Workers (IPC, controller poll) push here;
    // the UI thread drains it in WM_APP_DRAIN.
    std::mutex                      cmd_mu_;
    std::queue<Command>             cmd_queue_;
    std::mutex                      action_mu_;
    std::queue<Action>              action_queue_;
    std::mutex                      frame_mu_;
    ControllerFrame                 latest_frame_{};
    bool                            frame_pending_ = false;
};

} // namespace magnifier
