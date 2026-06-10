#pragma once

#include "ipc/Commands.h"

#include <Windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace magnifier {

class App;     // forward

// Hosts the three IPC channels:
//   1. WM_COPYDATA on a hidden message window (single-instance forwarding).
//   2. Named pipe \\.\pipe\<name> (current-user ACL, newline-delimited JSON).
//   3. (optional) HTTP loopback on 127.0.0.1:<port> via http.sys.
//
// All incoming commands are decoded into a Command and posted to the App
// via the supplied Sink. The sink IS called on a worker thread; the App
// implementation is responsible for marshalling to the UI thread (we use
// PostMessage(WM_APP_IPC_COMMAND)).
class IpcServer {
public:
    using Sink = std::function<std::string(const Command&)>;  // returns JSON response

    IpcServer() = default;
    ~IpcServer();

    IpcServer(const IpcServer&)            = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    bool Start(HINSTANCE hinst, std::string pipe_name, int http_port, Sink sink);
    void Stop();

    HWND CopyDataHwnd() const { return msg_wnd_; }

    // Send a Command to an already-running instance via WM_COPYDATA.
    // Returns the response JSON, or empty string on failure. Blocking,
    // bounded by `timeout_ms`.
    static std::string SendToRunningInstance(const Command& cmd, DWORD timeout_ms = 1500);

    // Find the running instance's hidden message window. Returns nullptr
    // if no instance is running.
    static HWND FindRunningInstance();

private:
    static LRESULT CALLBACK WndProc_(HWND, UINT, WPARAM, LPARAM);

    void PipeAcceptLoop_();
    void HttpServeLoop_();

    HINSTANCE       hinst_       = nullptr;
    HWND            msg_wnd_     = nullptr;
    Sink            sink_;
    std::string     pipe_name_;
    int             http_port_   = 0;

    std::atomic<bool> running_{false};
    std::thread       pipe_thread_;
    std::thread       http_thread_;

    // Last response produced by the sink for the current WM_COPYDATA call
    // — pulled out by SendCopyData so we can return it as a string. Only
    // touched from the UI thread.
    std::string     last_response_;
};

} // namespace magnifier
