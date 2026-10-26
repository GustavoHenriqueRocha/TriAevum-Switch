#include "fast/oot3d/pica_grass_source.h"

#include "fast/oot3d/directional_shadows.h"
#include "fast/oot3d/grass_primitive_topology.h"
#include "fast/oot3d/grass_scene_bridge.h"
#include "fast/oot3d/grass_shading_environment.h"
#include "fast/oot3d/grass_stage_profile.h"
#include "fast/oot3d/grass_surface_eligibility.h"
#include "fast/oot3d/grass_surface_extractor.h"
#include "fast/oot3d/grass_texture_source_cache.h"
#include "fast/oot3d/grass_texture_twins.h"
#include "fast/oot3d/pica_grass_texture_coordinates.h"
#include "fast/renderer3ds/pica_native_state.h"
#include "oot3d/renderer/pica_texture_decode.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace Fast::Oot3d {
namespace {

// The same identities and helpers as gfx_vulkan_pica_draw.cpp and
// gfx_vulkan_pica_textures.cpp, so a rule keyed on a texture hash on the PC
// matches the same texture here.
uint64_t HashBytes(std::span<const uint8_t> bytes) {
    uint64_t hash = 1469598103934665603ULL;
    for (const uint8_t value : bytes) {
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::optional<size_t> EncodedMipSize(uint8_t format, uint32_t width, uint32_t height) {
    static constexpr std::array<uint8_t, 14> kNibblesPerPixel{8U, 6U, 4U, 4U, 4U, 4U, 4U,
                                                              2U, 2U, 2U, 1U, 1U, 1U, 2U};
    if (format >= kNibblesPerPixel.size() || width == 0U || height == 0U) {
        return std::nullopt;
    }
    const uint64_t tiledWidth = (std::max(8U, width) + 7U) & ~7ULL;
    const uint64_t tiledHeight = (std::max(8U, height) + 7U) & ~7ULL;
    return static_cast<size_t>((tiledWidth * tiledHeight * kNibblesPerPixel[format] + 1U) / 2U);
}

std::optional<uint64_t> BaseLevelContentHash(const Renderer3ds::PicaTextureView& texture) {
    if (!texture.CopyReferences.empty()) {
        return std::nullopt;
    }
    if (texture.NativeBaseLevelContentHashAvailable) {
        return texture.NativeBaseLevelContentHash;
    }
    const auto byteCount = EncodedMipSize(texture.NativeFormat, texture.Width, texture.Height);
    if (!byteCount.has_value() || *byteCount > texture.NativeBytes.size()) {
        return std::nullopt;
    }
    return HashBytes(texture.NativeBytes.first(*byteCount));
}

std::optional<GrassTextureWrap> DecodeWrap(uint8_t wrap) {
    switch (wrap) {
    case 0: return GrassTextureWrap::Clamp;
    case 2:
    case 6:
    case 7: return GrassTextureWrap::Repeat;
    case 3: return GrassTextureWrap::Mirror;
    default: return std::nullopt;
    }
}

const Renderer3ds::PicaVertexBindingView* FindBinding(const Renderer3ds::PicaDrawView& draw, uint8_t binding) {
    const auto found = std::find_if(draw.VertexBindings.begin(), draw.VertexBindings.end(),
                                    [binding](const auto& value) { return value.Binding == binding; });
    return found == draw.VertexBindings.end() ? nullptr : &*found;
}

const Renderer3ds::PicaVertexAttributeView* FindAttribute(const Renderer3ds::PicaDrawView& draw, uint8_t location) {
    const auto found = std::find_if(draw.VertexAttributes.begin(), draw.VertexAttributes.end(),
                                    [location](const auto& value) { return value.Location == location; });
    return found == draw.VertexAttributes.end() ? nullptr : &*found;
}

uint64_t OccurrenceKey(uint64_t identity, uint64_t renderTargetNamespace, uint32_t colorPhysicalAddress) {
    uint64_t key = identity;
    key ^= renderTargetNamespace + 0x9e3779b97f4a7c15ULL + (key << 6U) + (key >> 2U);
    key ^= uint64_t{colorPhysicalAddress} + 0x9e3779b97f4a7c15ULL + (key << 6U) + (key >> 2U);
    return key;
}

uint64_t MixInstance(uint64_t identity, uint32_t occurrence, const Renderer3ds::PicaDrawView& draw,
                     uint64_t* worldInstance) {
    uint64_t id = identity ^ (uint64_t(occurrence) + 0x9e3779b97f4a7c15ULL + (identity << 6U) + (identity >> 2U));
    *worldInstance = id == 0U ? 1U : id;
    id ^= draw.RenderTargetNamespace + 0x9e3779b97f4a7c15ULL + (id << 6U) + (id >> 2U);
    id ^= uint64_t(draw.FramebufferColorPhysicalAddress) + 0x9e3779b97f4a7c15ULL + (id << 6U) + (id >> 2U);
    return id == 0U ? 1U : id;
}

using Matrix = std::array<float, 16>;

Matrix Identity() {
    return {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
}

Matrix Multiply(const Matrix& left, const Matrix& right) {
    Matrix result{};
    for (size_t row = 0; row < 4U; ++row) {
        for (size_t column = 0; column < 4U; ++column) {
            for (size_t inner = 0; inner < 4U; ++inner) {
                result[row * 4U + column] += left[row * 4U + inner] * right[inner * 4U + column];
            }
        }
    }
    return result;
}

bool Resolve(const Renderer3ds::PicaDrawView& draw, uint64_t frameId, const InteractiveGrassSettings& settings,
             PicaGrassSourceState& state, uint64_t sequenceId, uint64_t drawOrder, GrassSceneMesh& resolved,
             bool& cacheable) {
    cacheable = true;
    if (settings.Rules.empty()) {
        return false;
    }
    const auto texture = std::find_if(draw.Textures.begin(), draw.Textures.end(),
                                      [](const auto& value) { return value.Slot == 0U; });
    const bool perspective = UsesPicaPerspectiveProjection(draw.VertexUniformBytes);
    const GrassSurfaceDrawInfo eligibility{
        draw.FragmentOperationMode == 0U,
        true, // opaque world draw (checked by the caller)
        texture != draw.Textures.end(),
        draw.FragmentShaderHooks.SamplesTexture(0),
        draw.PositionAttributeLocation != 0xff,
        draw.TexCoord0AttributeLocation != 0xff,
        draw.DepthTestEnabled,
        draw.DepthWriteEnabled,
        draw.DepthCompare == decltype(draw.DepthCompare)::Always,
        perspective,
        (draw.ColorWriteMask & 0x7U) != 0U,
        !Renderer3ds::PicaBlendReplacesDestination(draw.Blend),
        IsGrassPrimitiveTopologySupported(draw.Topology),
    };
    if (!IsGrassSurfaceDrawEligible(eligibility) || texture == draw.Textures.end()) {
        return false;
    }
    const auto textureHash = BaseLevelContentHash(*texture);
    if (!textureHash.has_value()) {
        return false;
    }
    auto& sources = GrassTextureSourceCache::Instance();
    if (texture->NativeType == 0 && texture->CopyReferences.empty() && texture->Width >= 16 &&
        texture->Height >= 16 && !sources.Find(*textureHash, false)) {
        const auto size = EncodedMipSize(texture->NativeFormat, texture->Width, texture->Height);
        std::vector<uint8_t> rgba;
        if (size && *size <= texture->NativeBytes.size() &&
            ::Oot3d::Renderer::DecodePicaTextureRgba8(texture->NativeFormat, texture->Width, texture->Height,
                                                      texture->NativeBytes.first(*size), rgba, nullptr)) {
            sources.ObserveDecoded(*textureHash, texture->Width, texture->Height, rgba);
            (void)sources.Find(*textureHash, true);
        }
    }
    const uint64_t observedHash = sources.ResolveObservedHash(*textureHash, texture->Width, texture->Height);
    if (!GrassTwinRules::Instance().Configured(settings.Rules, observedHash, texture->Width, texture->Height, 0U)) {
        return false;
    }
    if (texture->NativeType != 0 || !texture->CopyReferences.empty()) {
        return false;
    }
    const auto wrapS = DecodeWrap(texture->NativeWrapS);
    const auto wrapT = DecodeWrap(texture->NativeWrapT);
    if (!wrapS || !wrapT) {
        return false;
    }
    const auto* position = FindAttribute(draw, draw.PositionAttributeLocation);
    const auto* uv = FindAttribute(draw, draw.TexCoord0AttributeLocation);
    if (!position || !uv || position->ComponentCount < 3 || uv->ComponentCount < 2) {
        return false;
    }
    const auto* positionBinding = FindBinding(draw, position->Binding);
    const auto* uvBinding = FindBinding(draw, uv->Binding);
    if (!positionBinding || !uvBinding || positionBinding->PerInstance || uvBinding->PerInstance) {
        return false;
    }
    const auto coordinates = DecodePicaGrassTextureCoordinateTransform(
        draw.VertexShaderSource, draw.FragmentShaderSource, draw.VertexUniformBytes, draw.TexCoord0AttributeLocation,
        &draw.TemporalVertexProgram.Hooks, &draw.FragmentShaderHooks);
    if (!coordinates.Applied()) {
        return false;
    }

    cacheable = false; // transient camera/transform failures are retried
    // OoT3D's rigid CMB vertex program: model f[20..22], view f[4..6],
    // projection f[0..3]; skinned draws (boolean 2) are left out.
    constexpr size_t kFloatUniformOffset = 16U + 4U * 4U * sizeof(uint32_t);
    constexpr size_t kVec4Bytes = 4U * sizeof(float);
    const auto uniformOffset = [](size_t first) { return kFloatUniformOffset + first * kVec4Bytes; };
    const size_t modelOffset = uniformOffset(20U);
    if (draw.VertexUniformBytes.size() < modelOffset + 3U * kVec4Bytes) {
        return false;
    }
    uint32_t booleanMask = 0;
    std::memcpy(&booleanMask, draw.VertexUniformBytes.data(), sizeof(booleanMask));
    if ((booleanMask & 4U) != 0U) {
        return false;
    }
    Matrix projection{};
    Matrix view = Identity();
    Matrix model = Identity();
    std::memcpy(projection.data(), draw.VertexUniformBytes.data() + uniformOffset(0U), 4U * kVec4Bytes);
    std::memcpy(view.data(), draw.VertexUniformBytes.data() + uniformOffset(4U), 3U * kVec4Bytes);
    std::memcpy(model.data(), draw.VertexUniformBytes.data() + modelOffset, 3U * kVec4Bytes);
    const auto finite = [](const Matrix& matrix) {
        return std::all_of(matrix.begin(), matrix.end(), [](float value) { return std::isfinite(value); });
    };
    if (!finite(projection) || !finite(view) || !finite(model)) {
        return false;
    }
    const Matrix modelToView = Multiply(view, model);
    DirectionalShadowMatrix capturedViewToWorld;
    if (!draw.LightBinding.WorldToView ||
        !InvertDirectionalShadowMatrix(*draw.LightBinding.WorldToView, capturedViewToWorld)) {
        return false;
    }
    Matrix viewToWorld;
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            viewToWorld[row * 4 + column] = capturedViewToWorld[column * 4 + row];
        }
    }
    const Matrix modelToWorld = Multiply(viewToWorld, modelToView);
    Matrix modelToClip = Multiply(projection, modelToView);
    if (draw.FramebufferFlipped) {
        for (size_t column = 0; column < 4U; ++column) {
            modelToClip[4U + column] *= -1.0F;
        }
    }
    const float determinant =
        modelToWorld[0] * (modelToWorld[5] * modelToWorld[10] - modelToWorld[6] * modelToWorld[9]) -
        modelToWorld[1] * (modelToWorld[4] * modelToWorld[10] - modelToWorld[6] * modelToWorld[8]) +
        modelToWorld[2] * (modelToWorld[4] * modelToWorld[9] - modelToWorld[5] * modelToWorld[8]);
    if (std::abs(determinant) < 1.0e-12F) {
        return false;
    }
    cacheable = true;

    const GrassGeometryRequest request{
        draw.GeometryIdentity,
        draw.GeometryContentVersion,
        draw.GeometryIdentityAvailable,
        frameId,
        {positionBinding->Bytes, positionBinding->ByteStride, position->Format, position->ComponentCount,
         position->ByteOffset},
        {uvBinding->Bytes, uvBinding->ByteStride, uv->Format, uv->ComponentCount, uv->ByteOffset},
        draw.IndexBytes,
        draw.Indexed,
        draw.IndicesAre16Bit,
        draw.BaseVertex,
        draw.VertexCount,
        draw.Topology,
        coordinates,
    };
    const auto geometry = state.Geometry.Resolve(request);
    if (geometry.Geometry == nullptr) {
        return false;
    }

    GrassSceneMesh mesh;
    mesh.SubmissionId = draw.SubmissionId;
    mesh.DrawOrder = drawOrder;
    mesh.SequenceId = sequenceId;
    mesh.NativeTextureIdentity = texture->NativeContentHash;
    const auto inherited =
        GrassTwinRules::Instance().Inherited(settings.Rules, observedHash, texture->Width, texture->Height);
    const auto rule = std::find_if(settings.Rules.begin(), settings.Rules.end(), [&](const auto& r) {
        return inherited ? r.RuleId == inherited->RuleId : r.Target.Rgba8Hash == observedHash;
    });
    if (rule != settings.Rules.end()) {
        mesh.ConfiguredRuleId = rule->RuleId;
        mesh.ConfiguredRuleHash = rule->Target.Rgba8Hash;
    }
    mesh.CoordinateVersion = PicaGrassTextureCoordinateTransformVersion(coordinates);
    mesh.Alphas = geometry.Geometry->Alphas;
    mesh.GeometryId = geometry.Geometry->GeometryId;
    mesh.AnchorVersion = geometry.Geometry->AnchorVersion;
    mesh.ContentVersion = geometry.Geometry->ContentVersion;
    mesh.TextureHash = *textureHash;
    const uint64_t identity = mesh.GeometryId ^ (mesh.TextureHash + 0x9e3779b97f4a7c15ULL + (mesh.GeometryId << 6U) +
                                                 (mesh.GeometryId >> 2U));
    const uint32_t occurrence = state.Occurrences[OccurrenceKey(identity, draw.RenderTargetNamespace,
                                                                draw.FramebufferColorPhysicalAddress)]++;
    mesh.InstanceId = MixInstance(identity, occurrence, draw, &mesh.WorldInstanceId);
    mesh.RenderTargetNamespace = draw.RenderTargetNamespace;
    mesh.FramebufferColorPhysicalAddress = draw.FramebufferColorPhysicalAddress;
    mesh.FrameId = frameId;
    mesh.TextureWidth = texture->Width;
    mesh.TextureHeight = texture->Height;
    mesh.MapperSlot = 0;
    mesh.MaterialWrapS = *wrapS;
    mesh.MaterialWrapT = *wrapT;
    mesh.TransformBakedIntoVertices = false;
    mesh.PreserveWorldAnchor = true;
    mesh.ModelToWorld = modelToWorld;
    // Row-major on the CPU; stored column-major like the Vulkan path.
    for (size_t row = 0; row < 4U; ++row) {
        for (size_t column = 0; column < 4U; ++column) {
            mesh.PicaModelToClip[column * 4U + row] = modelToClip[row * 4U + column];
        }
    }
    mesh.PicaModelToClipAvailable = true;
    mesh.Shading = DecodeGrassShadingEnvironment(draw.VertexUniformBytes, draw.FragmentUniformBytes,
                                                 draw.FragmentFeatures, capturedViewToWorld);
    mesh.Vertices = geometry.Geometry->Vertices;
    mesh.Indices = geometry.Geometry->Indices;
    mesh.ObservedTextureHash = observedHash;
    mesh.ObservedTextureResolved = true;
    resolved = std::move(mesh);
    return true;
}

// InteractiveGrassPass's GrassWorldPlacementIdentity.
uint64_t WorldPlacementIdentity(const GrassSceneMesh& mesh, const GrassPlacementKey& key) {
    uint64_t hash = 14695981039346656037ULL;
    const auto mix = [&hash](const auto& value) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
        for (size_t index = 0U; index < sizeof(value); ++index) {
            hash = (hash ^ bytes[index]) * 1099511628211ULL;
        }
    };
    mix(mesh.WorldInstanceId != 0U ? mesh.WorldInstanceId : mesh.InstanceId);
    mix(key.GeometryId);
    mix(key.ContentVersion);
    mix(key.TextureHash);
    mix(key.RuleId);
    mix(mesh.MapperSlot);
    return hash == 0U ? 1U : hash;
}

} // namespace

