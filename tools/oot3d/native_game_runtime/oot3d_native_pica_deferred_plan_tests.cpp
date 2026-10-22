// Deferred PICA planning must produce exactly the plans of the serial build,
// whoever builds them (plan worker, render thread, dependency flush).
#include "oot3d_native_pica_deferred_plan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Heap allocations of this process: the console's malloc has one lock for
// every thread, so a plan build that allocates stalls the game and the
// renderer behind the low-priority plan worker (hang watchdog, 2026-10-22).
std::atomic<uint64_t> gAllocations{0};

void* operator new(std::size_t size) {
    gAllocations.fetch_add(1U, std::memory_order_relaxed);
    if (void* pointer = std::malloc(size != 0U ? size : 1U)) return pointer;
    throw std::bad_alloc();
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }

namespace {

using namespace Oot3dNativeGame;

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_native_pica_deferred_plan_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) Fail(message);
}

// Indexed draw into one of two color targets, one vertex loader, an optional
// texture carrying one GPU copy producer. `variant` changes uniforms and bytes.
Oot3dPicaDrawSubmission MakeSubmission(uint64_t id, uint32_t variant,
                                       uint32_t colorAddress, bool textured) {
    Oot3dPicaDrawSubmission submission;
    submission.Id = id;
    submission.MinimumVertexIndex = 2U;
    submission.MaximumVertexIndex = 5U;
    submission.Packet.Indexed = true;
    submission.Packet.CommandListAddress = 0x14001200U + variant * 4U;
    submission.Packet.CommandListOffsetWords = 0x34U + variant;
    submission.Packet.CompositionDomain = Oot3dPicaCompositionDomain::Scene;
    submission.Packet.VertexShader.Program[0] = 0x88000000U;
    submission.Packet.VertexShader.ProgramWordCount = 1U;
    submission.Packet.VertexShader.SwizzleWordCount = 1U;
    submission.Packet.VertexShader.FloatUniforms[3] = {
        static_cast<float>(variant), 0.5F, 0.25F, 1.0F};
    submission.Packet.Registers[0x04F] = 1U;
    submission.Packet.Registers[0x050] = 0x03020100U;
    submission.Packet.Registers[0x2BD] = 1U;
    submission.Packet.Registers[0x0C3U] = 0x44332211U + variant;
    submission.Packet.DefaultAttributes[1] = {1.0F, 2.0F, 3.0F,
                                              static_cast<float>(variant)};

    auto& state = submission.State;
    state.Framebuffer.ColorPhysicalAddress = colorAddress;
    state.Viewport.HalfWidth = 200.0F;
    state.Viewport.HalfHeight = 120.0F;
    state.VertexInput.Indexed = true;
    state.VertexInput.VertexCount = 3U;
    state.VertexInput.AttributeCount = 2U;
    state.VertexInput.Attributes[0] = {Oot3dPicaVertexFormat::Float, 3U, false};
    state.VertexInput.Attributes[1] = {Oot3dPicaVertexFormat::Float, 4U, true};
    state.VertexInput.Loaders[0].ByteStride = 12U;
    state.VertexInput.Loaders[0].ComponentCount = 1U;
    state.VertexInput.Loaders[0].Components[0] = 0U;
    state.ShaderInterface.OutputMask = 1U;
    state.ShaderInterface.InputRegisterByAttribute[0] = 0U;
    state.ShaderInterface.InputRegisterByAttribute[1] = 2U;

    Oot3dPicaResourceSnapshot indices;
    indices.Kind = Oot3dPicaResourceKind::IndexBuffer;
    indices.Bytes = {2U, 5U, static_cast<uint8_t>(3U + variant % 3U)};
    indices.ContentVersion = 11U + variant;
    indices.ContentVersionAvailable = true;
    submission.Resources.push_back(indices);
    Oot3dPicaResourceSnapshot vertices;
    vertices.Kind = Oot3dPicaResourceKind::VertexLoader;
    vertices.FirstElement = 2U;
    vertices.Bytes.assign(48U, static_cast<uint8_t>(variant));
    vertices.ContentVersion = 100U + variant;
    vertices.ContentVersionAvailable = true;
    submission.Resources.push_back(vertices);
    if (textured) {
        state.Textures[0].Enabled = true;
        state.Textures[0].Width = 16U;
        state.Textures[0].Height = 16U;
        state.Textures[0].PhysicalAddress = 0x18000000U + variant * 0x400U;
        Oot3dPicaResourceSnapshot texture;
        texture.Kind = Oot3dPicaResourceKind::Texture;
        texture.Slot = 0U;
        texture.Bytes.assign(1024U, static_cast<uint8_t>(0x10U + variant));
        texture.CopyReferences.push_back(
            {7U + variant, id - 1U, state.Textures[0].PhysicalAddress, 1024U});
        submission.Resources.push_back(texture);
    }
    return submission;
}

