// "Piscada" ao girar a câmera rápido com fps60 (native30_interpolated).
//
// The game culls its world pieces with the camera of each guest step. While
// the camera turns fast, every step adds pieces at one screen edge and drops
// pieces at the other. The delayed fps60 presentation shows, per guest step,
// a step frame (alpha 0, the previous step's time) and an in-between frame
// (alpha 1/2), both sampled over the CURRENT frame's draw list:
//   - a piece the game dropped in the current step was missing from both
//     (a hole at the trailing edge for two refreshes, ~33 ms);
//   - a piece the game added in the current step was drawn with the current
//     step's camera: a whole step away from the rest at alpha 0, half a step
//     at alpha 1/2.
// At a pure 30 FPS the guest frame is drawn as is, so nothing of this shows.
//
// This test spins a camera at 9 degrees per guest step over a ring of world
// pieces culled like the game (camera in the view rows, and a producer that
// precomposes the camera into the model rows), presents every transition the
// way the window loop does and checks each presented piece against where it
// should be at that instant. Legacy sampling must show the defect; with
// Oot3dPicaVisualTransitionOptions (ExactStep + CarryUnmatched) there must
// be none.
#include "oot3d_native_pica_visual_frame.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace Oot3dNativeGame;

constexpr uint32_t kTopColor = 0x18000000U;
constexpr size_t kPieces = 72;
constexpr double kPi = 3.14159265358979323846;
constexpr double kStepDegrees = 9.0;    // ~270 degrees/s at 30 guest steps
constexpr double kVisibleDegrees = 34.0;  // half field of view + margin

int gFailures = 0;

void Check(bool condition, const std::string& message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message.c_str());
    if (!condition) ++gFailures;
}

using Rows3 = std::array<std::array<double, 4>, 3>;

Rows3 Multiply(const Rows3& a, const Rows3& b) {
    Rows3 out{};
    for (size_t r = 0; r < 3; ++r)
        for (size_t c = 0; c < 4; ++c) {
            double v = c == 3 ? a[r][3] : 0.0;
            for (size_t j = 0; j < 3; ++j) v += a[r][j] * b[j][c];
            out[r][c] = v;
        }
    return out;
}

// World -> camera for a camera at the origin looking down -Z rotated by yaw.
Rows3 CameraRows(double yawDegrees) {
    const double y = yawDegrees * kPi / 180.0;
    const double c = std::cos(y), s = std::sin(y);
    // Inverse of a rotation about Y by yaw.
    return {{{c, 0, -s, 0}, {0, 1, 0, -1.5}, {s, 0, c, 0}}};
}

double PieceDegrees(size_t piece) { return 360.0 * static_cast<double>(piece) / kPieces; }

Rows3 PieceModel(size_t piece) {
    const double a = PieceDegrees(piece) * kPi / 180.0;
    // At yaw a the camera looks toward (sin a, 0, -cos a).
    return {{{1, 0, 0, 20.0 * std::sin(a)}, {0, 1, 0, 0}, {0, 0, 1, -20.0 * std::cos(a)}}};
}

bool Visible(size_t piece, double yawDegrees) {
    double d = std::fmod(PieceDegrees(piece) - yawDegrees, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d < -180.0) d += 360.0;
    return std::abs(d) < kVisibleDegrees;
}

std::shared_ptr<const Oot3dPicaTemporalVertexProgram> Program() {
    auto program = std::make_shared<Oot3dPicaTemporalVertexProgram>();
    auto& t = program->Hooks.Transform;
    t.Operation = Fast::Renderer3ds::PicaVertexTransformOperation::ModelViewProjection3x4;
    t.ProjectionFirstUniform = 0;
    t.ProjectionRowCount = 4;
    t.ViewFirstUniform = 4;
    t.ViewRowCount = 3;
    t.ModelFirstUniform = 8;
    t.ModelRowCount = 3;
    t.PositionInputRegister = 0;
    t.NormalInputRegister = 1;
    return program;
}

void Put(Oot3dPicaVertexUniformState& u, size_t first, const Rows3& rows) {
    for (size_t r = 0; r < 3; ++r)
        for (size_t c = 0; c < 4; ++c) u.Floats[first + r][c] = static_cast<float>(rows[r][c]);
}

struct Scene {
    bool Precomposed = false;  // camera folded into the model rows
    std::shared_ptr<const Oot3dPicaTemporalVertexProgram> TemporalProgram = Program();
    uint64_t NextSubmission = 1;

