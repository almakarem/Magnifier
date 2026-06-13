#include "ui/SettingsWindow.h"
#include "util/Log.h"
#include "util/WinError.h"

#ifdef HAVE_ICON
#include "resource.h"   // IDI_APP
#endif

#include <Version.h>

#include <cstring>
#include <mutex>

#if MAGNIFIER_HAVE_IMGUI

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <d3d11.h>
#include <dxgi.h>

#include <wrl/client.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

// Forward declaration from imgui_impl_win32.cpp (the header keeps this in a
// #if 0 block so it doesn't drag in <Windows.h> for users who only include
// the helper header). MUST live in the global namespace — that's where the
// definition lives.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace magnifier {

namespace {

constexpr wchar_t kClassName[]  = L"MagnifierSettingsWnd_v1";
constexpr wchar_t kWindowTitle[] = L"Magnifier — Settings";

} // namespace

struct SettingsWindow::Impl {
    HWND          hwnd = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Device>           device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    ctx;
    Microsoft::WRL::ComPtr<IDXGISwapChain>         swap;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;

    Config              cfg;
    SettingsWindow::ApplySink apply;
    std::atomic<bool>   visible{false};
    bool                imgui_init = false;

    // ---- Hotkey rebinding state (UI thread only) ---------------------------
    // When `recording` is true, the next non-modifier key down captured by
    // the wndproc is written into `cfg.hotkeys[recording_action]` instead of
    // being forwarded to ImGui. Esc cancels.
    bool   recording        = false;
    Action recording_action = Action::None;

    // ---- Updates tab state -------------------------------------------------
    SettingsWindow::UpdateAction update_check_cb;
    SettingsWindow::UpdateAction update_download_cb;
    SettingsWindow::UpdateAction update_install_cb;
    UpdateCheckResult            update_status{};
    bool                         update_check_ever_ran = false;

    // ---- Diagnostics tab ---------------------------------------------------
    // Mirrored controller poll snapshot. The poll thread calls SetDiagnostics
    // very frequently; we just keep the most recent value behind a small
    // mutex so the UI thread can render a stable snapshot.
    std::mutex      diag_mu;
    ControllerFrame diag_frame{};

    // ---- DPI / scaling -----------------------------------------------------
    // Logical base window size at 100% (96 DPI). We multiply by dpi_scale to
    // get physical pixels, and we rebuild the ImGui font at base_font_px *
    // dpi_scale so text is crisp on every monitor.
    static constexpr int   kBaseW        = 760;
    static constexpr int   kBaseH        = 560;
    static constexpr float kBaseFontPx   = 15.0f;
    float dpi_scale = 1.0f;

    // (Re)build the ImGui default font at the current dpi_scale and apply
    // per-widget padding / rounding scale. Call after CreateContext and
    // after any WM_DPICHANGED. Must be on the UI thread.
    void RebuildFontsForDpi() {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        io.Fonts->AddFontDefault();
        // Override the default 13 px with a DPI-proportional size. The
        // default font is a rasterised bitmap so we scale via
        // FontGlobalScale, which ImGui applies at draw time without
        // requiring a rebuild of the atlas texture.
        // dpi_scale = 1.0 @ 96 DPI, 1.25 @ 120 DPI, 2.0 @ 192 DPI ...
        io.FontGlobalScale = (kBaseFontPx * dpi_scale) / 13.0f;

        // Scale all style sizes (padding, rounding, item spacing, ...) so
        // the whole UI breathes proportionally on high-DPI displays. We
        // re-apply StyleColorsDark first to reset any previous scaling.
        ImGui::StyleColorsDark();
        ImGui::GetStyle().ScaleAllSizes(dpi_scale);
    }
    // WGI probe results pushed less frequently (every few ticks) by App.
    unsigned        wgi_gamepad_count = 0;
    unsigned        wgi_raw_count     = 0;
    std::string     wgi_first_raw_name;

