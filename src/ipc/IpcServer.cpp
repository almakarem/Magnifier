#include "ipc/IpcServer.h"
#include "ipc/JsonFraming.h"
#include "util/Log.h"
#include "util/StringConv.h"
#include "util/WinError.h"

#include <nlohmann/json.hpp>

#include <Windows.h>
#include <AclAPI.h>
#include <sddl.h>
#include <http.h>

#pragma comment(lib, "Httpapi.lib")
#pragma comment(lib, "Advapi32.lib")

#include <array>
#include <cstring>

namespace magnifier {

using json = nlohmann::json;

namespace {

constexpr wchar_t kIpcClassName[]   = L"MagnifierIpcWnd_v1";
constexpr wchar_t kIpcWindowTitle[] = L"MagnifierIpcSink_v1";

// Magic value placed in COPYDATASTRUCT.dwData so we don't react to random
// WM_COPYDATA traffic from unrelated apps.
constexpr ULONG_PTR kCopyDataMagic = 0x4D41474DUL;   // 'MAGM'

} // namespace

// ---------------------------------------------------------------------------
// Command <-> JSON
// ---------------------------------------------------------------------------
static const char* KindName(CmdKind k) {
    switch (k) {
        case CmdKind::EnterLens:          return "enter_lens";
        case CmdKind::EnterFullscreen:    return "enter_fullscreen";
        case CmdKind::TurnOff:            return "turn_off";
        case CmdKind::Toggle:             return "toggle";
        case CmdKind::SetZoom:            return "set_zoom";
        case CmdKind::ZoomDelta:          return "zoom_delta";
        case CmdKind::SetLensSize:        return "set_lens_size";
        case CmdKind::SetMonitorIndex:    return "set_monitor_index";
        case CmdKind::ReloadConfig:       return "reload_config";
        case CmdKind::SwitchProfile:      return "switch_profile";
        case CmdKind::EnableController:   return "enable_controller";
        case CmdKind::DisableController:  return "disable_controller";
        case CmdKind::ShowSettings:       return "show_settings";
        case CmdKind::HideSettings:       return "hide_settings";
        case CmdKind::GetStatus:          return "get_status";
        case CmdKind::Quit:                return "quit";
        case CmdKind::ForceQuit:          return "force_quit";
        case CmdKind::Noop:                return "noop";
    }
    return "noop";
}

static CmdKind KindFromName(std::string_view n) {
    if (n == "enter_lens")          return CmdKind::EnterLens;
    if (n == "enter_fullscreen")    return CmdKind::EnterFullscreen;
    if (n == "turn_off")            return CmdKind::TurnOff;
    if (n == "toggle")              return CmdKind::Toggle;
    if (n == "set_zoom")            return CmdKind::SetZoom;
    if (n == "zoom_delta")          return CmdKind::ZoomDelta;
    if (n == "set_lens_size")       return CmdKind::SetLensSize;
    if (n == "set_monitor_index")   return CmdKind::SetMonitorIndex;
    if (n == "reload_config")       return CmdKind::ReloadConfig;
    if (n == "switch_profile")      return CmdKind::SwitchProfile;
    if (n == "enable_controller")   return CmdKind::EnableController;
    if (n == "disable_controller")  return CmdKind::DisableController;
    if (n == "show_settings")       return CmdKind::ShowSettings;
    if (n == "hide_settings")       return CmdKind::HideSettings;
    if (n == "get_status")          return CmdKind::GetStatus;
    if (n == "quit")                return CmdKind::Quit;
    if (n == "force_quit")          return CmdKind::ForceQuit;
    return CmdKind::Noop;
}

std::string SerializeCommand(const Command& c) {
    json j;
    j["cmd"] = KindName(c.kind);
    json args = json::object();
    if (c.f_value)  args["f"]  = *c.f_value;
    if (c.i_value)  args["i"]  = *c.i_value;
    if (c.i_value2) args["i2"] = *c.i_value2;
    if (c.s_value)  args["s"]  = *c.s_value;
    if (!args.empty()) j["args"] = std::move(args);
    return j.dump();
}

std::optional<Command> DeserializeCommand(std::string_view jstr) {
    try {
        auto j = json::parse(jstr);
        Command c{};
        if (!j.is_object() || !j.contains("cmd") || !j["cmd"].is_string()) return std::nullopt;
        c.kind = KindFromName(j["cmd"].get<std::string>());
        if (j.contains("args") && j["args"].is_object()) {
            const auto& a = j["args"];
            if (a.contains("f")  && a["f"].is_number())  c.f_value  = a["f"].get<float>();
            if (a.contains("i")  && a["i"].is_number_integer()) c.i_value  = a["i"].get<int>();
            if (a.contains("i2") && a["i2"].is_number_integer()) c.i_value2 = a["i2"].get<int>();
            if (a.contains("s")  && a["s"].is_string())  c.s_value  = a["s"].get<std::string>();
        }
        return c;
    } catch (const std::exception& e) {
        spdlog::warn("DeserializeCommand: {}", e.what());
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// IpcServer
// ---------------------------------------------------------------------------

IpcServer::~IpcServer() { Stop(); }

bool IpcServer::Start(HINSTANCE hinst, std::string pipe_name, int http_port, Sink sink) {
    if (msg_wnd_) return true;
    hinst_     = hinst;
    sink_      = std::move(sink);
    pipe_name_ = std::move(pipe_name);
    http_port_ = http_port;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = &IpcServer::WndProc_;
    wc.hInstance     = hinst;
    wc.lpszClassName = kIpcClassName;
    if (!::RegisterClassExW(&wc)) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            spdlog::error("IpcServer: RegisterClassExW failed: {}", LastErrorString(err));
            return false;
        }
    }
    msg_wnd_ = ::CreateWindowExW(0, kIpcClassName, kIpcWindowTitle,
        0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hinst, this);
    if (!msg_wnd_) {
        spdlog::error("IpcServer: CreateWindowExW failed: {}", LastErrorString());
        return false;
    }
    ::SetWindowLongPtrW(msg_wnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    running_.store(true, std::memory_order_release);
    pipe_thread_ = std::thread(&IpcServer::PipeAcceptLoop_, this);
    if (http_port_ > 0) {
        http_thread_ = std::thread(&IpcServer::HttpServeLoop_, this);
    }
    return true;
}

void IpcServer::Stop() {
    running_.store(false, std::memory_order_release);

    // Kick the pipe accept loop by opening + closing a client handle.
    if (pipe_thread_.joinable()) {
        const std::wstring full = L"\\\\.\\pipe\\" + Utf8ToWide(pipe_name_);
        HANDLE h = ::CreateFileW(full.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) ::CloseHandle(h);
        pipe_thread_.join();
    }
    if (http_thread_.joinable()) {
        // http.sys: terminating cleanly requires HttpShutdown of the request queue.
        // The serve loop checks running_ and bails out when read returns.
        http_thread_.join();
    }

    if (msg_wnd_) {
        ::DestroyWindow(msg_wnd_);
        msg_wnd_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// WM_COPYDATA forwarding
// ---------------------------------------------------------------------------
LRESULT CALLBACK IpcServer::WndProc_(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg != WM_COPYDATA) return ::DefWindowProcW(hwnd, msg, wp, lp);

    auto* self = reinterpret_cast<IpcServer*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self || !self->sink_) return FALSE;

    const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lp);
    if (!cds || cds->dwData != kCopyDataMagic || !cds->lpData || cds->cbData == 0) return FALSE;

    const std::string_view payload(
        static_cast<const char*>(cds->lpData),
        static_cast<size_t>(cds->cbData));
    auto maybe_cmd = DeserializeCommand(payload);
    if (!maybe_cmd) return FALSE;

    self->last_response_ = self->sink_(*maybe_cmd);

    // Send response back via a separate WM_COPYDATA reply to the sender if it
    // included a result-window HWND in the high bits of wParam. For now we
    // rely on the named-pipe path for queries; --status used over WM_COPYDATA
    // gets back a TRUE/FALSE ack only.
    (void)wp;
    return TRUE;
}

HWND IpcServer::FindRunningInstance() {
    return ::FindWindowExW(HWND_MESSAGE, nullptr, kIpcClassName, kIpcWindowTitle);
}

std::string IpcServer::SendToRunningInstance(const Command& cmd, DWORD timeout_ms) {
    HWND target = FindRunningInstance();
    if (!target) return {};

    const std::string payload = SerializeCommand(cmd);
    COPYDATASTRUCT cds{};
    cds.dwData = kCopyDataMagic;
    cds.cbData = static_cast<DWORD>(payload.size());
    cds.lpData = const_cast<char*>(payload.data());

    DWORD_PTR result = 0;
    LRESULT lr = ::SendMessageTimeoutW(target, WM_COPYDATA, 0,
        reinterpret_cast<LPARAM>(&cds),
        SMTO_ABORTIFHUNG, timeout_ms, &result);
    return lr ? "{\"ok\":true}" : std::string{};
}

// ---------------------------------------------------------------------------
// Named pipe
// ---------------------------------------------------------------------------
namespace {

// Build a SECURITY_ATTRIBUTES restricting the pipe to the current user.
// `out_sd` MUST outlive any handle that references it.
bool MakeOwnerOnlySecurity(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& out_sd) {
    out_sd = nullptr;
    // "D:(A;;GA;;;OW)"  =  DACL, allow GENERIC_ALL to Owner.
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;OW)", SDDL_REVISION_1, &out_sd, nullptr)) {
        return false;
    }
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = out_sd;
    sa.bInheritHandle       = FALSE;
    return true;
}

} // namespace

