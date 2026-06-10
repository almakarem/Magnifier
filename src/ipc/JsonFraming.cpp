#include "ipc/JsonFraming.h"
#include "util/Log.h"

namespace magnifier {

void JsonLineReader::Feed(const char* data, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const char c = data[i];
        if (c == '\n') {
            if (overflow_) {
                spdlog::warn("JsonLineReader: dropping over-long line ({} bytes)",
                             buf_.size());
                overflow_ = false;
            } else {
                std::string_view line(buf_);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                if (!line.empty() && sink_) sink_(line);
            }
            buf_.clear();
            continue;
        }
        if (buf_.size() < max_) {
            buf_.push_back(c);
        } else {
            overflow_ = true;
        }
    }
}

} // namespace magnifier
