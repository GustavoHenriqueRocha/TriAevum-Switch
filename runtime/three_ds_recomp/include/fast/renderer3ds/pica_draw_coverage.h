#pragma once

#include "fast/renderer3ds/pica_render_backend.h"
#include "fast/renderer3ds/pica_vertex_geometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace Fast::Renderer3ds {

// Whether a draw's primitives cover its whole viewport, proven by projecting
// its vertex positions through the model-view-projection transform the
// frontend identified in its vertex program (typed layout, float uniforms).
// A full-frame wash or a redraw of a copied frame is one quad over the screen;
// a placed element covers part of it. Anything not provable is not coverage:
// no typed transform, an enabled skeleton, non-float positions, more vertices
// than a full-frame quad needs, or a vertex at or behind the eye plane.
// Normalized-device bounds (x = clip row 0, y = clip row 1) of a draw's
// projected positions, under the same proof rules as the coverage below;
// nullopt when not provable.
struct PicaDrawNdcBounds {
    std::array<float, 2> Minimum{};
    std::array<float, 2> Maximum{};

    // Whether the bounds reach `extent` (in NDC, 1 = the viewport edge) on
    // both sides of each axis.
    [[nodiscard]] bool Reaches(std::array<float, 2> extent) const noexcept {
        constexpr float kSlack = 1.0e-3F;
        for (size_t axis = 0; axis < 2U; ++axis) {
            const float edge = extent[axis] * (1.0F - kSlack);
            if (!(Minimum[axis] <= -edge) || !(Maximum[axis] >= edge)) {
                return false;
            }
        }
        return true;
    }
};

[[nodiscard]] inline std::optional<PicaDrawNdcBounds> PicaDrawGeometryNdcBounds(
    const PicaDrawView& draw, uint32_t booleanUniforms,
    std::span<const uint8_t> floatUniforms,
    uint32_t maximumVertices = 16U) noexcept {
    using namespace VertexGeometry;
    const auto& hooks = draw.TemporalVertexProgram.Hooks;
    const auto* transform = hooks.TransformProgram();
    if (transform == nullptr) {
        return std::nullopt;
    }
    if (const auto* skeleton = hooks.SkeletonProgram();
        skeleton != nullptr && skeleton->EnableBooleanUniform < 32U &&
        (booleanUniforms & (1U << skeleton->EnableBooleanUniform)) != 0U) {
        return std::nullopt;
    }
    ShadowMatrix view;
    ShadowMatrix model;
    std::array<Vec4, 4> projection{};
    if (!UniformMatrix(floatUniforms, transform->ViewFirstUniform, view) ||
        !UniformMatrix(floatUniforms, transform->ModelFirstUniform, model) ||
        floatUniforms.size() <
            (size_t{transform->ProjectionFirstUniform} + projection.size()) *
                sizeof(Vec4)) {
        return std::nullopt;
    }
    for (size_t row = 0; row < projection.size(); ++row) {
        std::memcpy(projection[row].data(),
                    floatUniforms.data() +
                        (transform->ProjectionFirstUniform + row) * sizeof(Vec4),
                    sizeof(Vec4));
        if (!std::all_of(projection[row].begin(), projection[row].end(),
                         [](float value) { return std::isfinite(value); })) {
            return std::nullopt;
        }
    }
    const auto attribute = std::find_if(
        draw.VertexAttributes.begin(), draw.VertexAttributes.end(),
        [&](const auto& item) {
            return item.Location == transform->PositionInputRegister;
        });
    if (attribute == draw.VertexAttributes.end() ||
        attribute->Format != PicaVertexFormat::Float ||
        attribute->ComponentCount < 2U || attribute->ComponentCount > 4U) {
        return std::nullopt;
    }
    const auto binding = std::find_if(
        draw.VertexBindings.begin(), draw.VertexBindings.end(),
        [&](const auto& item) { return item.Binding == attribute->Binding; });
    if (binding == draw.VertexBindings.end() || binding->PerInstance ||
        binding->ByteStride == 0U) {
        return std::nullopt;
    }
    const size_t indexBytes = draw.IndicesAre16Bit ? 2U : 1U;
    const size_t count =
        draw.Indexed ? std::min<size_t>(draw.VertexCount,
                                        draw.IndexBytes.size() / indexBytes)
                     : draw.VertexCount;
    if (count == 0U || count > maximumVertices) {
        return std::nullopt;
    }
    std::array<float, 2> minimum{1.0e30F, 1.0e30F};
    std::array<float, 2> maximum{-1.0e30F, -1.0e30F};
    for (size_t item = 0; item < count; ++item) {
        uint32_t index = static_cast<uint32_t>(item);
        if (draw.Indexed) {
            if (draw.IndicesAre16Bit) {
                uint16_t value = 0;
                std::memcpy(&value, draw.IndexBytes.data() + item * 2U,
                            sizeof(value));
                index = value;
            } else {
                index = draw.IndexBytes[item];
            }
        }
        const int64_t vertex = int64_t{index} + draw.BaseVertex;
        const uint64_t offset =
            static_cast<uint64_t>(vertex) * binding->ByteStride +
            attribute->ByteOffset;
        const size_t bytes = size_t{attribute->ComponentCount} * sizeof(float);
        if (vertex < 0 || offset > binding->Bytes.size() ||
            bytes > binding->Bytes.size() - offset) {
            return std::nullopt;
        }
        Vec4 position{0.0F, 0.0F, 0.0F, 1.0F};
        std::memcpy(position.data(), binding->Bytes.data() + offset, bytes);
        if (!std::all_of(position.begin(), position.end(),
                         [](float value) { return std::isfinite(value); })) {
            return std::nullopt;
        }
        const auto modelPosition = Transform(model, position);
        const auto eye = Transform(
            view, {modelPosition[0], modelPosition[1], modelPosition[2], 1.0F});
        const Vec4 eye4{eye[0], eye[1], eye[2], 1.0F};
        const auto row = [&](size_t which) {
            double sum = 0.0;
            for (size_t column = 0; column < 4U; ++column) {
                sum += double{projection[which][column]} * eye4[column];
            }
            return sum;
        };
        const double w = row(3U);
        if (!(w > 1.0e-6)) {
            return std::nullopt;
        }
        const std::array<double, 2> ndc{row(0U) / w, row(1U) / w};
        for (size_t axis = 0; axis < 2U; ++axis) {
            if (!std::isfinite(ndc[axis])) {
                return std::nullopt;
            }
            minimum[axis] = std::min(minimum[axis], static_cast<float>(ndc[axis]));
            maximum[axis] = std::max(maximum[axis], static_cast<float>(ndc[axis]));
        }
    }
    return PicaDrawNdcBounds{minimum, maximum};
}

[[nodiscard]] inline bool PicaDrawGeometryCoversViewport(
    const PicaDrawView& draw, uint32_t booleanUniforms,
    std::span<const uint8_t> floatUniforms,
    uint32_t maximumVertices = 16U) noexcept {
    const auto bounds = PicaDrawGeometryNdcBounds(
        draw, booleanUniforms, floatUniforms, maximumVertices);
    return bounds.has_value() && bounds->Reaches({1.0F, 1.0F});
}

} // namespace Fast::Renderer3ds