PicaGrassPlacements::PicaGrassPlacements() : mBuilder(64U, 1U) {}

const std::vector<std::shared_ptr<const GrassWorldPlacement>>&
PicaGrassPlacements::Update(uint64_t frameId, const InteractiveGrassSettings& settings) {
    mReady.clear();
    mStats = {};
    const auto inherited = GrassTwinRules::Instance().AllInherited(settings.Rules);
    std::vector<GrassPlacementRule> rules = settings.Rules;
    rules.insert(rules.end(), inherited.begin(), inherited.end());
    for (const auto& rule : rules) {
        if (!GrassTextureSourceCache::Instance().Contains(rule.Target.Rgba8Hash)) {
            continue;
        }
        ++mStats.Rules;
        const auto match = GrassSceneBridge::Instance().Match(rule.Target);
        for (const auto& mesh : match.ScopedMeshes) {
            if (mesh.Vertices == nullptr || mesh.Indices == nullptr || mesh.FrameId != frameId) {
                continue;
            }
            ++mStats.Meshes;
            uint64_t version = mesh.ContentVersion ^ GrassPlacementRuleVersion(rule, settings.Generation);
            version ^= static_cast<uint64_t>(std::bit_cast<uint32_t>(settings.CullingClusterSize)) +
                       0x9e3779b97f4a7c15ULL + (version << 6U) + (version >> 2U);
            GrassAsyncPlacementRequest request;
            request.PlacementKey = {mesh.GeometryId, version, mesh.TextureHash, rule.RuleId};
            request.SourceContentVersion = mesh.ContentVersion;
            request.WorldIdentity = WorldPlacementIdentity(mesh, request.PlacementKey);
            request.FrameId = frameId;
            request.InstanceId = mesh.InstanceId;
            request.TextureHash = mesh.TextureHash;
            request.MapperSlot = mesh.MapperSlot;
            request.MaterialWrapS = mesh.MaterialWrapS;
            request.MaterialWrapT = mesh.MaterialWrapT;
            request.ModelToWorld = mesh.ModelToWorld;
            request.TransformBakedIntoVertices = mesh.TransformBakedIntoVertices;
            request.Vertices = mesh.Vertices;
            request.Indices = mesh.Indices;
            request.LazyMaskTexture = rule.Target.Rgba8Hash;
            request.LazyColorTexture = mesh.ObservedTextureHash;
            request.Rule = rule;
            request.Generation = settings.Generation;
            request.Budget = settings.MaxInstancesPerRoom;
            request.ClusterSize = settings.CullingClusterSize;
            request.NormalOffset = rule.NormalOffset;
            request.HeightScale = settings.Appearance.HeightScale;
            auto result = mBuilder.ResolveOrQueue(std::move(request));
            if (result.Queued || result.State != GrassAsyncPlacementState::Ready) {
                ++mStats.Queued;
            }
            if (result.Placement && !result.Placement->Anchors.empty()) {
                ++mStats.Ready;
                mStats.Anchors += result.Placement->Anchors.size();
                mStats.BuildMilliseconds += result.BuildMilliseconds;
                mReady.push_back(std::move(result.Placement));
            }
        }
    }
    return mReady;
}

