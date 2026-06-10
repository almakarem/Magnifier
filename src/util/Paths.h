#pragma once

#include <filesystem>

namespace magnifier {

// Returns the per-user roaming app-data directory for the magnifier
// (e.g. C:\Users\<u>\AppData\Roaming\Magnifier). Creates it if missing.
std::filesystem::path AppDataDir();

// Returns the local (non-roaming) app data directory. Creates it if missing.
std::filesystem::path LocalAppDataDir();

// Convenience subdirs.
std::filesystem::path ConfigFilePath();    // <AppData>\Magnifier\config.toml
std::filesystem::path LogsDir();           // <LocalAppData>\Magnifier\logs
std::filesystem::path CrashDumpsDir();     // <LocalAppData>\Magnifier\crashes
std::filesystem::path PidFilePath();       // <LocalAppData>\Magnifier\pid

// Directory next to the running .exe (e.g. for portable mode discovery).
std::filesystem::path ExecutableDir();

} // namespace magnifier
