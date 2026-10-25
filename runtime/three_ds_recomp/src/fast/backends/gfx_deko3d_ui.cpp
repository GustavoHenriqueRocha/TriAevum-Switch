// deko3d UI path, package 6 (docs/DEKO3D_PROGRESSO.md): what the window gets
// on top of the PICA scanout.
// - TopScreen UI (oot3d_legacy_ui_adapters.cpp): RGBA8 textures with their
//   sampler, and triangle lists in the layout of its two Fast3D programs
//   (solid = shade, textured = TEXEL0 * shade), drawn with embedded DKSH
//   (tools/oot3d/deko3d_ui).
// - ImGui (Fast3dGui, the config/fps_counter text): the same textured
//   program over the font atlas.
// GL rects and clip space are converted in gfx_deko3d_ui_plan.h.
#include "fast/backends/gfx_deko3d.h"

#if defined(__SWITCH__)

#include "gfx_deko3d_api.h"
#include "gfx_deko3d_ui_dksh.h"

#include <imgui.h>
#include <switch.h>

#include <cstring>

namespace Fast {

using namespace Deko3d;

namespace {

constexpr uint64_t kUiSolidVertexKey = 0xDE0C3D5C0B000001ULL;
constexpr uint64_t kUiSolidFragmentKey = 0xDE0C3D5C0B000002ULL;
constexpr uint64_t kUiTexturedVertexKey = 0xDE0C3D5C0B000003ULL;
constexpr uint64_t kUiTexturedFragmentKey = 0xDE0C3D5C0B000004ULL;
constexpr uint32_t kUiParamsBytes = 20U * sizeof(float); // mat4 + vec4

// The backend ImGui draws on (one window, one backend).
GfxRenderingAPIDeko3d* sImGuiBackend = nullptr;

} // namespace

void Deko3dRenderImGuiDrawData(const ImDrawData* data) {
    if (sImGuiBackend != nullptr) {
        sImGuiBackend->RenderImGui(data);
    }
}

void GfxRenderingAPIDeko3d::InitUi() {
    mUiSolidVertex = mShaders.Embedded(kUiSolidVertexKey, std::span<const uint8_t>(kUiSolidVertexDksh));
    mUiSolidFragment = mShaders.Embedded(kUiSolidFragmentKey, std::span<const uint8_t>(kUiSolidFragmentDksh));
    mUiTexturedVertex = mShaders.Embedded(kUiTexturedVertexKey, std::span<const uint8_t>(kUiTexturedVertexDksh));
    mUiTexturedFragment =
        mShaders.Embedded(kUiTexturedFragmentKey, std::span<const uint8_t>(kUiTexturedFragmentDksh));
    const bool ready = mUiSolidVertex != nullptr && mUiSolidFragment != nullptr && mUiTexturedVertex != nullptr &&
                       mUiTexturedFragment != nullptr;
    sImGuiBackend = this;
    Log("ui: shaders %s", ready ? "ready" : "MISSING");
}

void GfxRenderingAPIDeko3d::ShutdownUi() {
    if (sImGuiBackend == this) {
        sImGuiBackend = nullptr;
    }
    // After WaitIdle: nothing in flight.
    for (auto& [id, texture] : mUiTextures) {
        if (texture.Memory != nullptr) {
            dkMemBlockDestroy(texture.Memory);
            mResources.FreeImageSlot(texture.ImageSlot);
        }
    }
    mUiTextures.clear();
    for (const UiRetired& retired : mUiRetired) {
        dkMemBlockDestroy(retired.Memory);
        mResources.FreeImageSlot(retired.ImageSlot);
    }
    mUiRetired.clear();
    mImGuiFontTexture = 0;
}

void GfxRenderingAPIDeko3d::RetireUi(DkMemBlock memory, uint32_t imageSlot) {
    if (memory != nullptr) {
        mUiRetired.push_back({memory, imageSlot, mDevice.Frame()});
    }
}

void GfxRenderingAPIDeko3d::CollectUiRetired() {
    const uint64_t frame = mDevice.Frame();
    auto keep = mUiRetired.begin();
    for (auto it = mUiRetired.begin(); it != mUiRetired.end(); ++it) {
        if (it->Frame + kFramesInFlight <= frame) {
            dkMemBlockDestroy(it->Memory);
            mResources.FreeImageSlot(it->ImageSlot);
        } else {
            *keep++ = *it;
        }
    }
    mUiRetired.erase(keep, mUiRetired.end());
}

// ---- textures ---------------------------------------------------------------

uint32_t GfxRenderingAPIDeko3d::NewTexture() {
    const uint32_t id = mNextTextureId++;
    mUiTextures.emplace(id, UiTexture{});
    return id;
}

void GfxRenderingAPIDeko3d::SelectTexture(int tile, uint32_t textureId) {
    mUiActiveTile = tile & 1;
    mUiSelected[static_cast<size_t>(mUiActiveTile)] = textureId;
}

GfxRenderingAPIDeko3d::UiTexture* GfxRenderingAPIDeko3d::SelectedUiTexture() {
    const auto found = mUiTextures.find(mUiSelected[static_cast<size_t>(mUiActiveTile)]);
    return found != mUiTextures.end() ? &found->second : nullptr;
}

bool GfxRenderingAPIDeko3d::UploadUiTexture(UiTexture& texture, const uint8_t* rgba8, uint32_t width,
                                            uint32_t height) {
    if (!mDeviceReady || rgba8 == nullptr || width == 0U || height == 0U) {
        ++mCounters.UiTextureFailures;
        return false;
    }
    EnsureFrame();
    if (texture.Memory == nullptr || texture.Width != width || texture.Height != height) {
        // A new size: the old image may still be read by frames in flight.
        RetireUi(texture.Memory, texture.ImageSlot);
        texture.Memory = nullptr;
        texture.ImageSlot = PicaResources::kMaxImages;
        DkImageLayoutMaker maker;
        dkImageLayoutMakerDefaults(&maker, mDevice.Handle());
        maker.format = DkImageFormat_RGBA8_Unorm;
        maker.dimensions[0] = width;
        maker.dimensions[1] = height;
        DkImageLayout layout;
        dkImageLayoutInitialize(&layout, &maker);
        texture.Memory = mDevice.MakeBlock(static_cast<uint32_t>(dkImageLayoutGetSize(&layout)),
                                           DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, "ui texture");
        if (texture.Memory == nullptr) {
            ++mCounters.UiTextureFailures;
            return false;
        }
        dkImageInitialize(&texture.Image, &layout, texture.Memory, 0U);
        texture.ImageSlot = mResources.AllocateImageSlot();
        if (texture.ImageSlot >= PicaResources::kMaxImages) {
            dkMemBlockDestroy(texture.Memory);
            texture.Memory = nullptr;
            ++mCounters.UiTextureFailures;
            return false;
        }
        texture.Width = width;
        texture.Height = height;
        ++mCounters.UiTexturesCreated;
    }
    // Own staging block: UI textures (and the ImGui atlas) can be larger
    // than what a frame's stream should hold; freed after the frames in flight.
    const uint32_t bytes = width * height * 4U;
    DkMemBlock staging =
        mDevice.MakeBlock(bytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, "ui texture staging");
    if (staging == nullptr) {
        ++mCounters.UiTextureFailures;
        return false;
    }
    std::memcpy(dkMemBlockGetCpuAddr(staging), rgba8, bytes);
    DkImageView view;
    dkImageViewDefaults(&view, &texture.Image);
    const DkCopyBuf copy = {dkMemBlockGetGpuAddr(staging), 0U, 0U};
    const DkImageRect rect = {0U, 0U, 0U, width, height, 1U};
    dkCmdBufCopyBufferToImage(mDevice.Cmd(), &copy, &view, &rect, 0U);
    // Also asks for the barrier before the next draw samples it.
    mResources.WriteImageDescriptor(texture.ImageSlot, texture.Image);
    RetireUi(staging, PicaResources::kMaxImages);
    return true;
}

void GfxRenderingAPIDeko3d::UploadTexture(const uint8_t* rgba32Buf, uint32_t width, uint32_t height) {
    UiTexture* texture = SelectedUiTexture();
    if (texture == nullptr) {
        ++mCounters.UiTextureFailures;
        return;
    }
    UploadUiTexture(*texture, rgba32Buf, width, height);
}

void GfxRenderingAPIDeko3d::SetSamplerParameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt) {
    const auto found = mUiTextures.find(mUiSelected[static_cast<size_t>(sampler & 1)]);
    if (found != mUiTextures.end()) {
        found->second.Sampler = UiLegacySamplerKey(linear_filter, cms, cmt);
    }
}