void IpcServer::PipeAcceptLoop_() {
    const std::wstring full = L"\\\\.\\pipe\\" + Utf8ToWide(pipe_name_);

    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    const bool have_sd = MakeOwnerOnlySecurity(sa, sd);

    while (running_.load(std::memory_order_acquire)) {
        HANDLE pipe = ::CreateNamedPipeW(
            full.c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096,
            0,                          // default timeout
            have_sd ? &sa : nullptr);

        if (pipe == INVALID_HANDLE_VALUE) {
            spdlog::error("CreateNamedPipeW failed: {}", LastErrorString());
            ::Sleep(500);
            continue;
        }

        const BOOL connected = ::ConnectNamedPipe(pipe, nullptr)
            ? TRUE : (::GetLastError() == ERROR_PIPE_CONNECTED);

        if (!running_.load(std::memory_order_acquire)) {
            ::DisconnectNamedPipe(pipe);
            ::CloseHandle(pipe);
            break;
        }

        if (connected) {
            // Handle this client in-line (per-connection is short-lived).
            JsonLineReader reader([&](std::string_view line) {
                auto maybe = DeserializeCommand(line);
                std::string reply;
                if (maybe && sink_) reply = sink_(*maybe);
                else                reply = R"({"ok":false,"error":"bad_command"})";
                reply.push_back('\n');
                DWORD written = 0;
                ::WriteFile(pipe, reply.data(),
                    static_cast<DWORD>(reply.size()), &written, nullptr);
            });

            std::array<char, 4096> buf{};
            DWORD read = 0;
            while (::ReadFile(pipe, buf.data(),
                              static_cast<DWORD>(buf.size()), &read, nullptr) && read > 0) {
                reader.Feed(buf.data(), read);
            }
        }
        ::DisconnectNamedPipe(pipe);
        ::CloseHandle(pipe);
    }

    if (sd) ::LocalFree(sd);
}

