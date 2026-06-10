#pragma once

namespace magnifier::crash {

// Installs an SEH unhandled-exception filter that writes a minidump to
// %LOCALAPPDATA%\Magnifier\crashes\ before terminating. Idempotent.
void Install();

} // namespace magnifier::crash
