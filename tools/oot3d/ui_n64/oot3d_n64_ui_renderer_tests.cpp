#include "oot3d_n64_ui_renderer.h"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void Fail(std::string_view message) {
    std::cerr << "oot3d_n64_ui_renderer_tests: " << message << '\n';
    std::exit(1);
}

void Require(bool condition, std::string_view message) {
    if (!condition) {
        Fail(message);
    }
}

class RecordingBackend final : public Oot3d::Renderer::UiRenderBackend {
  public:
    bool PrepareUiOverlay(const Oot3d::Renderer::UiOverlayViewport&,
                          std::string*) override {
        return true;
    }
    bool IsUiFramebufferYInverted() const noexcept override { return false; }
    uint32_t CreateUiTexture(std::span<const uint8_t>, uint16_t, uint16_t,
                             std::string*) override {
        return ++created;
    }
    void DeleteUiTexture(uint32_t) noexcept override {}
    bool DrawUiTriangles(std::span<const Oot3d::Renderer::UiOverlayVertex>,
                         uint32_t textureId, std::string*) override {
        drawnTextures.push_back(textureId);
        return true;
    }
    uint32_t created = 0;
    std::vector<uint32_t> drawnTextures;
};

class CountingProvider final : public oot3d::ui::UiTextureProvider {
  public:
    bool Resolve(const oot3d::ui::UiTextureIdentity& identity,
                 oot3d::ui::UiTexturePixels& pixels,
                 std::string* error) const override {
        ++calls;
        if (identity.semantic_name == "oot3d/native/broken") {
            if (error != nullptr) *error = "broken payload";
            return false;
        }
        pixels.width = 2;
        pixels.height = 1;
        pixels.rgba8.assign(8, 0xFF);
        return true;
    }
    mutable int calls = 0;
};

oot3d::ui::UiPrimitive TexturedPrimitive(std::string_view name) {
    oot3d::ui::UiPrimitive primitive;
    primitive.texture.semantic_name = std::string(name);
    primitive.destination = {0.0F, 0.0F, 8.0F, 8.0F};
    return primitive;
}

// Prepared rendering reads the provider only while preparing, once per key,
// and renders exactly what the reading Render renders.
void TestPreparedTextures() {
    using namespace oot3d::ui;
    const std::vector<UiPrimitive> primitives = {
        TexturedPrimitive("oot3d/native/a"), TexturedPrimitive("oot3d/native/b"),
        TexturedPrimitive("oot3d/native/a"),
        TexturedPrimitive("oot3d/native/broken")};
    CountingProvider readingProvider;
    RecordingBackend readingBackend;
    std::string readingError;
    {
        N64UiFast3dRenderer renderer(readingBackend, &readingProvider);
        Require(!renderer.Render(primitives, 400, 240,
                                 N64UiCanvasMode::NativeTopScreen400x240,
                                 &readingError),
                "a broken UI texture was not reported");
    }
    CountingProvider provider;
    RecordingBackend backend;
    N64UiFast3dRenderer renderer(backend, &provider);
    auto prepared = renderer.PrepareTextures(primitives);
    Require(prepared.size() == 3U && provider.calls == 3,
            "UI texture preparation did not resolve each key once");
    std::string error;
    Require(!renderer.Render(primitives, 400, 240,
                             N64UiCanvasMode::NativeTopScreen400x240,
                             std::move(prepared), &error),
            "a prepared broken UI texture was not reported");
    Require(provider.calls == 3,
            "prepared UI rendering read the texture provider");
    Require(backend.drawnTextures == readingBackend.drawnTextures &&
                backend.created == readingBackend.created &&
                error == readingError && error == "broken payload",
            "prepared UI rendering differs from reading rendering");
    Require(renderer.PrepareTextures(primitives).empty() && provider.calls == 3,
            "UI texture preparation repeated a prepared key");
    const auto stats = renderer.Stats();
    Require(stats.native_texture_resolves == 3U &&
                stats.native_texture_failures == 1U &&
                stats.texture_uploads == 2U,
            "prepared UI texture statistics changed");
    renderer.Release();
    Require(renderer.PrepareTextures(primitives).size() == 3U &&
                provider.calls == 6,
            "Release kept prepared UI texture keys");
}

class SizingBackend final : public Oot3d::Renderer::UiRenderBackend {
  public:
    bool PrepareUiOverlay(const Oot3d::Renderer::UiOverlayViewport&,
                          std::string*) override {
        return true;
    }
    bool IsUiFramebufferYInverted() const noexcept override { return false; }
    uint32_t CreateUiTexture(std::span<const uint8_t>, uint16_t width, uint16_t,
                             std::string*) override {
        widths.push_back(width);
        return static_cast<uint32_t>(widths.size());
    }
    void DeleteUiTexture(uint32_t id) noexcept override { deleted.push_back(id); }
    bool DrawUiTriangles(std::span<const Oot3d::Renderer::UiOverlayVertex>,
                         uint32_t textureId, std::string*) override {
        drawn.push_back(textureId);
        return true;
    }
    std::vector<uint16_t> widths;
    std::vector<uint32_t> deleted, drawn;
};