bool PublishPicaGrassSource(const Renderer3ds::PicaDrawView& draw, uint64_t frameId,
                            const InteractiveGrassSettings& settings, PicaGrassSourceState& state,
                            uint64_t sequenceId, uint64_t drawOrder) {
    if (state.Frame != frameId) {
        state.Frame = frameId;
        state.Occurrences.clear();
    }
    GrassStageTimer total(GrassStage::Source);
    const bool perspective = UsesPicaPerspectiveProjection(draw.VertexUniformBytes);
    const auto key = BuildGrassDrawMemoKey(draw, state.Memo.Rules(settings, frameId),
                                           GrassTextureSourceCache::Instance().Generation(), true,
                                           draw.FragmentShaderHooks.SamplesTexture(0), perspective, false);
    if (auto* entry = key ? state.Memo.Find(*key, frameId) : nullptr) {
        if (!entry->Mesh) {
            return false;
        }
        auto mesh = *entry->Mesh;
        if (RefreshGrassMemoDraw(mesh, draw, state.Memo.CameraInverse(draw))) {
            entry->Mesh->PicaModelToClip = mesh.PicaModelToClip;
            const uint64_t identity = mesh.GeometryId ^ (mesh.TextureHash + 0x9e3779b97f4a7c15ULL +
                                                         (mesh.GeometryId << 6U) + (mesh.GeometryId >> 2U));
            const uint32_t occurrence = state.Occurrences[OccurrenceKey(identity, draw.RenderTargetNamespace,
                                                                        draw.FramebufferColorPhysicalAddress)]++;
            mesh.InstanceId = MixInstance(identity, occurrence, draw, &mesh.WorldInstanceId);
            mesh.FrameId = frameId;
            mesh.SubmissionId = draw.SubmissionId;
            mesh.SequenceId = sequenceId;
            mesh.DrawOrder = drawOrder;
            GrassSceneBridge::Instance().Publish(std::move(mesh));
            return true;
        }
    }
    GrassSceneMesh mesh;
    bool cacheable = false;
    const bool resolved = Resolve(draw, frameId, settings, state, sequenceId, drawOrder, mesh, cacheable);
    if (key && cacheable) {
        state.Memo.Store(*key, resolved ? std::optional(mesh) : std::nullopt, frameId);
    }
    if (resolved) {
        GrassSceneBridge::Instance().Publish(std::move(mesh));
    }
    return resolved;
}

} // namespace Fast::Oot3d
