#pragma once

// deko3d rendering backend (Switch): selected at run time by
// sdmc:/switch/oot3dre/config/renderer containing "deko3d". Without the file
// (or with "gl") the OpenGL/Mesa backend stays in use, unchanged.
// docs/DEKO3D_PROGRESSO.md has the design and the package status.

#include <cctype>
#include <string_view>

namespace Fast {
class GfxRenderingAPI;

// Pure: the selection rule for the content of config/renderer (first line,
// surrounding blanks ignored, case-insensitive). Unit-tested on the host.
inline bool Deko3dRendererConfigSelects(std::string_view content) {
    const size_t newline = content.find_first_of("\r\n");
    if (newline != std::string_view::npos) {
        content = content.substr(0, newline);
    }
    while (!content.empty() && std::isspace(static_cast<unsigned char>(content.front()))) {
        content.remove_prefix(1);
    }
    while (!content.empty() && std::isspace(static_cast<unsigned char>(content.back()))) {
        content.remove_suffix(1);
    }
    constexpr std::string_view kName = "deko3d";
    if (content.size() != kName.size()) {
        return false;
    }
    for (size_t index = 0; index < kName.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(content[index])) != kName[index]) {
            return false;
        }
    }
    return true;
}

// Build tag written at the top of logs/deko3d.txt: tells which NRO a log
// came from (a p3 log and a p5 log looked alike, 24/10).
inline constexpr const char* kDeko3dBuildTag = "p5c";

// Presented frames logged in logs/deko3d.txt: the first, then 60, 300 and
// 1800 (the PICA counters go with 300 and every 1800), then every 3600.
[[nodiscard]] constexpr bool Deko3dFrameLogDue(unsigned long long frame) noexcept {
    return frame == 1U || frame == 60U || frame == 300U || frame == 1800U || (frame != 0U && frame % 3600U == 0U);
}
[[nodiscard]] constexpr bool Deko3dPicaLogDue(unsigned long long frame) noexcept {
    return frame == 300U || (frame != 0U && frame % 1800U == 0U);
}

// True on the Switch when config/renderer selects deko3d (read once, at the
// first call). Always false on other platforms.
bool Deko3dRendererSelected();

// The deko3d GfxRenderingAPI (Switch only; nullptr elsewhere).
GfxRenderingAPI* CreateDeko3dRenderingApi();
} // namespace Fast
