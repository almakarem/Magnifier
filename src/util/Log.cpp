#include "util/Log.h"
#include "util/Paths.h"
#include "util/StringConv.h"

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>

namespace magnifier::log {

namespace {
std::once_flag g_init_once;
std::atomic<bool> g_initialised{false};
} // namespace

spdlog::level::level_enum ParseLevel(std::string_view name) {
    std::string s(name);
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "trace") return spdlog::level::trace;
    if (s == "debug") return spdlog::level::debug;
    if (s == "info")  return spdlog::level::info;
    if (s == "warn" || s == "warning") return spdlog::level::warn;
    if (s == "error" || s == "err")    return spdlog::level::err;
    if (s == "critical")               return spdlog::level::critical;
    if (s == "off")                    return spdlog::level::off;
    return spdlog::level::info;
}

void Init(std::string_view level_name) {
    std::call_once(g_init_once, [&] {
        try {
            spdlog::init_thread_pool(8192, 1);

            const auto log_path = LogsDir() / L"magnifier.log";

            // 5 MB per file, keep 3 (= ~15 MB cap), wide-char filenames.
            auto file_sink =
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    log_path.wstring(), 5 * 1024 * 1024, 3, /*rotate_on_open*/ false);

            auto dbg_sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();

            std::vector<spdlog::sink_ptr> sinks{file_sink, dbg_sink};

            auto logger = std::make_shared<spdlog::async_logger>(
                "magnifier",
                sinks.begin(), sinks.end(),
                spdlog::thread_pool(),
                spdlog::async_overflow_policy::overrun_oldest);

            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
            logger->set_level(ParseLevel(level_name));
            logger->flush_on(spdlog::level::warn);

            spdlog::set_default_logger(logger);
            spdlog::flush_every(std::chrono::seconds(5));

            g_initialised.store(true, std::memory_order_release);
            spdlog::info("Magnifier logger initialised (level={})",
                         std::string(level_name));
        } catch (const std::exception& ex) {
            // Last-ditch: fall back to default stderr logger so we have *some*
            // output instead of a silent crash.
            try { spdlog::default_logger()->error(
                "Logger init failed: {}", ex.what()); } catch (...) {}
        }
    });
}

void Shutdown() {
    if (!g_initialised.load(std::memory_order_acquire)) return;
    try {
        spdlog::shutdown();
    } catch (...) {
        // Swallow — we are tearing down.
    }
}

} // namespace magnifier::log