bool GfxRenderingAPIDeko3d::SetNativeSamplerParameters(int sampler, const GfxNativeSamplerState& state) {
    if (sampler < 0 || sampler > 1) {
        return false;
    }
    const auto found = mUiTextures.find(mUiSelected[static_cast<size_t>(sampler)]);
    if (found != mUiTextures.end()) {
        found->second.Sampler = UiSamplerKey(state);
    }
    return true;
}

void GfxRenderingAPIDeko3d::DeleteTexture(uint32_t texId) {
    const auto found = mUiTextures.find(texId);
    if (found == mUiTextures.end()) {
        return;
    }
    RetireUi(found->second.Memory, found->second.ImageSlot);
    mUiTextures.erase(found);
}

// ---- state ------------------------------------------------------------------

void GfxRenderingAPIDeko3d::SetViewport(int x, int y, int width, int height) {
    mUiViewport = {x, y, width, height};
    mUiViewportSet = true;
}

void GfxRenderingAPIDeko3d::SetScissor(int x, int y, int width, int height) {
    mUiScissor = {x, y, width, height};
    mUiScissorSet = true;
}

void GfxRenderingAPIDeko3d::SetUseAlpha(bool useAlpha) {
    mUiUseAlpha = useAlpha;
}

bool GfxRenderingAPIDeko3d::SetOot3dNativeTransform(const float* row_major_matrix) {
    if (row_major_matrix == nullptr) {
        return false;
    }
    std::memcpy(mUiTransform.data(), row_major_matrix, sizeof(float) * mUiTransform.size());
    return true;
}