// ---------------------------------------------------------------------------
// HTTP loopback (http.sys)
// ---------------------------------------------------------------------------
void IpcServer::HttpServeLoop_() {
    HTTPAPI_VERSION ver = HTTPAPI_VERSION_2;
    ULONG rc = ::HttpInitialize(ver, HTTP_INITIALIZE_SERVER, nullptr);
    if (rc != NO_ERROR) {
        spdlog::error("HttpInitialize failed ({})", rc);
        return;
    }

    HTTP_SERVER_SESSION_ID session = 0;
    rc = ::HttpCreateServerSession(ver, &session, 0);
    if (rc != NO_ERROR) {
        spdlog::error("HttpCreateServerSession failed ({})", rc);
        ::HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
        return;
    }

    HTTP_URL_GROUP_ID group = 0;
    rc = ::HttpCreateUrlGroup(session, &group, 0);
    if (rc != NO_ERROR) {
        spdlog::error("HttpCreateUrlGroup failed ({})", rc);
        ::HttpCloseServerSession(session);
        ::HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
        return;
    }

    HANDLE req_queue = nullptr;
    rc = ::HttpCreateRequestQueue(ver, nullptr, nullptr, 0, &req_queue);
    if (rc != NO_ERROR) {
        spdlog::error("HttpCreateRequestQueue failed ({})", rc);
        ::HttpCloseUrlGroup(group);
        ::HttpCloseServerSession(session);
        ::HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
        return;
    }

    HTTP_BINDING_INFO bind{};
    bind.Flags.Present     = 1;
    bind.RequestQueueHandle = req_queue;
    ::HttpSetUrlGroupProperty(group, HttpServerBindingProperty, &bind, sizeof(bind));

    wchar_t url[64];
    std::swprintf(url, _countof(url), L"http://127.0.0.1:%d/", http_port_);
    rc = ::HttpAddUrlToUrlGroup(group, url, 0, 0);
    if (rc != NO_ERROR) {
        spdlog::error("HttpAddUrlToUrlGroup({}) failed ({}). "
                      "Is the port in use, or does it need 'netsh http add urlacl'?",
                      WideToUtf8(url), rc);
    } else {
        spdlog::info("HTTP IPC listening on http://127.0.0.1:{}/", http_port_);
    }

    std::vector<unsigned char> req_buf(8192);

    while (running_.load(std::memory_order_acquire)) {
        auto* req = reinterpret_cast<HTTP_REQUEST*>(req_buf.data());
        DWORD bytes = 0;
        ULONG ret = ::HttpReceiveHttpRequest(req_queue, HTTP_NULL_ID,
            0, req, static_cast<ULONG>(req_buf.size()), &bytes, nullptr);

        if (ret == ERROR_MORE_DATA) {
            req_buf.resize(bytes);
            continue;
        }
        if (ret == ERROR_OPERATION_ABORTED || ret == ERROR_HANDLE_EOF) break;
        if (ret != NO_ERROR) { ::Sleep(20); continue; }

        std::string body;
        if (req->Flags & HTTP_REQUEST_FLAG_MORE_ENTITY_BODY_EXISTS) {
            std::array<char, 4096> chunk{};
            DWORD got = 0;
            while (::HttpReceiveRequestEntityBody(req_queue, req->RequestId,
                       0, chunk.data(), static_cast<ULONG>(chunk.size()),
                       &got, nullptr) == NO_ERROR && got > 0) {
                body.append(chunk.data(), got);
            }
        }

        std::string reply_body = R"({"ok":false,"error":"bad_command"})";
        auto maybe = DeserializeCommand(body);
        if (maybe && sink_) reply_body = sink_(*maybe);

        HTTP_RESPONSE resp{};
        resp.StatusCode = 200;
        const char* phrase = "OK";
        resp.pReason       = phrase;
        resp.ReasonLength  = static_cast<USHORT>(std::strlen(phrase));

        HTTP_DATA_CHUNK chunk{};
        chunk.DataChunkType                  = HttpDataChunkFromMemory;
        chunk.FromMemory.pBuffer             = reply_body.data();
        chunk.FromMemory.BufferLength        = static_cast<ULONG>(reply_body.size());
        resp.EntityChunkCount                = 1;
        resp.pEntityChunks                   = &chunk;
        resp.Headers.KnownHeaders[HttpHeaderContentType].pRawValue =
            "application/json";
        resp.Headers.KnownHeaders[HttpHeaderContentType].RawValueLength =
            static_cast<USHORT>(std::strlen("application/json"));

        DWORD sent = 0;
        ::HttpSendHttpResponse(req_queue, req->RequestId, 0, &resp,
            nullptr, &sent, nullptr, 0, nullptr, nullptr);
    }

    ::HttpRemoveUrlFromUrlGroup(group, url, 0);
    ::HttpCloseRequestQueue(req_queue);
    ::HttpCloseUrlGroup(group);
    ::HttpCloseServerSession(session);
    ::HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
}

} // namespace magnifier