    Rows3 ViewRows(double yaw) const {
        return Precomposed ? Rows3{{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}}} : CameraRows(yaw);
    }
    Rows3 ModelRows(size_t piece, double yaw) const {
        return Precomposed ? Multiply(CameraRows(yaw), PieceModel(piece)) : PieceModel(piece);
    }

    Oot3dPicaVisualFrame Frame(uint64_t sequence, double yaw) {
        Oot3dPicaVisualFrame frame;
        frame.Sequence = sequence;
        frame.TopTransfer.InputPhysicalAddress = kTopColor;
        frame.TopTransfer.OutputPhysicalAddress = 0x1F000000U;
        for (size_t piece = 0; piece < kPieces; ++piece) {
            if (!Visible(piece, yaw)) continue;
            Oot3dPicaVulkanDrawPlan plan;
            plan.SubmissionId = NextSubmission++;
            plan.CompositionDomain = Oot3dPicaCompositionDomain::Scene;
            plan.Composition.Layer = Oot3dPicaCompositionLayer::OpaqueWorld;
            plan.VertexShader.StateKey = 11;
            plan.FragmentShader.StateKey = 12;
            plan.VertexShader.TemporalProgram = TemporalProgram;
            plan.State.Framebuffer.ColorPhysicalAddress = kTopColor;
            plan.State.Framebuffer.DepthPhysicalAddress = kTopColor + 0x100000U;
            plan.State.Framebuffer.Width = 240U;
            plan.State.Framebuffer.Height = 400U;
            plan.State.Viewport.HalfWidth = 120.0F;
            plan.State.Viewport.HalfHeight = 200.0F;
            plan.State.Viewport.DepthRange = 1.0F;
            // Distinct meshes: no two pieces share an identity.
            plan.VertexCount = static_cast<uint32_t>(100U + piece);
            auto& u = plan.VertexShader.Uniforms;
            u.Floats[0] = {1.2F, 0, 0, 0};
            u.Floats[1] = {0, 2.1F, 0, 0};
            u.Floats[2] = {0, 0, -1.0F, -2.0F};
            u.Floats[3] = {0, 0, -1.0F, 0};
            Put(u, 4, ViewRows(yaw));
            Put(u, 8, ModelRows(piece, yaw));
            frame.Draws.push_back(std::move(plan));
        }
        return frame;
    }
};

// The piece's origin in camera space as the sample would draw it.
std::array<double, 3> DrawnOrigin(const Oot3dPicaVisualDrawView& view) {
    const auto& floats =
        view.VertexUniforms ? view.VertexUniforms->Floats : view.BasePlan->VertexShader.Uniforms.Floats;
    Rows3 v{}, m{};
    for (size_t r = 0; r < 3; ++r)
        for (size_t c = 0; c < 4; ++c) {
            v[r][c] = floats[4 + r][c];
            m[r][c] = floats[8 + r][c];
        }
    const auto composed = Multiply(v, m);
    return {composed[0][3], composed[1][3], composed[2][3]};
}

// Where a piece must be at alpha between the two steps: the matched draws'
// rule (rows interpolated component by component).
std::array<double, 3> ExpectedOrigin(size_t piece, double yawBefore, double yawAfter, double alpha) {
    const auto before = Multiply(CameraRows(yawBefore), PieceModel(piece));
    const auto after = Multiply(CameraRows(yawAfter), PieceModel(piece));
    std::array<double, 3> out{};
    for (size_t r = 0; r < 3; ++r) out[r] = before[r][3] + (after[r][3] - before[r][3]) * alpha;
    return out;
}

struct Defects {
    size_t Missing = 0;    // a piece the game drew at that time, absent
    size_t Displaced = 0;  // drawn, but not where the camera of that time sees it
    size_t Extra = 0;      // drawn although the game drew it in neither step
    size_t Samples = 0;
    size_t Draws = 0;
    [[nodiscard]] size_t Total() const { return Missing + Displaced + Extra; }
};

void Inspect(const Oot3dPicaVisualFrameViewSample& sample, double yawBefore, double yawAfter, double alpha,
             Defects& defects) {
    ++defects.Samples;
    std::set<size_t> drawn;
    for (const auto& view : sample.Draws) {
        const size_t piece = view.BasePlan->VertexCount - 100U;
        ++defects.Draws;
        drawn.insert(piece);
        const bool expected =
            alpha <= 0.0 ? Visible(piece, yawBefore) : (Visible(piece, yawBefore) || Visible(piece, yawAfter));
        if (!expected) {
            ++defects.Extra;
            continue;
        }
        const auto at = DrawnOrigin(view);
        const auto want = ExpectedOrigin(piece, yawBefore, yawAfter, alpha);
        double error = 0;
        for (size_t i = 0; i < 3; ++i) error = std::max(error, std::abs(at[i] - want[i]));
        if (error > 1.0e-2) ++defects.Displaced;
    }
    for (size_t piece = 0; piece < kPieces; ++piece) {
        const bool must =
            alpha <= 0.0 ? Visible(piece, yawBefore) : (Visible(piece, yawBefore) || Visible(piece, yawAfter));
        if (must && !drawn.count(piece)) ++defects.Missing;
    }
}