std::vector<Oot3dPicaDrawSubmission> MakeSubmissions(uint64_t firstId,
                                                     size_t count) {
    std::vector<Oot3dPicaDrawSubmission> result;
    for (size_t index = 0; index < count; ++index) {
        result.push_back(MakeSubmission(
            firstId + index, static_cast<uint32_t>(index % 5U),
            index % 4U == 3U ? 0x1F200000U : 0x1F000000U, index % 2U == 0U));
    }
    return result;
}

void RequireSamePlan(const Oot3dPicaVulkanDrawPlan& expected,
                     const Oot3dPicaVulkanDrawPlan& actual,
                     std::string_view what) {
    const auto fail = [&](std::string_view field) {
        Fail(std::string(what) + ": " + std::string(field) + " differs");
    };
    if (actual.Deferred != nullptr) fail("resolution");
    if (expected.SubmissionId != actual.SubmissionId) fail("submission id");
    if (expected.CommandListAddress != actual.CommandListAddress ||
        expected.CommandListOffsetWords != actual.CommandListOffsetWords)
        fail("command list");
    if (expected.VertexShader.StateKey != actual.VertexShader.StateKey ||
        expected.FragmentShader.StateKey != actual.FragmentShader.StateKey)
        fail("shader keys");
    if (expected.ResolvedVertexShaderSource() !=
            actual.ResolvedVertexShaderSource() ||
        expected.ResolvedFragmentShaderSource() !=
            actual.ResolvedFragmentShaderSource())
        fail("shader sources");
    if (expected.VertexShader.Uniforms.Floats !=
            actual.VertexShader.Uniforms.Floats ||
        expected.VertexShader.Uniforms.Integers !=
            actual.VertexShader.Uniforms.Integers ||
        expected.VertexShader.Uniforms.BooleanMask !=
            actual.VertexShader.Uniforms.BooleanMask)
        fail("vertex uniforms");
    if (expected.FragmentShader.Uniforms.TevConstants !=
        actual.FragmentShader.Uniforms.TevConstants)
        fail("fragment uniforms");
    if (expected.GeometryIdentity != actual.GeometryIdentity ||
        expected.GeometryContentVersion != actual.GeometryContentVersion ||
        expected.GeometryIdentityAvailable != actual.GeometryIdentityAvailable)
        fail("geometry contract");
    if (expected.BaseVertex != actual.BaseVertex ||
        expected.VertexCount != actual.VertexCount ||
        expected.Indexed != actual.Indexed)
        fail("draw range");
    if (!std::ranges::equal(expected.ResolvedIndexBytes(),
                            actual.ResolvedIndexBytes()))
        fail("index bytes");
    if (expected.VertexBindings.size() != actual.VertexBindings.size())
        fail("vertex binding count");
    for (size_t index = 0; index < expected.VertexBindings.size(); ++index) {
        const auto& a = expected.VertexBindings[index];
        const auto& b = actual.VertexBindings[index];
        if (a.ByteStride != b.ByteStride || a.InputRate != b.InputRate ||
            a.ContentVersion != b.ContentVersion ||
            !std::ranges::equal(a.ResolvedBytes(), b.ResolvedBytes()))
            fail("vertex binding");
    }
    if (expected.VertexAttributes.size() != actual.VertexAttributes.size())
        fail("vertex attribute count");
    if (expected.Textures.size() != actual.Textures.size())
        fail("texture count");
    for (size_t index = 0; index < expected.Textures.size(); ++index) {
        const auto& a = expected.Textures[index];
        const auto& b = actual.Textures[index];
        if (a.Slot != b.Slot ||
            !std::ranges::equal(a.ResolvedNativeBytes(),
                                b.ResolvedNativeBytes()) ||
            a.NativeContentHash != b.NativeContentHash ||
            a.NativeContentHashAvailable != b.NativeContentHashAvailable ||
            a.CopyReferences.size() != b.CopyReferences.size())
            fail("texture");
    }
    if (ComputeOot3dPicaVisualDrawIdentity(expected) !=
        ComputeOot3dPicaVisualDrawIdentity(actual))
        fail("visual identity");
}

