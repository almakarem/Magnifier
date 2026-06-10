#pragma once

#include <string>
#include <string_view>

namespace magnifier {

// UTF-8 <-> UTF-16 conversions using Win32 (no locale dependency).
std::wstring Utf8ToWide(std::string_view utf8);
std::string  WideToUtf8(std::wstring_view wide);

} // namespace magnifier
