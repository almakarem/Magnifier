#include "ipc/CliParser.h"
#include "util/StringConv.h"

#include <charconv>
#include <cstring>
#include <string>
#include <vector>

namespace magnifier {

namespace {

bool StartsWith(std::string_view s, std::string_view p) {
    return s.size() >= p.size() && std::memcmp(s.data(), p.data(), p.size()) == 0;
}

std::optional<float> ParseFloat(std::string_view s) {
    float v = 0.0f;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return v;
}

std::optional<int> ParseInt(std::string_view s) {
    int v = 0;
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
    return v;
}

// Returns true if argv[i] is `--name` or `--name=value`. Splits the value
// out into `value_out` if present.
bool MatchFlag(std::string_view arg, std::string_view name,
               std::optional<std::string>& value_out) {
    value_out.reset();
    if (arg == name) return true;
    if (StartsWith(arg, name) && arg.size() > name.size() && arg[name.size()] == '=') {
        value_out = std::string(arg.substr(name.size() + 1));
        return true;
    }
    return false;
}

// Greedy "next non-flag argv as value" lookup.
std::optional<std::string> NextValue(std::vector<std::string>& args, size_t& i) {
    if (i + 1 >= args.size()) return std::nullopt;
    const std::string& next = args[i + 1];
    if (StartsWith(next, "--") || next == "-") return std::nullopt;
    return args[++i];
}

void SetCommand(CliResult& res, CmdKind k) {
    if (!res.startup_command) res.startup_command = Command{};
    res.startup_command->kind = k;
}

} // namespace

const char* HelpText() {
    return
    "Magnifier - overlay zoom for streamers.\n"
    "\n"
    "Usage: Magnifier.exe [options]\n"
    "\n"
    "Mode commands (forwarded to a running instance, or applied at startup):\n"
    "  --lens                       Enable lens mode.\n"
    "  --fullscreen                 Enable full-screen zoom mode.\n"
    "  --off                        Disable zoom.\n"
    "  --toggle                     Toggle last-used mode.\n"
    "\n"
    "Adjustments:\n"
    "  --zoom <factor>              Set zoom factor (1.0 .. 16.0).\n"
    "  --zoom-in [step]             Increase zoom (default step from config).\n"
    "  --zoom-out [step]            Decrease zoom.\n"
    "  --lens-size <WxH>            e.g. --lens-size 640x360.\n"
    "  --monitor <index>            Pin fullscreen mode to monitor index.\n"
    "\n"
    "Lifecycle / control:\n"
    "  --reload-config              Re-read config.toml on the running instance.\n"
    "  --profile <name>             Switch to named profile.\n"
    "  --enable-controller          Enable controller input.\n"
    "  --disable-controller         Disable controller input.\n"
    "  --status                     Print running instance status as JSON.\n"
    "  --quit                       Ask running instance to shut down gracefully.\n"
    "  --force-quit                 Terminate running instance immediately.\n"
    "\n"
    "Startup:\n"
    "  --start-minimized            Start with the tray icon only (no flash).\n"
    "  --help, -h                   This text.\n"
    "  --version, -v                Print version and exit.\n";
}

CliResult ParseCli(int argc, wchar_t** argv) {
    CliResult res;

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 1; i < argc; ++i) {
        args.push_back(WideToUtf8(argv[i]));
    }

    bool ipc_only = false;   // when true, exit after forwarding command

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        std::optional<std::string> v;

        if (a == "--help" || a == "-h") {
            res.mode = CliResult::Mode::PrintHelp;
            return res;
        }
        if (a == "--version" || a == "-v") {
            res.mode = CliResult::Mode::PrintVersion;
            return res;
        }
        if (a == "--start-minimized") {
            res.start_minimized = true;
            continue;
        }
        if (a == "--lens")         { SetCommand(res, CmdKind::EnterLens);       continue; }
        if (a == "--fullscreen")   { SetCommand(res, CmdKind::EnterFullscreen); continue; }
        if (a == "--off")          { SetCommand(res, CmdKind::TurnOff); ipc_only = true; continue; }
        if (a == "--toggle")       { SetCommand(res, CmdKind::Toggle);  ipc_only = true; continue; }
        if (a == "--status")       { SetCommand(res, CmdKind::GetStatus); ipc_only = true; continue; }
        if (a == "--quit")         { SetCommand(res, CmdKind::Quit);      ipc_only = true; continue; }
        if (a == "--force-quit")   { SetCommand(res, CmdKind::ForceQuit); ipc_only = true; continue; }
        if (a == "--reload-config"){ SetCommand(res, CmdKind::ReloadConfig); ipc_only = true; continue; }
        if (a == "--enable-controller")  { SetCommand(res, CmdKind::EnableController);  ipc_only = true; continue; }
        if (a == "--disable-controller") { SetCommand(res, CmdKind::DisableController); ipc_only = true; continue; }

