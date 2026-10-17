// Long-run heap traffic of the visual-frame path the render thread runs
// every guest frame with fps60: the accumulator closes a frame of draw plans,
// the native view, the prepared transition (matching) and the interpolated
// samples, and the previous frame is dropped. On the console the newlib heap
// arena grew from 631 to 1659 MB in 8.5 min while ~700 MB stayed in use:
// large blocks of varying size allocated and freed every frame fragment it.
// This test counts, per simulated frame, the allocations, their bytes and the
// large ones (>= 256 KiB) after a warm-up, and requires the large ones to be
// gone in steady state and the traffic not to grow with time.
#include "oot3d_native_pica_visual_frame.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <vector>

namespace {
std::atomic<uint64_t> gAllocatedBytes{0};
std::atomic<uint64_t> gAllocations{0};
std::atomic<uint64_t> gLargeAllocations{0};
constexpr std::size_t kLargeAllocation = 256U * 1024U;
} // namespace

void* operator new(std::size_t size) {
    gAllocatedBytes.fetch_add(size, std::memory_order_relaxed);
    gAllocations.fetch_add(1U, std::memory_order_relaxed);
    if (size >= kLargeAllocation) {
        gLargeAllocations.fetch_add(1U, std::memory_order_relaxed);
    }
    if (void* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

using namespace Oot3dNativeGame;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_native_pica_visual_frame_soak_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

constexpr size_t kFrames = 1800U;      // one minute of guest frames at 30 Hz
constexpr size_t kWarmup = 120U;
constexpr size_t kWindow = 300U;
constexpr uint32_t kTopColor = 0x18000000U;
constexpr uint32_t kDisplayOutput = 0x18300000U;

// Shared snapshot buffers, as the submission queue's caches hand them out.
struct Scene {
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> Vertices;
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> Indices;
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> Textures;

    Scene() {
        for (size_t index = 0; index < 64U; ++index) {
            Vertices.push_back(std::make_shared<const std::vector<uint8_t>>(
                4096U + index * 32U, static_cast<uint8_t>(index)));
            Indices.push_back(std::make_shared<const std::vector<uint8_t>>(
                768U, static_cast<uint8_t>(index)));
            Textures.push_back(std::make_shared<const std::vector<uint8_t>>(
                16384U, static_cast<uint8_t>(index)));
        }
    }
};

Oot3dPicaVulkanDrawPlan MakePlan(const Scene& scene, size_t index, uint64_t id,
                                 uint64_t frame) {
    Oot3dPicaVulkanDrawPlan plan;
    plan.SubmissionId = id;
    plan.CompositionDomain = Oot3dPicaCompositionDomain::Scene;
    plan.VertexShader.StateKey = 1U + index % 12U;
    plan.FragmentShader.StateKey = 100U + index % 9U;
    plan.State.Framebuffer.ColorPhysicalAddress = kTopColor;
    plan.State.Framebuffer.Width = 240U;
    plan.State.Framebuffer.Height = 400U;
    plan.State.Viewport.HalfWidth = 120.0F;
    plan.State.Viewport.HalfHeight = 200.0F;
    plan.State.VertexInput.PhysicalBaseAddress =
        0x20000000U + static_cast<uint32_t>(index) * 0x1000U;
    plan.Indexed = true;
    plan.IndicesAre16Bit = true;
    plan.VertexCount = 384U;
    // An object moving smoothly: its model-view rows change every frame.
    const float phase = static_cast<float>(frame) * 0.05F +
                        static_cast<float>(index);
    for (size_t row = 0; row < 12U; ++row) {
        for (size_t column = 0; column < 4U; ++column) {
            plan.VertexShader.Uniforms.Floats[row][column] =
                std::sin(phase + static_cast<float>(row * 4U + column));
        }
    }
    Oot3dPicaVulkanVertexBinding binding;
    binding.Binding = 0U;
    binding.ByteStride = 32U;
    binding.SharedBytes = scene.Vertices[index % scene.Vertices.size()];
    plan.VertexBindings.push_back(std::move(binding));
    if (index % 10U == 0U) {
        // A CPU-animated mesh: its vertices are this frame's own bytes.
        Oot3dPicaVulkanVertexBinding animated;
        animated.Binding = 1U;
        animated.ByteStride = 16U;
        animated.Bytes.assign(6144U, static_cast<uint8_t>(frame));
        plan.VertexBindings.push_back(std::move(animated));
    }
    plan.VertexAttributes.push_back({0U, 0U, Oot3dPicaVertexFormat::Float, 3U, 0U});
    plan.VertexAttributes.push_back({1U, 0U, Oot3dPicaVertexFormat::Float, 2U, 12U});
    plan.SharedIndexBytes = scene.Indices[index % scene.Indices.size()];
    Oot3dPicaVulkanTextureBinding texture;
    texture.Slot = 0U;
    texture.State.PhysicalAddress =
        0x21000000U + static_cast<uint32_t>(index % 32U) * 0x4000U;
    texture.State.Width = 64U;
    texture.State.Height = 64U;
    texture.SharedNativeBytes = scene.Textures[index % scene.Textures.size()];
    plan.Textures.push_back(std::move(texture));
    return plan;
}

struct Window {
    uint64_t Allocations = 0;
    uint64_t Bytes = 0;
    uint64_t Large = 0;
};

} // namespace

int main() {
    Scene scene;
    Oot3dPicaVisualFrameAccumulator accumulator;
    Oot3dPicaPreparedVisualTransition transition;
    Oot3dPicaVisualFrameViewSample sample;
    std::optional<Oot3dPicaVisualFrame> previous;
    std::optional<Oot3dPicaVisualFrame> latest;
    uint64_t nextId = 1U;
    uint64_t nextCompletion = 1U;
    size_t matched = 0U;
    size_t interpolated = 0U;
    std::vector<Window> windows;
    Window window;
    const std::array<uint32_t, 1> outputs{kDisplayOutput};
    using Clock = std::chrono::steady_clock;
    double closeSeconds = 0.0;
    double renderSeconds = 0.0;

    for (size_t frame = 0; frame < kFrames; ++frame) {
        const uint64_t allocationsBefore = gAllocations.load();
        const uint64_t bytesBefore = gAllocatedBytes.load();
        const uint64_t largeBefore = gLargeAllocations.load();

        // Guest thread: the frame's draws (180-220, varying like a scene),
        // its top-screen snapshot, and the frame close.
        const size_t draws = 180U + (frame * 7U) % 41U;
        for (size_t index = 0; index < draws; ++index) {
            accumulator.Append(MakePlan(scene, index, nextId++, frame));
        }
        Oot3dPicaDisplayTransferSubmission top;
        top.CompletionId = nextCompletion++;
        top.AfterDrawSubmissionId = nextId - 1U;
        top.InputPhysicalAddress = kTopColor;
        top.OutputPhysicalAddress = kDisplayOutput;
        accumulator.Append(top);
        const auto closeStart = Clock::now();
        auto finished = accumulator.FinishFrames(top, outputs);
        const auto renderStart = Clock::now();
        if (frame >= kWarmup) {
            closeSeconds += std::chrono::duration<double>(renderStart - closeStart).count();
        }
        Require(finished.size() == 1U, "one visual frame per guest frame");

        // Render thread: native view, matching against the latest frame,
        // two interpolated samples (fps60), then the frame becomes the latest
        // and the previous one is dropped.
        std::optional<Oot3dPicaVisualFrame> current(std::move(finished.back()));
        finished.clear();
        (void)ViewOot3dPicaVisualFrame(*current, sample, true);
        if (latest.has_value() && transition.Prepare(*latest, *current)) {
            matched += transition.Stats().MatchedDraws;
            for (int step = 0; step < 2; ++step) {
                if (transition.Sample(*latest, *current, 0.5F, sample)) {
                    interpolated += sample.InterpolatedDraws;
                }
            }
        }
        (void)ViewOot3dPicaVisualFrame(*current, sample);
        transition.Reset();
        if (latest.has_value()) previous = std::move(latest);
        latest = std::move(current);
        if (frame >= kWarmup) {
            renderSeconds += std::chrono::duration<double>(Clock::now() - renderStart).count();
        }

        if (frame >= kWarmup) {
            window.Allocations += gAllocations.load() - allocationsBefore;
            window.Bytes += gAllocatedBytes.load() - bytesBefore;
            window.Large += gLargeAllocations.load() - largeBefore;
            if ((frame - kWarmup + 1U) % kWindow == 0U) {
                windows.push_back(window);
                window = {};
            }
        }
    }

    Require(!windows.empty(), "no measurement window");
    for (size_t index = 0; index < windows.size(); ++index) {
        const auto& w = windows[index];
        std::printf("window %zu: per frame %.1f allocations, %.1f KiB, "
                    "%.2f large (>= 256 KiB)\n",
                    index, static_cast<double>(w.Allocations) / kWindow,
                    static_cast<double>(w.Bytes) / 1024.0 / kWindow,
                    static_cast<double>(w.Large) / kWindow);
    }
    std::printf("matched draws %zu, interpolated draws %zu\n", matched,
                interpolated);
    const double measured = static_cast<double>(kFrames - kWarmup);
    std::printf("host CPU per frame: close %.3f ms, render-side (views, "
                "matching, 2 samples, drop) %.3f ms\n",
                closeSeconds * 1000.0 / measured,
                renderSeconds * 1000.0 / measured);
    const auto& first = windows.front();
    const auto& last = windows.back();
    // Steady state: the plan arrays are recycled, so no frame allocates a
    // large block, and the traffic does not grow with time.
    Require(last.Large == 0U, "large allocations every frame in steady state");
    Require(last.Bytes <= first.Bytes + first.Bytes / 20U,
            "heap traffic per frame grows with time");
    Require(last.Allocations <= first.Allocations + first.Allocations / 20U,
            "allocations per frame grow with time");
    std::puts("oot3d_native_pica_visual_frame_soak_tests: ok");
    return 0;
}