Defects Spin(bool precomposed, const Oot3dPicaVisualTransitionOptions& options, size_t steps,
             Oot3dPicaVisualUnmatchedStats* lastStats = nullptr) {
    Scene scene;
    scene.Precomposed = precomposed;
    Oot3dPicaPreparedVisualTransition transition;
    transition.SetOptions(options);
    Oot3dPicaVisualContinuityTracker tracker;
    Defects defects;
    double yaw = 0;
    auto previous = scene.Frame(1, yaw);
    for (size_t step = 1; step <= steps; ++step) {
        const double before = yaw;
        yaw += kStepDegrees;
        auto current = scene.Frame(step + 1, yaw);
        if (!transition.Prepare(previous, current)) {
            Check(false, "transition prepared");
            break;
        }
        if (!tracker.Accept(transition.Stats())) {
            Check(false, "continuous transition at step " + std::to_string(step));
        }
        if (lastStats != nullptr) *lastStats = transition.UnmatchedStats();
        // As the window loop: the step frame at 0, then the in-between.
        for (const float alpha : {0.0F, 0.5F}) {
            Oot3dPicaVisualFrameViewSample sample;
            if (!transition.Sample(previous, current, alpha, sample)) {
                Check(false, "sample");
                continue;
            }
            // The scheduler refuses a draw twice in one presentation.
            std::set<uint64_t> ids;
            for (const auto& view : sample.Draws) {
                if (!ids.insert(view.BasePlan->SubmissionId).second) Check(false, "draw repeated in a sample");
            }
            Inspect(sample, before, yaw, alpha, defects);
        }
        previous = std::move(current);
    }
    return defects;
}

void Report(const char* name, const Defects& d) {
    std::printf("     %-34s samples=%zu draws=%zu missing=%zu displaced=%zu extra=%zu\n", name, d.Samples, d.Draws,
                d.Missing, d.Displaced, d.Extra);
}

// A slow turn and a still camera: the fix must not change what the old
// sample already got right (no draw ever unmatched -> identical samples).
void TestStillCameraIsUnchanged() {
    Scene scene;
    auto previous = scene.Frame(1, 10.0);
    auto current = scene.Frame(2, 10.0);
    Oot3dPicaPreparedVisualTransition legacy, fixed;
    fixed.SetOptions({.ExactStep = true, .CarryUnmatched = true});
    legacy.Prepare(previous, current);
    fixed.Prepare(previous, current);
    Oot3dPicaVisualFrameViewSample a, b;
    legacy.Sample(previous, current, 0.5F, a);
    fixed.Sample(previous, current, 0.5F, b);
    bool same = a.Draws.size() == b.Draws.size();
    for (size_t i = 0; same && i < a.Draws.size(); ++i) {
        same = a.Draws[i].BasePlan == b.Draws[i].BasePlan &&
               a.Draws[i].VertexUniforms.has_value() == b.Draws[i].VertexUniforms.has_value();
    }
    Check(same, "câmera parada: amostra intermediária igual à antiga");
    Check(fixed.UnmatchedStats().UnmatchedCurrent == 0 && fixed.UnmatchedStats().InsertedPrevious == 0,
          "câmera parada: nenhum draw sem par");
}

} // namespace

int main() {
    const Oot3dPicaVisualTransitionOptions legacy{};
    const Oot3dPicaVisualTransitionOptions exactOnly{.ExactStep = true, .CarryUnmatched = false};
    const Oot3dPicaVisualTransitionOptions fixed{.ExactStep = true, .CarryUnmatched = true};
    for (const bool precomposed : {false, true}) {
        const char* mode = precomposed ? "câmera nas linhas do modelo" : "câmera nas linhas de view";
        std::printf("== %s, giro de %.0f graus por passo\n", mode, kStepDegrees);
        const auto before = Spin(precomposed, legacy, 80);
        const auto exact = Spin(precomposed, exactOnly, 80);
        Oot3dPicaVisualUnmatchedStats stats;
        const auto after = Spin(precomposed, fixed, 80, &stats);
        Report("antigo (amostra sobre o quadro novo)", before);
        Report("so exact_step", exact);
        Report("exact_step + carry_unmatched", after);
        Check(before.Missing > 0 && before.Displaced > 0,
              std::string(mode) + ": o caminho antigo mostra o defeito (buracos e peças deslocadas)");
        Check(exact.Total() < before.Total(), std::string(mode) + ": exact_step sozinho já reduz");
        Check(after.Total() == 0, std::string(mode) + ": com a correção nenhuma peça falta ou sai do lugar");
        Check(stats.CameraMeasured && stats.WithoutCamera == 0, std::string(mode) + ": câmera medida nos draws pareados");
    }
    TestStillCameraIsUnchanged();
    std::printf("%s\n", gFailures == 0 ? "todos ok" : "FALHOU");
    return gFailures == 0 ? 0 : 1;
}