    bool CreateDevice() {
        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount        = 2;
        sd.BufferDesc.Width   = 0;
        sd.BufferDesc.Height  = 0;
        sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.RefreshRate.Numerator   = 60;
        sd.BufferDesc.RefreshRate.Denominator = 1;
        sd.Flags                = 0;
        sd.BufferUsage          = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow         = hwnd;
        sd.SampleDesc.Count     = 1;
        sd.Windowed             = TRUE;
        sd.SwapEffect           = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL fls[] = {
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0,
        };
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = ::D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            0, fls, _countof(fls), D3D11_SDK_VERSION,
            &sd, swap.GetAddressOf(), device.GetAddressOf(),
            &got, ctx.GetAddressOf());
        if (FAILED(hr)) {
            spdlog::error("D3D11CreateDeviceAndSwapChain failed: 0x{:08X}",
                          static_cast<uint32_t>(hr));
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
        swap->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf()));
        device->CreateRenderTargetView(back.Get(), nullptr, rtv.GetAddressOf());
        return true;
    }

    void DestroyDevice() {
        rtv.Reset();
        swap.Reset();
        ctx.Reset();
        device.Reset();
    }

    void ResizeBackBuffer(UINT w, UINT h) {
        if (!swap || w == 0 || h == 0) return;
        rtv.Reset();
        swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> back;
        swap->GetBuffer(0, IID_PPV_ARGS(back.GetAddressOf()));
        device->CreateRenderTargetView(back.Get(), nullptr, rtv.GetAddressOf());
    }

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        // Non-client messages (title-bar drag, frame resize, hit-test) MUST
        // reach DefWindowProc untouched — ImGui has no business handling them
        // and intercepting them breaks window dragging.
        if (m == WM_NCHITTEST || (m >= WM_NCMOUSEMOVE && m <= WM_NCXBUTTONDBLCLK)) {
            return ::DefWindowProcW(h, m, w, l);
        }
        auto* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(h, GWLP_USERDATA));

        // ---- Hotkey rebind capture --------------------------------------
        // While the user is recording a new binding, swallow keyboard
        // messages here so ImGui doesn't act on them and so the OS doesn't
        // ring the system bell for unhandled accelerators.
        if (self && self->recording) {
            if (m == WM_KEYDOWN || m == WM_SYSKEYDOWN) {
                const unsigned vk = static_cast<unsigned>(w);
                if (vk == VK_ESCAPE) {
                    self->recording = false;
                    self->recording_action = Action::None;
                    return 0;
                }
                // Ignore pure modifier presses — we need a real key with
                // them held.
                const bool is_modifier =
                    vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
                    vk == VK_SHIFT   || vk == VK_LSHIFT   || vk == VK_RSHIFT   ||
                    vk == VK_MENU    || vk == VK_LMENU    || vk == VK_RMENU    ||
                    vk == VK_LWIN    || vk == VK_RWIN;
                if (!is_modifier) {
                    unsigned mods = 0;
                    if (::GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
                    if (::GetKeyState(VK_SHIFT)   & 0x8000) mods |= MOD_SHIFT;
                    if (::GetKeyState(VK_MENU)    & 0x8000) mods |= MOD_ALT;
                    if ((::GetKeyState(VK_LWIN) | ::GetKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
                    HotkeyBinding b{};
                    b.modifiers = mods;
                    b.vk        = vk;
                    self->cfg.hotkeys[self->recording_action] = b;
                    self->recording = false;
                    self->recording_action = Action::None;
                }
                return 0;
            }
            if (m == WM_KEYUP || m == WM_SYSKEYUP || m == WM_CHAR ||
                m == WM_SYSCHAR || m == WM_DEADCHAR) {
                return 0;
            }
        }

        if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return 1;
        switch (m) {
            case WM_SIZE:
                if (self && w != SIZE_MINIMIZED) {
                    self->ResizeBackBuffer(LOWORD(l), HIWORD(l));
                }
                return 0;
            case WM_DPICHANGED: {
                // lParam = suggested RECT at the new DPI. Resize to it so
                // the window doesn't shrink / grow mid-drag on a per-monitor
                // setup. Also rebuild fonts so text stays sharp.
                const RECT* r = reinterpret_cast<const RECT*>(l);
                ::SetWindowPos(h, nullptr,
                    r->left, r->top,
                    r->right  - r->left,
                    r->bottom - r->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
                if (self) {
                    self->dpi_scale = static_cast<float>(HIWORD(w)) / 96.0f;
                    if (self->imgui_init) self->RebuildFontsForDpi();
                }
                return 0;
            }
            case WM_SYSCOMMAND:
                // Disable Alt-application-menu — ImGui handles its own menus.
                if ((w & 0xfff0) == SC_KEYMENU) return 0;
                return ::DefWindowProcW(h, m, w, l);
            case WM_CLOSE: {
                if (self) self->visible.store(false, std::memory_order_release);
                ::ShowWindow(h, SW_HIDE);
                return 0;
            }
            case WM_DESTROY:
                return 0;
            default:
                return ::DefWindowProcW(h, m, w, l);
        }
    }
};

SettingsWindow::SettingsWindow() : p_(std::make_unique<Impl>()) {}
SettingsWindow::~SettingsWindow() { Hide(); }

void SettingsWindow::SetConfig(const Config& cfg) {
    p_->cfg = cfg;
}
void SettingsWindow::SetApplySink(ApplySink sink) { p_->apply = std::move(sink); }
void SettingsWindow::SetUpdateController(UpdateAction check, UpdateAction download, UpdateAction install) {
    p_->update_check_cb    = std::move(check);
    p_->update_download_cb = std::move(download);
    p_->update_install_cb  = std::move(install);
}
void SettingsWindow::SetUpdateStatus(const UpdateCheckResult& status) {
    p_->update_status         = status;
    p_->update_check_ever_ran = true;
}

void SettingsWindow::SetDiagnostics(const ControllerFrame& frame) {
    std::scoped_lock lk(p_->diag_mu);
    p_->diag_frame = frame;
}

void SettingsWindow::SetWgiProbe(unsigned gamepad_count, unsigned raw_count,
                                 const std::string& first_raw_name) {
    std::scoped_lock lk(p_->diag_mu);
    p_->wgi_gamepad_count  = gamepad_count;
    p_->wgi_raw_count      = raw_count;
    p_->wgi_first_raw_name = first_raw_name;
}

bool SettingsWindow::IsVisible() const noexcept {
    return p_->visible.load(std::memory_order_acquire);
}

void SettingsWindow::Show() {
    if (!p_->hwnd) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = &Impl::Proc;
        wc.hInstance     = ::GetModuleHandleW(nullptr);
        wc.hCursor       = ::LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon         = nullptr;
#ifdef HAVE_ICON
        wc.hIcon = static_cast<HICON>(::LoadImageW(
            wc.hInstance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
            ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON),
            LR_DEFAULTCOLOR));
#endif
        if (!wc.hIcon) wc.hIcon = ::LoadIcon(nullptr, IDI_APPLICATION);
        wc.lpszClassName = kClassName;
        ::RegisterClassExW(&wc);

        p_->hwnd = ::CreateWindowExW(0, kClassName, kWindowTitle,
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT, CW_USEDEFAULT,
            Impl::kBaseW, Impl::kBaseH,
            nullptr, nullptr, wc.hInstance, nullptr);
        if (!p_->hwnd) {
            spdlog::error("SettingsWindow: CreateWindowExW failed: {}", LastErrorString());
            return;
        }

        // Resolve the monitor DPI now that the window has an HWND (Windows
        // assigns it to a monitor before returning from CreateWindowExW).
        // GetDpiForWindow is available on Win10 1607+; fall back to 96 on
        // older builds.
        {
            using GetDpiFn = UINT (WINAPI*)(HWND);
            HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
            auto fn = user32
                ? reinterpret_cast<GetDpiFn>(::GetProcAddress(user32, "GetDpiForWindow"))
                : nullptr;
            const UINT dpi = fn ? fn(p_->hwnd) : 96u;
            p_->dpi_scale  = static_cast<float>(dpi) / 96.0f;
            // Resize the window to the correct physical size for this monitor.
            const int w_px = static_cast<int>(Impl::kBaseW * p_->dpi_scale);
            const int h_px = static_cast<int>(Impl::kBaseH * p_->dpi_scale);
            ::SetWindowPos(p_->hwnd, nullptr, 0, 0, w_px, h_px,
                           SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        // GWLP_USERDATA must be set BEFORE the swap chain is created — the
        // wndproc may receive WM_SIZE during D3D11CreateDeviceAndSwapChain.
        ::SetWindowLongPtrW(p_->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p_.get()));

        if (!p_->CreateDevice()) {
            ::DestroyWindow(p_->hwnd);
            p_->hwnd = nullptr;
            return;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui_ImplWin32_Init(p_->hwnd);
        ImGui_ImplDX11_Init(p_->device.Get(), p_->ctx.Get());
        // Build fonts + style at the actual monitor DPI so the window is
        // readable on 4K / high-DPI displays out of the box.
        p_->RebuildFontsForDpi();
        p_->imgui_init = true;
    }

    ::ShowWindow(p_->hwnd, SW_SHOW);
    ::SetForegroundWindow(p_->hwnd);
    p_->visible.store(true, std::memory_order_release);
}

void SettingsWindow::Hide() {
    if (!p_) return;
    p_->visible.store(false, std::memory_order_release);
    if (p_->imgui_init) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        p_->imgui_init = false;
    }
    p_->DestroyDevice();
    if (p_->hwnd) {
        ::DestroyWindow(p_->hwnd);
        p_->hwnd = nullptr;
    }
}

bool SettingsWindow::Render() {
    if (!p_->visible.load(std::memory_order_acquire)) return false;
    if (!p_->imgui_init) return false;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos({0, 0});
    const ImVec2 sz = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(sz);
    ImGui::Begin("Settings", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoCollapse);

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("General")) {
            ImGui::Checkbox("Start minimised",       &p_->cfg.general.start_minimized);
            ImGui::Checkbox("Restore last mode",     &p_->cfg.general.restore_last_mode);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Lens")) {
            ImGui::SliderInt  ("Width",            &p_->cfg.lens.width,            160, 1920);
            ImGui::SliderInt  ("Height",           &p_->cfg.lens.height,           90,  1080);
            ImGui::SliderInt  ("Border thickness", &p_->cfg.lens.border_thickness, 0,   16);
            ImGui::SliderFloat("Position smoothing tau (s)", &p_->cfg.lens.position_tau, 0.0f, 0.5f);
            ImGui::SliderFloat("Zoom smoothing tau (s)",     &p_->cfg.lens.zoom_tau,     0.0f, 0.5f);
            ImGui::Checkbox   ("Follow mouse",     &p_->cfg.lens.follow_mouse);
            ImGui::Checkbox   ("Magnify cursor (laggy)", &p_->cfg.lens.magnify_cursor);
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Off (default): the real OS cursor shows over the lens at\n"
                    "full display refresh rate \u2014 no ghosting.\n\n"
                    "On: the Magnification API draws a zoomed cursor inside\n"
                    "the lens content. Matches the system Magnifier app but\n"
                    "visibly trails behind fast mouse movement.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Zoom")) {
            ImGui::SliderFloat("Initial",      &p_->cfg.zoom.initial,      1.0f, 8.0f);
            ImGui::SliderFloat("Min",          &p_->cfg.zoom.min,          1.0f, 4.0f);
            ImGui::SliderFloat("Max",          &p_->cfg.zoom.max,          2.0f, 16.0f);
            ImGui::SliderFloat("Default step", &p_->cfg.zoom.default_step, 0.05f, 1.0f);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controller")) {
            ImGui::Checkbox  ("Enabled",     &p_->cfg.controller.enabled);
            ImGui::SliderFloat("Deadzone",    &p_->cfg.controller.deadzone,   0.0f, 0.5f);
            ImGui::SliderFloat("Curve",       &p_->cfg.controller.curve,      1.0f, 4.0f);
            ImGui::SliderFloat("Move speed (px/s)", &p_->cfg.controller.move_speed, 100.0f, 4000.0f);
            ImGui::SliderFloat("Zoom speed",  &p_->cfg.controller.zoom_speed, 0.1f, 8.0f);
            ImGui::SliderFloat("Idle re-center (s)", &p_->cfg.controller.idle_recenter_seconds, 0.0f, 10.0f);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Hotkeys")) {
            ImGui::TextWrapped(
                "Click 'Set' next to an action, then press the key combo "
                "you want (e.g. Ctrl+Y). Press Esc to cancel. Click 'Clear' "
                "to unbind. Bindings take effect when you press 'Apply'.");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
            ImGui::TextWrapped(
                "NOTE about Ctrl+Alt+Arrow keys: Intel and AMD graphics drivers "
                "reserve Ctrl+Alt+Arrows for screen rotation. If those bindings "
                "don't seem to fire here, the GPU driver is eating them BEFORE "
                "Windows can dispatch them to us. Two fixes:\n"
                "  1. Disable the GPU's hotkeys (Intel Graphics Command Center -> "
                "Preferences -> System -> Hotkeys -> Off; or AMD Adrenalin -> "
                "Preferences -> Hotkeys).\n"
                "  2. OR rebind 'Pan left/right/up/down' below to something the "
                "GPU doesn't claim, e.g. Win+Alt+Arrows or Shift+Alt+Arrows.\n"
                "Plain Arrow keys also pan while lens mode is active (no "
                "modifier needed).");
            ImGui::PopStyleColor();
            ImGui::Separator();

            struct Row { const char* label; Action act; };
            static constexpr Row kRows[] = {
                {"Toggle lens",        Action::ToggleLens},
                {"Toggle fullscreen",  Action::ToggleFullscreen},
                {"Turn off",           Action::TurnOff},
                {"Zoom in",            Action::ZoomIn},
                {"Zoom out",           Action::ZoomOut},
                {"Zoom reset",         Action::ZoomReset},
                {"Lens larger",        Action::LensSizeUp},
                {"Lens smaller",       Action::LensSizeDown},
                {"Pan left",           Action::PanLeft},
                {"Pan right",          Action::PanRight},
                {"Pan up",             Action::PanUp},
                {"Pan down",           Action::PanDown},
                {"Recenter on cursor", Action::Recenter},
                {"Next monitor",       Action::NextMonitor},
                {"Show settings",      Action::ShowSettings},
                {"Reload config",      Action::ReloadConfig},
                {"Quit",               Action::Quit},
            };
            const ImGuiTableFlags tflags =
                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("hk_table", 3, tflags)) {
                ImGui::TableSetupColumn("Action",  ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Binding", ImGuiTableColumnFlags_WidthFixed,   220.0f);
                ImGui::TableSetupColumn("",       ImGuiTableColumnFlags_WidthFixed,   150.0f);
                for (const auto& r : kRows) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(r.label);

                    ImGui::TableSetColumnIndex(1);
                    const bool recording =
                        p_->recording && p_->recording_action == r.act;
                    auto it = p_->cfg.hotkeys.find(r.act);
                    const bool bound =
                        !recording && it != p_->cfg.hotkeys.end() && it->second.is_bound();
                    if (recording) {
                        ImGui::TextColored({1.0f, 0.85f, 0.2f, 1.0f},
                                           "Press a key...");
                    } else if (bound) {
                        ImGui::TextUnformatted(it->second.to_human().c_str());
                    } else {
                        ImGui::TextDisabled("(unbound)");
                    }

                    ImGui::TableSetColumnIndex(2);
                    ImGui::PushID(static_cast<int>(r.act));
                    if (recording) {
                        if (ImGui::Button("Cancel")) {
                            p_->recording = false;
                            p_->recording_action = Action::None;
                        }
                    } else {
                        if (ImGui::Button("Set")) {
                            p_->recording = true;
                            p_->recording_action = r.act;
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Clear")) {
                        p_->cfg.hotkeys.erase(r.act);
                        if (p_->recording_action == r.act) {
                            p_->recording = false;
                            p_->recording_action = Action::None;
                        }
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Capture / OBS")) {
            const char* modes[] = {"auto", "exclude_self", "include_self"};
            int idx =
                p_->cfg.capture.mode == CaptureMode::ExcludeSelf ? 1 :
                p_->cfg.capture.mode == CaptureMode::IncludeSelf ? 2 : 0;
            if (ImGui::Combo("Mode", &idx, modes, 3)) {
                p_->cfg.capture.mode =
                    idx == 1 ? CaptureMode::ExcludeSelf :
                    idx == 2 ? CaptureMode::IncludeSelf : CaptureMode::Auto;
            }
            ImGui::TextWrapped(
                "auto: hide overlay from OBS/screen capture when supported (Win10 2004+); "
                "include_self: viewers also see the zoom.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Advanced")) {
            ImGui::Checkbox("Low-level keyboard hook (anti-cheat risk)",
                            &p_->cfg.advanced.low_level_keyboard_hook);
            ImGui::SliderInt("HTTP IPC port (0 = off)", &p_->cfg.ipc.http_port, 0, 65535);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Updates")) {
            ImGui::Text("Current version: %s", kVersionString);
            ImGui::TextDisabled("Source: https://github.com/%s/%s",
                                p_->cfg.update.owner.c_str(),
                                p_->cfg.update.repo.c_str());
            ImGui::Separator();

            ImGui::Checkbox("Check on startup", &p_->cfg.update.check_on_startup);
            ImGui::SameLine();
            ImGui::Checkbox("Auto-download new MSI", &p_->cfg.update.auto_download);

            ImGui::Spacing();
            ImGui::Separator();

            // Status section
            const auto& st = p_->update_status;
            if (!p_->update_check_ever_ran) {
                ImGui::TextDisabled("Status: (no check performed yet)");
            } else if (!st.error.empty()) {
                ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "Error: %s", st.error.c_str());
            } else if (st.ok && st.info) {
                if (st.update_available) {
                    ImGui::TextColored({0.4f, 1.0f, 0.4f, 1.0f},
                        "Update available: %s (you have %s)",
                        st.info->version.c_str(), st.current_version.c_str());
                    if (!st.info->release_name.empty()) {
                        ImGui::Text("Release: %s", st.info->release_name.c_str());
                    }
                    if (!st.info->msi_asset_name.empty()) {
                        ImGui::Text("Asset: %s (%.1f MB)",
                                    st.info->msi_asset_name.c_str(),
                                    st.info->msi_asset_size / (1024.0 * 1024.0));
                    } else {
                        ImGui::TextColored({1.0f, 0.8f, 0.2f, 1.0f},
                            "No .msi asset attached to this release.");
                    }
                } else {
                    ImGui::Text("You are on the latest version (%s).",
                                st.current_version.c_str());
                }
                if (!st.info->notes.empty()) {
                    if (ImGui::CollapsingHeader("Release notes")) {
                        ImGui::TextWrapped("%s", st.info->notes.c_str());
                    }
                }
                if (!st.info->release_url.empty()) {
                    ImGui::TextDisabled("%s", st.info->release_url.c_str());
                }
            }

            ImGui::Spacing();
            if (ImGui::Button("Check now") && p_->update_check_cb) {
                p_->update_check_cb();
            }
            const bool can_install =
                st.ok && st.update_available && st.info && !st.info->msi_asset_url.empty();
            if (can_install) {
                ImGui::SameLine();
                if (ImGui::Button("Download && Install") && p_->update_download_cb) {
                    p_->update_download_cb();
                }
            }
            // If the app already downloaded an MSI, offer to launch it now.
            // (We expose this by reading no extra state — the download cb
            // posts WM_APP_UPDATE; the install cb is wired through to
            // App::StartUpdateInstall_ which checks update_downloaded_msi_.)
            ImGui::SameLine();
            if (ImGui::Button("Install downloaded") && p_->update_install_cb) {
                p_->update_install_cb();
            }
            ImGui::SameLine();
            ImGui::TextDisabled("(?)");
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(
                    "Install launches msiexec on the downloaded .msi and\n"
                    "asks Magnifier to exit so the installer can replace the\n"
                    "binary in-place. Your config is preserved.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Diagnostics")) {
            // Live mirror of the latest ControllerPoll frame. Useful for
            // verifying that the BT pad is being seen at all and that
            // sticks/buttons map correctly.
            ControllerFrame f;
            unsigned ng = 0, nr = 0;
            std::string raw_name;
            {
                std::scoped_lock lk(p_->diag_mu);
                f        = p_->diag_frame;
                ng       = p_->wgi_gamepad_count;
                nr       = p_->wgi_raw_count;
                raw_name = p_->wgi_first_raw_name;
            }
            ImGui::Text("Backend: %s",
                        f.backend.empty() ? "(none - no controller detected)" : f.backend.c_str());
            ImGui::Text("Device : %s",
                        f.device_name.empty() ? "-" : f.device_name.c_str());
            ImGui::Text("Present: %s", f.present ? "yes" : "no");
            ImGui::Separator();
            ImGui::Text("WGI enumeration:");
            ImGui::BulletText("Standard Gamepads visible to Windows: %u", ng);
            ImGui::BulletText("Raw HID gamepads visible to Windows : %u%s%s",
                              nr,
                              raw_name.empty() ? "" : "  -  ",
                              raw_name.empty() ? "" : raw_name.c_str());
            ImGui::Separator();

            auto bar = [](const char* label, float v, float vmin, float vmax) {
                const float t = (v - vmin) / (vmax - vmin);
                char buf[32];
                snprintf(buf, sizeof(buf), "%+0.3f", v);
                ImGui::ProgressBar(t < 0 ? 0 : (t > 1 ? 1 : t),
                                   ImVec2(180, 0), buf);
                ImGui::SameLine();
                ImGui::Text("%s", label);
            };
            bar("Left  X",  f.ls_x, -1, 1);
            bar("Left  Y",  f.ls_y, -1, 1);
            bar("Right X",  f.rs_x, -1, 1);
            bar("Right Y",  f.rs_y, -1, 1);
            bar("LT",       f.lt,    0, 1);
            bar("RT",       f.rt,    0, 1);
            ImGui::Separator();
            ImGui::Text("Buttons (XInput-style bitfield): 0x%04X", f.buttons);

            ImGui::Spacing();
            ImGui::TextWrapped(
                "PlayStation controllers (DualShock 4 / DualSense):\n"
                "  Windows does not expose Sony controllers as XInput or standard"
                " gamepads, so they show as 'Gamepads: 0' above even when paired.\n"
                "  Fix: install DS4Windows (https://github.com/Ryochan7/DS4Windows).\n"
                "  DS4Windows wraps your PS controller as a virtual Xbox pad that\n"
                "  Magnifier picks up immediately via XInput. Steam's 'PlayStation\n"
                "  Controller Support' does NOT help here because it operates\n"
                "  inside Steam's own overlay, not system-wide.\n"
                "\n"
                "Xbox / generic XInput controllers:\n"
                "  Should show Gamepads >= 1 when connected. If not, try a\n"
                "  different USB port or check Device Manager for driver issues.\n"
                "\n"
                "Other controllers (joysticks, fight sticks, racing wheels):\n"
                "  If Gamepads = 0 AND Raw > 0, the controller is seen as a generic\n"
                "  HID device. Left-stick analogue movement will work; button\n"
                "  actions may not map correctly. Rebind from the Hotkeys tab or\n"
                "  use DS4Windows / x360ce to wrap it as XInput.");

            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("About")) {
            ImGui::Text("Magnifier %s", kVersionString);
            ImGui::TextDisabled("A low-latency screen magnifier for Windows.");
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextWrapped(
                "This application is free to use. I built it because Windows' "
                "own Magnifier is unreliable and a lot of people struggle with "
                "it, so hopefully this works perfectly for you.");
            ImGui::Spacing();
            ImGui::TextWrapped(
                "It is and will stay free, but if you'd like to tip or donate "
                "it is not required and is very welcome.");
            ImGui::Spacing();

            ImGui::TextUnformatted("Support / donations");
            ImGui::BulletText("Ko-fi : https://ko-fi.com/theamsa");
            ImGui::BulletText("PayPal: https://paypal.me/TheAMSA");
            ImGui::Spacing();

            ImGui::TextUnformatted("Contact");
            ImGui::TextWrapped(
                "If you want to reach out or report a bug, any of these channels work:");
            ImGui::BulletText("Discord: AMSA");
            ImGui::BulletText("X / Twitter: https://x.com/AlmakaremA");
            ImGui::BulletText("Email: AMSAbualmakarem@gmail.com");
            ImGui::Spacing();

            ImGui::TextUnformatted("Source");
            ImGui::BulletText("https://github.com/almakarem/Magnifier");
            ImGui::Spacing();

            ImGui::TextUnformatted("Acknowledgements");
            ImGui::BulletText("Windows Magnification API (Microsoft)");
            ImGui::BulletText("Dear ImGui  -  https://github.com/ocornut/imgui");
            ImGui::BulletText("spdlog, nlohmann/json, tomlplusplus, GoogleTest");
            ImGui::BulletText("WiX Toolset (MSI packaging)");
            ImGui::Spacing();

            ImGui::TextUnformatted("License");
            ImGui::TextWrapped("MIT License - Copyright (c) 2026 AMSA. See LICENSE for details.");

            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Apply") && p_->apply) {
        p_->apply(p_->cfg);
    }
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        ::PostMessageW(p_->hwnd, WM_CLOSE, 0, 0);
    }

    ImGui::End();
    ImGui::Render();

    const float clear[4] = {0.10f, 0.10f, 0.12f, 1.0f};
    p_->ctx->OMSetRenderTargets(1, p_->rtv.GetAddressOf(), nullptr);
    p_->ctx->ClearRenderTargetView(p_->rtv.Get(), clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    // Present without vsync — the app drives us at 60 Hz from its tick timer
    // and we don't want to block the UI thread for 16 ms per call.
    p_->swap->Present(0, 0);
    return true;
}

} // namespace magnifier

#else // !MAGNIFIER_HAVE_IMGUI — null implementation

namespace magnifier {
struct SettingsWindow::Impl { Config cfg; ApplySink apply; bool visible = false; };
SettingsWindow::SettingsWindow() : p_(std::make_unique<Impl>()) {}
SettingsWindow::~SettingsWindow() = default;
void SettingsWindow::SetConfig(const Config& cfg) { p_->cfg = cfg; }
void SettingsWindow::SetApplySink(ApplySink sink) { p_->apply = std::move(sink); }
void SettingsWindow::SetUpdateController(UpdateAction, UpdateAction, UpdateAction) {}
void SettingsWindow::SetUpdateStatus(const UpdateCheckResult&) {}
void SettingsWindow::Show() {
    spdlog::warn("Settings UI not compiled in (MAGNIFIER_ENABLE_IMGUI=OFF).");
}
void SettingsWindow::Hide() { p_->visible = false; }
bool SettingsWindow::IsVisible() const noexcept { return p_->visible; }
bool SettingsWindow::Render() { return false; }
} // namespace magnifier

#endif
