#include "three_ds_motion_composition.h"

#include <algorithm>
#include <cmath>

namespace ThreeDsRecomp::Input {
namespace {
using Vector = std::array<double, 3>;
using Rotation = std::array<double, 4>;
constexpr double kRadians = 3.14159265358979323846 / 180.0;
double Dot(const Vector& a, const Vector& b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
Vector Cross(const Vector& a, const Vector& b) {
    return {a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]};
}
Vector Unit(Vector v) {
    const double length = std::sqrt(Dot(v, v));
    if (!std::isfinite(length) || length < 1e-8) return {0, -1, 0};
    for (auto& x : v) x /= length;
    return v;
}
Vector Wide(const std::array<float, 3>& v) { return {v[0], v[1], v[2]}; }
std::array<float, 3> Narrow(const Vector& v) { return {float(v[0]), float(v[1]), float(v[2])}; }
Rotation Normalize(Rotation q) {
    double norm = 0;
    for (auto v : q) norm += v*v;
    if (!std::isfinite(norm) || norm < 1e-16) return {1, 0, 0, 0};
    for (auto& v : q) v /= std::sqrt(norm);
    return q;
}
Rotation Multiply(const Rotation& a, const Rotation& b) {
    return {a[0]*b[0]-a[1]*b[1]-a[2]*b[2]-a[3]*b[3],
            a[0]*b[1]+a[1]*b[0]+a[2]*b[3]-a[3]*b[2],
            a[0]*b[2]-a[1]*b[3]+a[2]*b[0]+a[3]*b[1],
            a[0]*b[3]+a[1]*b[2]-a[2]*b[1]+a[3]*b[0]};
}
Vector Rotate(const Rotation& q, const Vector& v) {
    const Vector axis{q[1], q[2], q[3]};
    auto t = Cross(axis, v);
    for (auto& x : t) x *= 2;
    const auto c = Cross(axis, t);
    return {v[0]+q[0]*t[0]+c[0], v[1]+q[0]*t[1]+c[1], v[2]+q[0]*t[2]+c[2]};
}
Rotation Align(const Vector& from, const Vector& to) {
    const auto a = Unit(from), b = Unit(to);
    const double dot = std::clamp(Dot(a, b), -1.0, 1.0);
    if (dot < -0.999999) {
        const auto axis = Unit(Cross(a, std::abs(a[0]) < 0.8 ? Vector{1,0,0} : Vector{0,1,0}));
        return {0, axis[0], axis[1], axis[2]};
    }
    const auto c = Cross(a, b);
    return Normalize({1+dot, c[0], c[1], c[2]});
}
Rotation Delta(const Vector& rate, double seconds) {
    const double speed = std::sqrt(Dot(rate, rate));
    if (speed < 1e-10) return {1,0,0,0};
    const double halfAngle = -0.5 * speed * kRadians * seconds;
    const double scale = std::sin(halfAngle) / speed;
    return {std::cos(halfAngle), rate[0]*scale, rate[1]*scale, rate[2]*scale};
}
}

void MotionCompositionState::RestoreGravity(const std::array<float, 3>& gravity) noexcept {
    *this = {};
    LastGravity = gravity;
    HasOutput = true;
}

ComposedMotion MotionCompositionState::Sample(const std::array<float, 3>& gravity,
    bool gravityValid, const std::array<float, 3>& bodyRate, float pitchRate,
    float yawRate, double seconds, bool physicalBase, bool advance) noexcept {
    auto next = *this;
    if (gravityValid) next.BaseGravity = gravity;
    const Vector base = physicalBase ? Wide(next.BaseGravity) : Vector{0, -1, 0};
    if (!next.Initialized || next.PhysicalBase != physicalBase) {
        next.SensorToVirtual = next.HasOutput ? Align(base, Wide(next.LastGravity)) : Rotation{1,0,0,0};
        next.Initialized = true;
        next.PhysicalBase = physicalBase;
    }
    const double dt = std::clamp(seconds, 0.001, 0.25);
    // Modelo: virtual = Yaw_mundo(yaw sintético) * físico * Offset_corpo.
    // O SensorToVirtual guarda só o offset preso ao corpo (pitch sintético em
    // torno do próprio X do aparelho virtual, mais o alinhamento da troca de
    // dono). A gravidade e as taxas físicas do corpo passam por ele, então a
    // fusão do guest vê um aparelho rígido. O yaw sintético gira o aparelho
    // inteiro em torno do "para cima" do mundo: isso não muda nem a gravidade
    // nem as taxas no referencial do corpo, então ele só sai como taxa e nunca
    // é guardado. Guardado do lado do sensor, ele girava as taxas físicas
    // seguintes em torno do "para cima": passando de 90 graus o pitch físico
    // virava roll, e passando de 180 trocava de sinal.
    const auto step = [&](double t) {
        return Normalize(Multiply(Delta({pitchRate,0,0}, t), next.SensorToVirtual));
    };
    const auto midpoint = step(dt*0.5), endpoint = step(dt);
    const auto down = Unit(Rotate(midpoint, base));
    // Yaw em torno do "para cima" do mundo, no corpo virtual (projetado em Y/Z quando inclinado).
    const Vector synthetic{pitchRate - down[0]*yawRate, -down[1]*yawRate, -down[2]*yawRate};
    auto rate = Rotate(midpoint, Wide(bodyRate));
    for (int i = 0; i < 3; ++i) rate[i] += synthetic[i];
    next.SensorToVirtual = endpoint;
    next.LastGravity = Narrow(Rotate(endpoint, base));
    next.HasOutput = true;
    if (advance) *this = next;
    return {next.LastGravity, Narrow(rate)};
}
} // namespace ThreeDsRecomp::Input
