#pragma once

#include "ipc/Commands.h"

#include <optional>
#include <string>
#include <vector>

namespace magnifier {

// Result of parsing argv. The first parsed command (if any) is what we
// forward to a running instance / execute locally. `print_help`,
// `print_version` and `print_status` short-circuit the GUI message loop.
struct CliResult {
    enum class Mode {
        Normal,            // proceed to normal startup, applying any startup_command
        ForwardOnly,       // command only — exit after forwarding to running instance
        PrintHelp,
        PrintVersion,
        ParseError,
    };
    Mode                    mode             = Mode::Normal;
    bool                    start_minimized  = false;
    std::optional<Command>  startup_command; // applied locally if we ARE the instance
    std::string             error_message;   // populated on ParseError
};

CliResult ParseCli(int argc, wchar_t** argv);

// Human-readable help text, printed to stdout for --help.
const char* HelpText();

} // namespace magnifier
