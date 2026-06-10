#pragma once

#include <Windows.h>
#include <string>
#include <system_error>

namespace magnifier {

// Stringify the last Win32 error.
inline std::string LastErrorString(DWORD err = ::GetLastError()) {
    if (err == 0) return "(no error)";
    LPSTR buf = nullptr;
    const DWORD len = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    std::string s;
    if (len && buf) {
        s.assign(buf, len);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        ::LocalFree(buf);
    } else {
        s = "Win32 error " + std::to_string(err);
    }
    return s;
}

} // namespace magnifier
