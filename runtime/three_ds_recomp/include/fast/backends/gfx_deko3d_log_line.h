#pragma once

// One logs/deko3d.txt line from a printf format (Fast::Deko3d::Log). Formatted
// into a stack buffer, and only a longer line formats again into a string of
// its length: a line is never cut (test.14's 512-byte buffer cut the tail of
// every `texture pack change` line at 511 characters). Header-only, checked on
// the host (oot3d_texture_pack_scale_tests).

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>

namespace Fast::Deko3d {

// Most lines fit; a `texture pack change` line is about 700 characters.
inline constexpr std::size_t kLogLineStackBytes = 1024U;

[[nodiscard]] inline std::string FormatLogLine(const char* format, va_list args) {
    char line[kLogLineStackBytes];
    va_list again;
    va_copy(again, args);
    const int length = std::vsnprintf(line, sizeof(line), format, args);
    if (length < 0) {
        va_end(again);
        return {};
    }
    if (static_cast<std::size_t>(length) < sizeof(line)) {
        va_end(again);
        return std::string(line, static_cast<std::size_t>(length));
    }
    std::string longer(static_cast<std::size_t>(length), '\0');
    std::vsnprintf(longer.data(), longer.size() + 1U, format, again);
    va_end(again);
    return longer;
}

} // namespace Fast::Deko3d
