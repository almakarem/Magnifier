#pragma once

#include <spdlog/spdlog.h>
#include <string_view>

namespace magnifier::log {

// Initialise spdlog. Idempotent.
//   level_name : trace|debug|info|warn|error|off
void Init(std::string_view level_name);

// Flush + drop all sinks. Safe to call on shutdown.
void Shutdown();

// Convert a string ("info", "DEBUG", ...) to spdlog::level::level_enum.
spdlog::level::level_enum ParseLevel(std::string_view name);

} // namespace magnifier::log
