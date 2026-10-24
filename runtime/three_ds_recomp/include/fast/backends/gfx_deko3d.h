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

// True on the Switch when config/renderer selects deko3d (read once, at the
// first call). Always false on other platforms.
bool Deko3dRendererSelected();

// The deko3d GfxRenderingAPI (Switch only; nullptr elsewhere).
GfxRenderingAPI* CreateDeko3dRenderingApi();
} // namespace Fast