// ---- drawing ------------------------------------------------------------------

void GfxRenderingAPIDeko3d::DrawTriangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!mDeviceReady) {
        return;
    }
    const UiVertexLayout layout = ClassifyUiVertices(buf_vbo_len, buf_vbo_num_tris);
    if (mCurrentFramebuffer != 0 || layout == UiVertexLayout::Unsupported) {
        if (layout == UiVertexLayout::Unsupported && mCounters.UiDrawsSkipped == 0U) {
            Log("ui: unsupported triangle list (%zu floats, %zu triangles, framebuffer %d)", buf_vbo_len,
                buf_vbo_num_tris, mCurrentFramebuffer);
        }
        ++mCounters.UiDrawsSkipped;
        return;
    }
    const UiTexture* texture = nullptr;
    if (layout == UiVertexLayout::Textured) {
        const auto found = mUiTextures.find(mUiSelected[0]);
        if (found == mUiTextures.end() || found->second.Memory == nullptr) {
            ++mCounters.UiDrawsSkipped;
            return;
        }
        texture = &found->second;
    }
    const bool invertY = !mFramebuffers.empty() && mFramebuffers[0].InvertY;
    const uint32_t width = mDevice.Width();
    const uint32_t height = mDevice.Height();
    const UiRect whole = {0U, 0U, width, height};
    const UiRect viewport = mUiViewportSet ? GlRectToUpperLeft(mUiViewport[0], mUiViewport[1], mUiViewport[2],
                                                               mUiViewport[3], width, height, invertY)
                                           : whole;
    const UiRect scissor = mUiScissorSet ? GlRectToUpperLeft(mUiScissor[0], mUiScissor[1], mUiScissor[2],
                                                             mUiScissor[3], width, height, invertY)
                                         : whole;
    if (RecordUiDraw(buf_vbo, buf_vbo_len, buf_vbo_num_tris * 3U, layout, texture, viewport, scissor,
                     UiClipYScale(invertY), mUiUseAlpha, mUiTransform.data())) {
        ++mCounters.UiDraws;
    } else {
        ++mCounters.UiDrawsSkipped;
    }
}