std::shared_ptr<Oot3dPicaDeferredPlanBatch> BeginOwned(
    Oot3dPicaDeferredPlanBuilder& builder,
    std::vector<Oot3dPicaDrawSubmission> submissions) {
    return builder.BeginBatch(submissions);
}

std::vector<Oot3dPicaVulkanDrawPlan> SerialPlans(
    std::vector<Oot3dPicaDrawSubmission> submissions) {
    Oot3dPicaVulkanShaderSourceCache cache;
    Oot3dPicaDeferredPlanBuilder builder(cache);
    std::vector<Oot3dPicaVulkanDrawPlan> plans(submissions.size());
    std::string error;
    for (size_t index = 0; index < submissions.size(); ++index) {
        Require(builder.Build(submissions[index], plans[index], &error), error);
    }
    Require(builder.Stats().SerialBuilds == submissions.size(),
            "serial builds were not counted");
    return plans;
}

// The guest-side sequence of the drain loop: placeholders into the
// accumulator, publish, close a frame, resolve it where the renderer would.
void TestDeferredMatchesSerial(bool worker) {
    constexpr size_t kDraws = 24U;
    const auto expected = SerialPlans(MakeSubmissions(1U, kDraws));
    Oot3dPicaVulkanShaderSourceCache cache;
    Oot3dPicaDeferredPlanBuilder builder(cache);
    builder.Configure(true, worker);
    Require(builder.Deferred() && builder.WorkerEnabled() == worker,
            "builder configuration");
    auto batch = BeginOwned(builder, MakeSubmissions(1U, kDraws));
    Oot3dPicaVisualFrameAccumulator accumulator;
    std::vector<Fast::Renderer3ds::PicaTextureCopyReference> copies;
    for (size_t index = 0; index < kDraws; ++index) {
        auto placeholder = builder.MakeDeferredPlan(batch, index);
        Require(placeholder.Deferred != nullptr &&
                    placeholder.SubmissionId == expected[index].SubmissionId &&
                    placeholder.State.Framebuffer.ColorPhysicalAddress ==
                        expected[index].State.Framebuffer.ColorPhysicalAddress &&
                    placeholder.State.Viewport.HalfWidth ==
                        expected[index].State.Viewport.HalfWidth,
                "placeholder does not carry the guest-side frame fields");
        copies.clear();
        CollectOot3dPicaPlanCopyReferences(placeholder, copies);
        std::vector<Fast::Renderer3ds::PicaTextureCopyReference> expectedCopies;
        CollectOot3dPicaPlanCopyReferences(expected[index], expectedCopies);
        Require(copies.size() == expectedCopies.size() &&
                    std::equal(copies.begin(), copies.end(),
                               expectedCopies.begin(),
                               [](const auto& a, const auto& b) {
                                   return a.CompletionId == b.CompletionId &&
                                          a.PhysicalAddress == b.PhysicalAddress &&
                                          a.ByteCount == b.ByteCount;
                               }),
                "placeholder does not pin the copies its plan will reference");
        accumulator.Append(std::move(placeholder));
    }
    builder.Publish(std::move(batch));

    // A dependency flush resolves a prefix in place.
    const auto* firstPending = accumulator.PendingDraws().data();
    std::string error;
    Require(accumulator.ResolveDeferredDraws(
                5U,
                [&](Oot3dPicaVulkanDrawPlan& plan) {
                    return ResolveOot3dPicaDeferredDrawPlan(plan, &error);
                }),
            error);
    Require(accumulator.PendingDraws().data() == firstPending,
            "prefix resolution moved the pending draws");
    for (const auto& draw : accumulator.PendingDraws()) {
        Require((draw.SubmissionId <= 5U) == (draw.Deferred == nullptr),
                "prefix resolution resolved the wrong draws");
    }

    Oot3dPicaDisplayTransferSubmission top;
    top.CompletionId = 1U;
    top.AfterDrawSubmissionId = kDraws;
    top.InputPhysicalAddress = 0x1F000000U;
    top.OutputPhysicalAddress = 0x1E000000U;
    auto frames = accumulator.FinishFrames(top, {});
    Require(frames.size() == 1U && frames[0].Draws.size() == kDraws,
            "frame accounting changed with placeholders");
    Require(frames[0].StrictDrawIdentities.empty(),
            "a frame with placeholders must leave identities to resolution");
    // Resolution may run while the worker is still building.
    Require(ResolveOot3dPicaDeferredVisualFrame(frames[0], &error), error);
    Require(frames[0].StrictDrawIdentities.size() == kDraws,
            "resolution did not complete the strict identities");
    for (size_t index = 0; index < kDraws; ++index) {
        RequireSamePlan(expected[index], frames[0].Draws[index],
                        worker ? "worker" : "render thread");
        Require(frames[0].StrictDrawIdentities[index] ==
                    ComputeOot3dPicaVisualDrawIdentity(expected[index]),
                "strict identity differs");
    }
    // Resolving again is a no-op for built plans.
    Require(ResolveOot3dPicaDeferredVisualFrame(frames[0], &error), error);
    // The worker publishes its build count once per batch (no per-draw
    // atomics on the A57): give it until the batch's end to do so.
    auto stats = builder.Stats();
    for (int wait = 0; wait < 2000 && stats.WorkerBuilds + stats.ConsumerBuilds != kDraws; ++wait) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        stats = builder.Stats();
    }
    Require(stats.DeferredDraws == kDraws &&
                stats.WorkerBuilds + stats.ConsumerBuilds == kDraws &&
                (worker || stats.WorkerBuilds == 0U),
            "deferred builds were not counted once each");
}

