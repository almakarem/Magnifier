// Magnifier — WinMain entry point.
//
// Responsibilities:
//   1. Parse argv (CliParser).
//   2. If another instance is already running, forward the command over
//      WM_COPYDATA and exit.
//   3. Otherwise:
//        a. Install crash handler + early logger.
//        b. Acquire a per-user mutex (single-instance gate).
//        c. Construct App, hand off to its message loop.

#include "app/App.h"
#include "ipc/CliParser.h"
#include "ipc/IpcServer.h"
#include "util/Crash.h"
#include "util/Log.h"
#include "util/StringConv.h"
#include "Version.h"

#include <Windows.h>
#include <shellapi.h>

#include <cstdio>
#include <memory>
#include <string>

using namespace magnifier;

namespace {

constexpr wchar_t kSingleInstanceMutex[] = L"Local\\MagnifierApp_SingleInstance_v1";

// Print a UTF-8 string to stdout if a console is attached, otherwise show
// a message box. Streamer + scripting users typically run with a console
// (PowerShell/CMD); the message box is a safety net for GUI launches.
void PrintToUser(const std::string& utf8) {
    HANDLE out = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        ::WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()),
                    &written, nullptr);
        if (written) return;
    }
    ::MessageBoxW(nullptr, Utf8ToWide(utf8).c_str(), L"Magnifier",
                  MB_OK | MB_ICONINFORMATION);
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR, int)
{
    // Argv via CommandLineToArgvW (more reliable than lpCmdLine).
    int wargc = 0;
    LPWSTR* wargv = ::CommandLineToArgvW(::GetCommandLineW(), &wargc);
    if (!wargv) return 1;

    const CliResult cli = ParseCli(wargc, wargv);
    ::LocalFree(wargv);

    // --- pure-info short-circuits ---------------------------------------
    if (cli.mode == CliResult::Mode::PrintHelp) {
        PrintToUser(HelpText());
        return 0;
    }
    if (cli.mode == CliResult::Mode::PrintVersion) {
        std::string s = std::string(kProjectName) + " " + kVersionString + "\n";
        PrintToUser(s);
        return 0;
    }
    if (cli.mode == CliResult::Mode::ParseError) {
        PrintToUser("Magnifier: " + cli.error_message + "\n\n");
        PrintToUser(HelpText());
        return 2;
    }

    // --- if a command is intended ONLY to control the running instance ---
    if (cli.mode == CliResult::Mode::ForwardOnly) {
        if (!cli.startup_command) return 0;
        // Special case --status: query via named pipe so we get a real reply.
        if (cli.startup_command->kind == CmdKind::GetStatus) {
            // Try WM_COPYDATA first to make sure an instance is alive, then
            // open the pipe and ask. (Doing this from a separate small CLI
            // utility is also fine — we keep things simple by reusing the
            // current binary.)
            HWND target = IpcServer::FindRunningInstance();
            if (!target) {
                PrintToUser("{\"ok\":false,\"error\":\"not_running\"}\n");
                return 3;
            }
            // Forward via pipe using a default name (we don't have config
            // loaded). Production deployments should pass --ipc-pipe-name
            // if they customise it; for v1 we assume the default.
            const wchar_t* kPipe = L"\\\\.\\pipe\\MagnifierCtl";
            HANDLE h = ::CreateFileW(kPipe, GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                PrintToUser("{\"ok\":false,\"error\":\"pipe_open_failed\"}\n");
                return 3;
            }
            std::string payload = SerializeCommand(*cli.startup_command);
            payload.push_back('\n');
            DWORD written = 0;
            ::WriteFile(h, payload.data(),
                static_cast<DWORD>(payload.size()), &written, nullptr);
            char buf[4096];
            DWORD read = 0;
            std::string reply;
            while (::ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) {
                reply.append(buf, read);
                if (!reply.empty() && reply.back() == '\n') break;
            }
            ::CloseHandle(h);
            PrintToUser(reply.empty() ? "{\"ok\":false,\"error\":\"no_reply\"}\n" : reply);
            return 0;
        }

        // --force-quit: try graceful first; if no instance found, exit cleanly.
        if (cli.startup_command->kind == CmdKind::ForceQuit) {
            std::string ack = IpcServer::SendToRunningInstance(*cli.startup_command, 1500);
            return ack.empty() ? 0 : 0;
        }

        const std::string ack = IpcServer::SendToRunningInstance(
            *cli.startup_command, 1500);
        if (ack.empty()) {
            PrintToUser("Magnifier is not running.\n");
            return 4;
        }
        return 0;
    }

    // --- normal launch ---------------------------------------------------
    crash::Install();
    log::Init("info");

    // Single-instance gate. If another instance owns the mutex, forward our
    // startup command (if any) and exit.
    HANDLE mtx = ::CreateMutexW(nullptr, FALSE, kSingleInstanceMutex);
    if (mtx && ::GetLastError() == ERROR_ALREADY_EXISTS) {
        if (cli.startup_command) {
            IpcServer::SendToRunningInstance(*cli.startup_command, 1500);
        } else {
            // Bring the running instance's settings window to front as a
            // friendly "I'm already running" signal.
            Command c{}; c.kind = CmdKind::ShowSettings;
            IpcServer::SendToRunningInstance(c, 1500);
        }
        ::CloseHandle(mtx);
        return 0;
    }

    int rc = 0;
    {
        App app;
        AppOptions opts;
        opts.start_minimized = cli.start_minimized;
        opts.startup_command = cli.startup_command;
        if (!app.Initialise(hinst, opts)) {
            spdlog::error("App initialisation failed.");
            rc = 5;
        } else {
            rc = app.Run();
        }
    }

    log::Shutdown();
    if (mtx) ::CloseHandle(mtx);
    return rc;
}