bool GfxRenderingAPIDeko3d::RecordUiDraw(const float* vertices, size_t floats, size_t vertexCount,
                                         UiVertexLayout layout, const UiTexture* texture, const UiRect& viewport,
                                         const UiRect& scissor, float clipYScale, bool blend,
                                         const float* transform) {
    const bool textured = layout == UiVertexLayout::Textured;
    const DkShader* vertexShader = textured ? mUiTexturedVertex : mUiSolidVertex;
    const DkShader* fragmentShader = textured ? mUiTexturedFragment : mUiSolidFragment;
    if (vertexShader == nullptr || fragmentShader == nullptr || (textured && texture == nullptr) ||
        viewport.Width == 0U || viewport.Height == 0U || scissor.Width == 0U || scissor.Height == 0U) {
        return false;
    }
    BindWindowTarget();
    if (!mWindowBound) {
        return false;
    }
    const uint32_t vertexBytes = static_cast<uint32_t>(floats * sizeof(float));
    const StreamSpan params = mDevice.Stream(AlignUp(kUiParamsBytes, DK_UNIFORM_BUF_ALIGNMENT),
                                             DK_UNIFORM_BUF_ALIGNMENT);
    const StreamSpan stream = mDevice.Stream(vertexBytes, 16U);
    if (params.Cpu == nullptr || stream.Cpu == nullptr) {
        return false;
    }
    float values[20];
    std::memcpy(values, transform, 16U * sizeof(float));
    values[16] = clipYScale;
    values[17] = 0.0F;
    values[18] = 0.0F;
    values[19] = 0.0F;
    std::memcpy(params.Cpu, values, sizeof(values));
    std::memcpy(stream.Cpu, vertices, vertexBytes);

    DkCmdBuf cmd = mDevice.Cmd();
    FlushPicaInvalidations();
    DkRasterizerState rasterizer;
    dkRasterizerStateDefaults(&rasterizer);
    rasterizer.cullMode = DkFace_None;
    dkCmdBufBindRasterizerState(cmd, &rasterizer);
    DkDepthStencilState depthStencil;
    dkDepthStencilStateDefaults(&depthStencil);
    depthStencil.depthTestEnable = 0;
    depthStencil.depthWriteEnable = 0;
    depthStencil.stencilTestEnable = 0;
    dkCmdBufBindDepthStencilState(cmd, &depthStencil);
    DkColorState colorState;
    dkColorStateDefaults(&colorState);
    if (blend) {
        // GL Fast3D: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA).
        dkColorStateSetBlendEnable(&colorState, 0, true);
        DkBlendState blendState;
        dkBlendStateDefaults(&blendState);
        dkBlendStateSetOps(&blendState, DkBlendOp_Add, DkBlendOp_Add);
        dkBlendStateSetFactors(&blendState, DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha,
                               DkBlendFactor_SrcAlpha, DkBlendFactor_InvSrcAlpha);
        dkCmdBufBindBlendState(cmd, 0, &blendState);
    }
    dkCmdBufBindColorState(cmd, &colorState);
    DkColorWriteState writeState;
    dkColorWriteStateDefaults(&writeState);
    dkCmdBufBindColorWriteState(cmd, &writeState);

    const DkViewport dkViewport = {static_cast<float>(viewport.X), static_cast<float>(viewport.Y),
                                   static_cast<float>(viewport.Width), static_cast<float>(viewport.Height),
                                   0.0F, 1.0F};
    dkCmdBufSetViewports(cmd, 0, &dkViewport, 1);
    const DkScissor dkScissor = {scissor.X, scissor.Y, scissor.Width, scissor.Height};
    dkCmdBufSetScissors(cmd, 0, &dkScissor, 1);

    // xyzw, [uv], rgba from one interleaved buffer.
    const uint32_t stride = (textured ? kUiTexturedFloats : kUiSolidFloats) * sizeof(float);
    DkVtxAttribState attributes[3] = {};
    const auto attribute = [](uint32_t offset, DkVtxAttribSize size) {
        DkVtxAttribState state{};
        state.bufferId = 0;
        state.isFixed = 0;
        state.offset = offset;
        state.size = size;
        state.type = DkVtxAttribType_Float;
        state.isBgra = 0;
        return state;
    };
    uint32_t attributeCount = 0U;
    attributes[attributeCount++] = attribute(0U, DkVtxAttribSize_4x32);
    if (textured) {
        attributes[attributeCount++] = attribute(16U, DkVtxAttribSize_2x32);
        attributes[attributeCount++] = attribute(24U, DkVtxAttribSize_4x32);
    } else {
        attributes[attributeCount++] = attribute(16U, DkVtxAttribSize_4x32);
    }
    const DkVtxBufferState buffer = {stride, 0U};
    const DkBufExtents extent = {stream.Gpu, vertexBytes};
    dkCmdBufBindVtxAttribState(cmd, attributes, attributeCount);
    dkCmdBufBindVtxBufferState(cmd, &buffer, 1);
    dkCmdBufBindVtxBuffers(cmd, 0, &extent, 1);

    const DkShader* shaders[] = {vertexShader, fragmentShader};
    dkCmdBufBindShaders(cmd, DkStageFlag_Vertex | DkStageFlag_Fragment, shaders, 2);
    dkCmdBufBindUniformBuffer(cmd, DkStage_Vertex, 0, params.Gpu,
                              AlignUp(kUiParamsBytes, DK_UNIFORM_BUF_ALIGNMENT));
    if (textured) {
        dkCmdBufBindTexture(cmd, DkStage_Fragment, 0,
                            dkMakeTextureHandle(texture->ImageSlot, mResources.SamplerSlot(texture->Sampler)));
    }
    dkCmdBufDraw(cmd, DkPrimitive_Triangles, static_cast<uint32_t>(vertexCount), 1, 0, 0);
    return true;
}

// ---- ImGui ----------------------------------------------------------------------