void TestSkippedAndFailedSlots() {
    Oot3dPicaVulkanShaderSourceCache cache;
    Oot3dPicaDeferredPlanBuilder builder(cache);
    builder.Configure(true, true);
    auto submissions = MakeSubmissions(40U, 3U);
    // No vertex snapshot: the serial build fails, so must the deferred one.
    submissions[2].Resources.erase(submissions[2].Resources.begin() + 1);
    auto batch = builder.BeginBatch(submissions);
    auto first = builder.MakeDeferredPlan(batch, 0U);
    // Slot 1 is never planned (a suppressed draw): the worker skips it.
    auto broken = builder.MakeDeferredPlan(batch, 2U);
    builder.Publish(std::move(batch));
    std::string error;
    Require(ResolveOot3dPicaDeferredDrawPlan(first, &error), error);
    Require(!ResolveOot3dPicaDeferredDrawPlan(broken, &error) &&
                error.find("no vertex snapshot") != std::string::npos,
            "a failed deferred build must report the serial error");
    builder.StopWorker();
    Require(builder.Stats().DeferredDraws == 2U &&
                builder.Stats().WorkerBuilds + builder.Stats().ConsumerBuilds <= 2U,
            "a skipped slot was built");
}

void TestUnpublishedAndStoppedWorker() {
    Oot3dPicaVulkanShaderSourceCache cache;
    Oot3dPicaDeferredPlanBuilder builder(cache);
    builder.Configure(true, true);
    auto batch = BeginOwned(builder, MakeSubmissions(60U, 2U));
    auto early = builder.MakeDeferredPlan(batch, 0U);
    std::string error;
    Require(!ResolveOot3dPicaDeferredDrawPlan(early, &error),
            "an unpublished placeholder must not resolve");
    auto batch2 = BeginOwned(builder, MakeSubmissions(70U, 64U));
    std::vector<Oot3dPicaVulkanDrawPlan> plans;
    for (size_t index = 0; index < 64U; ++index)
        plans.push_back(builder.MakeDeferredPlan(batch2, index));
    builder.Publish(std::move(batch2));
    // Stopping the worker mid-batch leaves the rest to the consumer.
    builder.StopWorker();
    Require(!builder.WorkerEnabled(), "worker still enabled after stop");
    for (auto& plan : plans) {
        Require(ResolveOot3dPicaDeferredDrawPlan(plan, &error), error);
        Require(plan.Deferred == nullptr && !plan.VertexBindings.empty(),
                "plan after a stopped worker");
    }
}

