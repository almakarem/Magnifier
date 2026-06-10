#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace magnifier {

// Newline-delimited JSON stream reader. Append raw bytes via Feed(); each
// complete line (terminated by '\n', with optional preceding '\r') is
// emitted via the on_line callback. Carries over partial lines between
// Feed() calls. Lines longer than max_line_bytes are dropped with a warning
// to protect against DoS by a malicious pipe client.
class JsonLineReader {
public:
    using LineSink = std::function<void(std::string_view)>;

    explicit JsonLineReader(LineSink sink, size_t max_line_bytes = 64 * 1024)
        : sink_(std::move(sink)), max_(max_line_bytes) {}

    void Feed(const char* data, size_t n);

    // Discard any partial line (e.g. on disconnect).
    void Reset() { buf_.clear(); overflow_ = false; }

private:
    LineSink   sink_;
    std::string buf_;
    size_t     max_;
    bool       overflow_ = false;
};

} // namespace magnifier