void GfxRenderingAPIDeko3d::RenderImGui(const ImDrawData* data) {
    if (!mDeviceReady || data == nullptr || data->TotalVtxCount <= 0) {
        return;
    }
    if (mImGuiFontTexture == 0U) {
        // The atlas is built by ImGui::NewFrame on the game thread; its pixels
        // stay put afterwards (the GL backend reads them on this thread too).
        unsigned char* pixels = nullptr;
        int atlasWidth = 0;
        int atlasHeight = 0;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &atlasWidth, &atlasHeight);
        const uint32_t id = NewTexture();
        UiTexture& atlas = mUiTextures[id];
        atlas.Sampler.MinLinear = true;
        atlas.Sampler.MagLinear = true;
        if (pixels == nullptr || atlasWidth <= 0 || atlasHeight <= 0 ||
            !UploadUiTexture(atlas, pixels, static_cast<uint32_t>(atlasWidth), static_cast<uint32_t>(atlasHeight))) {
            DeleteTexture(id);
            return;
        }
        mImGuiFontTexture = id;
        Log("ui: imgui font atlas %dx%d", atlasWidth, atlasHeight);
    }
    const auto atlas = mUiTextures.find(mImGuiFontTexture);
    if (atlas == mUiTextures.end()) {
        return;
    }
    ++mCounters.ImGuiFrames;
    const uint32_t width = mDevice.Width();
    const uint32_t height = mDevice.Height();
    const float displayWidth = data->DisplaySize.x > 0.0F ? data->DisplaySize.x : static_cast<float>(width);
    const float displayHeight = data->DisplaySize.y > 0.0F ? data->DisplaySize.y : static_cast<float>(height);
    const float scaleX = static_cast<float>(width) / displayWidth;
    const float scaleY = static_cast<float>(height) / displayHeight;
    static constexpr float kIdentity[16] = {1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                                            0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
    const UiRect whole = {0U, 0U, width, height};
    std::vector<float> vertices;
    for (const ImDrawList* list : data->CmdLists) {
        for (const ImDrawCmd& command : list->CmdBuffer) {
            if (command.UserCallback != nullptr || command.ElemCount == 0U) {
                continue;
            }
            // Only the font atlas (TexID 0: no backend set one). ImTextureID is
            // a pointer in the Switch build and an integer elsewhere.
            const auto texture = (uintptr_t)command.GetTexID();
            if (texture != 0U && texture != mImGuiFontTexture) {
                ++mCounters.ImGuiCommandsSkipped;
                continue;
            }
            const UiRect scissor =
                ImGuiClipToScissor(command.ClipRect.x * scaleX, command.ClipRect.y * scaleY,
                                   command.ClipRect.z * scaleX, command.ClipRect.w * scaleY,
                                   data->DisplayPos.x * scaleX, data->DisplayPos.y * scaleY, width, height);
            if (scissor.Width == 0U || scissor.Height == 0U) {
                continue;
            }
            vertices.clear();
            vertices.reserve(static_cast<size_t>(command.ElemCount) * kUiTexturedFloats);
            for (uint32_t element = 0U; element < command.ElemCount; ++element) {
                const ImDrawIdx index = list->IdxBuffer[static_cast<int>(command.IdxOffset + element)];
                const ImDrawVert& vertex = list->VtxBuffer[static_cast<int>(command.VtxOffset + index)];
                const auto packed =
                    ImGuiVertexToUi(vertex.pos.x, vertex.pos.y, vertex.uv.x, vertex.uv.y, vertex.col,
                                    data->DisplayPos.x, data->DisplayPos.y, displayWidth, displayHeight);
                vertices.insert(vertices.end(), packed.begin(), packed.end());
            }
            if (RecordUiDraw(vertices.data(), vertices.size(), command.ElemCount, UiVertexLayout::Textured,
                             &atlas->second, whole, scissor, UiClipYScale(false), true, kIdentity)) {
                ++mCounters.ImGuiCommands;
            }
        }
    }
}

void GfxRenderingAPIDeko3d::LogUiCounters(const char* when) {
    Log("ui (%s): draws=%llu skipped=%llu textures=%llu live=%zu failures=%llu; imgui frames=%llu commands=%llu "
        "skipped=%llu",
        when, static_cast<unsigned long long>(mCounters.UiDraws),
        static_cast<unsigned long long>(mCounters.UiDrawsSkipped),
        static_cast<unsigned long long>(mCounters.UiTexturesCreated), mUiTextures.size(),
        static_cast<unsigned long long>(mCounters.UiTextureFailures),
        static_cast<unsigned long long>(mCounters.ImGuiFrames),
        static_cast<unsigned long long>(mCounters.ImGuiCommands),
        static_cast<unsigned long long>(mCounters.ImGuiCommandsSkipped));
}

} // namespace Fast

#endif