void TestConcurrentBatches() {
    // Many drain passes: the worker builds ahead while the consumer resolves
    // newest-first, sharing one shader cache.
    Oot3dPicaVulkanShaderSourceCache cache;
    Oot3dPicaDeferredPlanBuilder builder(cache);
    builder.Configure(true, true);
    const auto expected = SerialPlans(MakeSubmissions(1000U, 16U));
    std::vector<std::vector<Oot3dPicaVulkanDrawPlan>> passes;
    for (size_t pass = 0; pass < 40U; ++pass) {
        auto batch = BeginOwned(builder, MakeSubmissions(1000U, 16U));
        std::vector<Oot3dPicaVulkanDrawPlan> plans;
        for (size_t index = 0; index < 16U; ++index)
            plans.push_back(builder.MakeDeferredPlan(batch, index));
        builder.Publish(std::move(batch));
        passes.push_back(std::move(plans));
    }
    std::string error;
    for (auto pass = passes.rbegin(); pass != passes.rend(); ++pass) {
        for (size_t index = 0; index < pass->size(); ++index) {
            Require(ResolveOot3dPicaDeferredDrawPlan((*pass)[index], &error), error);
            RequireSamePlan(expected[index], (*pass)[index], "concurrent");
        }
    }
}

} // namespace

constexpr uint64_t kMaxPlanAllocations = 5U;  // medido em 2026-10-22
constexpr uint64_t kMaxPlanAllocationsTextured = 5U;

// Allocations of one plan build once the shader cache is warm. The
// submission's payloads move into the plan; what is left is the plan's own
// vectors. A rise here is heap traffic on every draw of every frame.
void TestPlanBuildAllocations() {
    Oot3dPicaVulkanShaderSourceCache cache;
    Oot3dPicaDeferredPlanBuilder builder(cache);
    std::string error;
    Oot3dPicaVulkanDrawPlan plan;
    for (bool textured : {false, true}) {
        auto warm = MakeSubmission(1U, 1U, 0x1F000000U, textured);
        Require(builder.Build(warm, plan, &error), error);
        auto submission = MakeSubmission(2U, 1U, 0x1F000000U, textured);
        const uint64_t before = gAllocations.load();
        Require(builder.Build(submission, plan, &error), error);
        const uint64_t spent = gAllocations.load() - before;
        std::cout << "plan build allocations (textured=" << textured << "): " << spent << '\n';
        Require(spent <= (textured ? kMaxPlanAllocationsTextured : kMaxPlanAllocations),
                "plan build allocates more than before");
    }
}

int main() {
    TestPlanBuildAllocations();
    TestDeferredMatchesSerial(true);
    TestDeferredMatchesSerial(false);
    TestSkippedAndFailedSlots();
    TestUnpublishedAndStoppedWorker();
    TestConcurrentBatches();
    std::cout << "oot3d_native_pica_deferred_plan_tests: ok\n";
    return 0;
}
