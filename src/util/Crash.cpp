#include "util/Crash.h"
#include "util/Paths.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

#pragma comment(lib, "Dbghelp.lib")

namespace magnifier::crash {

namespace {

std::atomic<bool> g_in_handler{false};

std::wstring TimestampedDumpName() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto tt  = system_clock::to_time_t(now);
    std::tm tm{};
    ::localtime_s(&tm, &tt);
    wchar_t buf[64];
    std::swprintf(buf, _countof(buf),
        L"crash-%04d%02d%02d-%02d%02d%02d.dmp",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

LONG WINAPI Filter(EXCEPTION_POINTERS* info) {
    // Guard against re-entry if MiniDumpWriteDump itself faults.
    bool expected = false;
    if (!g_in_handler.compare_exchange_strong(expected, true)) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    const auto dump_path = CrashDumpsDir() / TimestampedDumpName();

    HANDLE file = ::CreateFileW(dump_path.c_str(),
        GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId          = ::GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers    = FALSE;

        const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(
            MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules |
            MiniDumpWithDataSegs);

        ::MiniDumpWriteDump(::GetCurrentProcess(),
                            ::GetCurrentProcessId(),
                            file, type,
                            info ? &mei : nullptr,
                            nullptr, nullptr);
        ::CloseHandle(file);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

std::once_flag g_install_once;

} // namespace

void Install() {
    std::call_once(g_install_once, [] {
        ::SetUnhandledExceptionFilter(&Filter);
        // Disable "Application Error" dialog so the dump is written and the
        // process exits cleanly (important when running headless / from CI).
        ::SetErrorMode(::GetErrorMode() | SEM_NOGPFAULTERRORBOX);
    });
}

} // namespace magnifier::crash