        if (MatchFlag(a, "--zoom", v) || a == "--zoom") {
            const auto raw = v ? *v : NextValue(args, i).value_or("");
            const auto f = ParseFloat(raw);
            if (!f) {
                res.mode = CliResult::Mode::ParseError;
                res.error_message = "--zoom requires a number, got: " + raw;
                return res;
            }
            SetCommand(res, CmdKind::SetZoom);
            res.startup_command->f_value = *f;
            continue;
        }
        if (MatchFlag(a, "--zoom-in", v) || a == "--zoom-in") {
            const auto raw = v ? *v : NextValue(args, i).value_or("");
            float step = 0.25f;
            if (!raw.empty()) {
                const auto f = ParseFloat(raw);
                if (!f) {
                    res.mode = CliResult::Mode::ParseError;
                    res.error_message = "--zoom-in step must be a number: " + raw;
                    return res;
                }
                step = *f;
            }
            SetCommand(res, CmdKind::ZoomDelta);
            res.startup_command->f_value = +step;
            ipc_only = true;
            continue;
        }
        if (MatchFlag(a, "--zoom-out", v) || a == "--zoom-out") {
            const auto raw = v ? *v : NextValue(args, i).value_or("");
            float step = 0.25f;
            if (!raw.empty()) {
                const auto f = ParseFloat(raw);
                if (!f) {
                    res.mode = CliResult::Mode::ParseError;
                    res.error_message = "--zoom-out step must be a number: " + raw;
                    return res;
                }
                step = *f;
            }
            SetCommand(res, CmdKind::ZoomDelta);
            res.startup_command->f_value = -step;
            ipc_only = true;
            continue;
        }
        if (MatchFlag(a, "--lens-size", v) || a == "--lens-size") {
            const auto raw = v ? *v : NextValue(args, i).value_or("");
            const auto xpos = raw.find_first_of("xX*");
            if (xpos == std::string::npos) {
                res.mode = CliResult::Mode::ParseError;
                res.error_message = "--lens-size expects WxH, got: " + raw;
                return res;
            }
            const auto w = ParseInt(std::string_view(raw).substr(0, xpos));
            const auto h = ParseInt(std::string_view(raw).substr(xpos + 1));
            if (!w || !h || *w <= 0 || *h <= 0) {
                res.mode = CliResult::Mode::ParseError;
                res.error_message = "--lens-size values must be positive ints: " + raw;
                return res;
            }
            SetCommand(res, CmdKind::SetLensSize);
            res.startup_command->i_value  = *w;
            res.startup_command->i_value2 = *h;
            continue;
        }
        if (MatchFlag(a, "--monitor", v) || a == "--monitor") {
            const auto raw = v ? *v : NextValue(args, i).value_or("");
            const auto idx = ParseInt(raw);
            if (!idx) {
                res.mode = CliResult::Mode::ParseError;
                res.error_message = "--monitor expects an integer: " + raw;
                return res;
            }
            SetCommand(res, CmdKind::SetMonitorIndex);
            res.startup_command->i_value = *idx;
            continue;
        }
        if (MatchFlag(a, "--profile", v) || a == "--profile") {
            const auto raw = v ? *v : NextValue(args, i).value_or("");
            if (raw.empty()) {
                res.mode = CliResult::Mode::ParseError;
                res.error_message = "--profile requires a name";
                return res;
            }
            SetCommand(res, CmdKind::SwitchProfile);
            res.startup_command->s_value = raw;
            continue;
        }

        res.mode = CliResult::Mode::ParseError;
        res.error_message = "Unknown argument: " + a;
        return res;
    }

    if (ipc_only) res.mode = CliResult::Mode::ForwardOnly;
    return res;
}

} // namespace magnifier
