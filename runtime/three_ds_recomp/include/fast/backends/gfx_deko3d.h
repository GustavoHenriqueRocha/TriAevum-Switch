#pragma once

// deko3d rendering backend (Switch): selected at run time by
// sdmc:/switch/oot3dre/config/renderer containing "deko3d". Without the file
// (or with "gl") the OpenGL/Mesa backend stays in use, unchanged.
// docs/DEKO3D_PROGRESSO.md has the design and the package status.

#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

struct ImDrawData;

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
inline constexpr const char* kDeko3dBuildTag = "p6n";

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

// Draws ImGui's draw data on the deko3d window (FPS counter, windows), in
// renderer order: called on the render thread with a copy of the frame's
// data. Does nothing without an initialized deko3d backend.
void Deko3dRenderImGuiDrawData(const ImDrawData* data);

// The "dkperf ..." line of logs/perf.txt (the deko3d counterpart of glperf,
// tools/oot3d/compare_backends.py): GPU time per presented frame since the
// last call, from timestamps at the start and the end of each frame. Empty
// without a deko3d device.
std::string Deko3dPerfReport(unsigned long long hostFrames);

// GPU busy time of one presented frame (the acquire wait left out), for the
// pacing governor and dynamic resolution, which on GL get it from
// NativeGpuFrameTimer. Render thread: Deko3dTagFrame before the frame's
// present names its kind and render-thread cost; Deko3dTakeGpuFrame returns
// the measured frames, oldest first, once their fence passed (two frames
// later). Nothing elsewhere or without a deko3d device.
struct Deko3dGpuFrame {
    uint8_t Kind = 0;
    double Milliseconds = 0.0;
    double CpuMilliseconds = 0.0;
};
void Deko3dTagFrame(uint8_t kind, double cpuMilliseconds);
bool Deko3dTakeGpuFrame(Deko3dGpuFrame& frame);
} // namespace Fast
