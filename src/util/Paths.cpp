#include "util/Paths.h"

#include <Windows.h>
#include <ShlObj.h>
#include <KnownFolders.h>

#include <system_error>

namespace fs = std::filesystem;

namespace magnifier {

namespace {

fs::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    if (FAILED(::SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p)) || !p) {
        if (p) ::CoTaskMemFree(p);
        return {};
    }
    fs::path out(p);
    ::CoTaskMemFree(p);
    return out;
}

fs::path EnsureDir(fs::path p) {
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

} // namespace

fs::path AppDataDir() {
    return EnsureDir(KnownFolder(FOLDERID_RoamingAppData) / L"Magnifier");
}

fs::path LocalAppDataDir() {
    return EnsureDir(KnownFolder(FOLDERID_LocalAppData) / L"Magnifier");
}

fs::path ConfigFilePath() {
    return AppDataDir() / L"config.toml";
}

fs::path LogsDir() {
    return EnsureDir(LocalAppDataDir() / L"logs");
}

fs::path CrashDumpsDir() {
    return EnsureDir(LocalAppDataDir() / L"crashes");
}

fs::path PidFilePath() {
    return LocalAppDataDir() / L"pid";
}

fs::path ExecutableDir() {
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, buf, _countof(buf));
    if (n == 0 || n >= _countof(buf)) return {};
    return fs::path(buf).parent_path();
}

} // namespace magnifier