// A replacement still decoding: native pixels stand in (provisional) and are
// polled again; the final pixels, of another size, replace them. A new
// provider revision (a texture pack turned off) resolves everything again.
class PackProvider final : public oot3d::ui::UiTextureProvider {
  public:
    bool Resolve(const oot3d::ui::UiTextureIdentity&, oot3d::ui::UiTexturePixels& pixels,
                 std::string*) const override {
        ++calls;
        const uint16_t extent = ready ? 6U : 2U;
        pixels.width = extent;
        pixels.height = extent;
        pixels.rgba8.assign(std::size_t(extent) * extent * 4U, 0xFF);
        pixels.provisional = !ready;
        return true;
    }
    std::uint64_t Revision() const noexcept override { return revision; }
    bool ready = false;
    std::uint64_t revision = 1U;
    mutable int calls = 0;
};

void TestProvisionalTextures() {
    using namespace oot3d::ui;
    const std::vector<UiPrimitive> primitives = {
        TexturedPrimitive("oot3d/native/a"), TexturedPrimitive("oot3d/native/a")};
    PackProvider provider;
    SizingBackend backend;
    N64UiFast3dRenderer renderer(backend, &provider);
    const auto frame = [&] {
        std::string error;
        Require(renderer.Render(primitives, 400, 240, N64UiCanvasMode::NativeTopScreen400x240,
                                renderer.PrepareTextures(primitives), &error),
                "provisional UI rendering failed");
    };
    frame();
    Require(provider.calls == 1 && backend.widths == std::vector<uint16_t>{2U},
            "a provisional texture was not drawn once, or was resolved per primitive");
    // Polled every few frames, not every frame; never uploaded again while
    // still provisional.
    for (int index = 0; index < 7; ++index) frame();
    Require(provider.calls == 1 && backend.widths.size() == 1U,
            "a provisional texture was polled every frame or uploaded again");
    provider.ready = true;
    for (int index = 0; index < 8; ++index) frame();
    Require(provider.calls == 2 && backend.widths == std::vector<uint16_t>({2U, 6U}) &&
                backend.deleted == std::vector<uint32_t>{1U} && backend.drawn.back() == 2U,
            "the final texture of another size did not replace the provisional one");
    for (int index = 0; index < 16; ++index) frame();
    Require(provider.calls == 2, "a final texture was resolved again");
    provider.revision = 2U;
    frame();
    Require(provider.calls == 3 && backend.widths.size() == 3U &&
                backend.deleted == std::vector<uint32_t>({1U, 2U}),
            "a new provider revision did not resolve and replace the texture");
}

} // namespace

int main() {
    using namespace oot3d::ui;
    TestProvisionalTextures();
    Require(N64UiSolidShaderId0() != 0U &&
                N64UiTexturedShaderId0() != 0U &&
                N64UiSolidShaderId0() != N64UiTexturedShaderId0(),
            "solid and textured shader contracts are not distinct");
    Require(N64UiShaderId1() == 1U,
            "N64 UI alpha shader option changed");
    Require(ResolveN64UiTexturePath("n64/file_select/highlight") ==
                "__OTR__textures/title_static/gFileSelBigButtonHighlightTex",
            "file-select texture semantic did not resolve to the N64 asset");
    Require(ResolveN64UiTexturePath("n64/name_entry/backspace") ==
                "__OTR__textures/title_static/gFileSelBackspaceButtonTex" &&
                ResolveN64UiTexturePath(
                    "n64/name_entry/highlight/character") ==
                    "__OTR__textures/title_static/gFileSelCharHighlightTex",
            "name-entry texture semantics did not resolve to N64 assets");
    Require(ResolveN64UiTexturePath("n64/name_entry/confirm/prompt") ==
                "__OTR__textures/title_static/gFileSelAreYouSureENGTex" &&
                ResolveN64UiTexturePath("n64/name_entry/confirm/yes") ==
                    "__OTR__textures/title_static/gFileSelYesButtonENGTex" &&
                ResolveN64UiTexturePath("n64/name_entry/confirm/quit") ==
                    "__OTR__textures/title_static/gFileSelQuitButtonENGTex",
            "name-entry confirmation did not resolve to N64 assets");
    Require(ResolveN64UiTexturePath("n64/file_select/missing").empty(),
            "unknown UI texture semantic unexpectedly resolved");
    UiTextureIdentity nativeIdentity;
    nativeIdentity.semantic_name =
        "oot3d/native/pause_shared/pause_top_page";
    nativeIdentity.guest_resource_address = 0x08123456U;
    nativeIdentity.guest_surface_address = 0x20100000U;
    Require(BuildUiTextureCacheKey(nativeIdentity) ==
                "oot3d/native/pause_shared/pause_top_page@08123456:20100000",
            "native texture cache identity omitted a guest address");
    UiTextureIdentity relocatedIdentity = nativeIdentity;
    relocatedIdentity.guest_surface_address += 0x1000U;
    Require(BuildUiTextureCacheKey(relocatedIdentity) !=
                BuildUiTextureCacheKey(nativeIdentity),
            "relocated native texture reused a stale cache identity");
    UiTextureIdentity archiveIdentity;
    archiveIdentity.semantic_name = "n64/file_select/highlight";
    Require(BuildUiTextureCacheKey(archiveIdentity) ==
                archiveIdentity.semantic_name,
            "archive texture cache identity unexpectedly changed");
    TestPreparedTextures();
    std::cout << "oot3d_n64_ui_renderer_tests: ok\n";
    return 0;
}
