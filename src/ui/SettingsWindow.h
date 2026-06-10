#pragma once

#include "config/ConfigStore.h"
#include "input/ControllerPoll.h"
#include "update/Updater.h"

#include <Windows.h>

#include <atomic>
#include <functional>
#include <memory>

namespace magnifier {

// On-demand settings window backed by Dear ImGui + D3D11. The window is
// created the first time Show() is called and destroyed on Hide().
// All methods must be invoked on the UI thread.
class SettingsWindow {
public:
    using ApplySink = std::function<void(const Config&)>;

    SettingsWindow();
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&)            = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    // Provide the current config snapshot and a callback invoked when the
    // user clicks "Apply".
    void SetConfig(const Config& cfg);
    void SetApplySink(ApplySink sink);

    // Wire the Updates tab buttons. Each callback runs on the UI thread.
    using UpdateAction = std::function<void()>;
    void SetUpdateController(UpdateAction check, UpdateAction download, UpdateAction install);
    // Push the latest async result into the UI (call from WM_APP_UPDATE).
    void SetUpdateStatus(const UpdateCheckResult& status);

    // Push the latest controller poll snapshot for the Diagnostics tab.
    // Cheap to call every tick — copy is small, mutex is uncontended.
    void SetDiagnostics(const ControllerFrame& frame);

    void Show();
    void Hide();
    bool IsVisible() const noexcept;

    // Called from the UI message pump to render a frame when the window is
    // visible. Returns false if the window has been closed (the app should
    // skip further calls until Show() is invoked again).
    bool Render();

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace magnifier
