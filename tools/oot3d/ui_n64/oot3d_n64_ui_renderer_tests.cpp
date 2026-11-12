#include "oot3d_n64_ui_renderer.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_set>
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
        pixels.native_stand_in = !ready && nativeStandIn;
        return true;
    }
    std::uint64_t Revision() const noexcept override { return revision; }
    bool ready = false;
    bool nativeStandIn = false;
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

// test.7: the TopScreen item icons drew the game's art for ~0.5 s, then the
// pack's. A first sight whose replacement is decoding, with only the game's
// pixels to show, is not drawn until the replacement comes (polled at every
// call), for kUiHoldPolls calls at most; an earlier revision's texture of
// the key keeps drawing meanwhile.
void TestHeldFirstDraws() {
    using namespace oot3d::ui;
    const std::vector<UiPrimitive> primitives = {
        TexturedPrimitive("oot3d/native/a"), TexturedPrimitive("oot3d/native/a")};
    {
        PackProvider provider;
        provider.nativeStandIn = true;
        SizingBackend backend;
        N64UiFast3dRenderer renderer(backend, &provider);
        const auto frame = [&] {
            std::string error;
            Require(renderer.Render(primitives, 400, 240, N64UiCanvasMode::NativeTopScreen400x240,
                                    renderer.PrepareTextures(primitives), &error),
                    "held UI rendering failed: " + error);
        };
        frame();
        frame();
        frame();
        Require(backend.widths.empty() && backend.drawn.empty() && renderer.Stats().held_draws == 6U,
                "a texture whose replacement was decoding drew the old art");
        Require(provider.calls == 3, "a held texture was not polled at every call");
        provider.ready = true;
        frame();
        Require(provider.calls == 4 && backend.widths == std::vector<uint16_t>{6U} &&
                    backend.drawn == std::vector<uint32_t>({1U, 1U}),
                "the replacement did not draw as soon as it was ready");
        frame();
        Require(provider.calls == 4, "a final texture was resolved again");

        // Turned off and on again (a new revision): the resident texture
        // keeps drawing while the replacement decodes again, polled every
        // few calls (test.13: polled at every call, the game thread lost
        // 25-50 ms per frame).
        provider.ready = false;
        provider.revision = 2U;
        frame();
        Require(provider.calls == 5 && backend.widths.size() == 1U && backend.deleted.empty() &&
                    backend.drawn.back() == 1U,
                "a held revision dropped the texture that was drawing");
        for (std::uint32_t call = 1U; call < kUiProvisionalPollInterval; ++call) frame();
        Require(provider.calls == 5 && backend.drawn.back() == 1U && renderer.Stats().held_draws == 6U,
                "a texture that draws was polled at every call while its replacement decoded");
        provider.ready = true;
        frame();
        Require(provider.calls == 6 && backend.widths == std::vector<uint16_t>({6U, 6U}) &&
                    backend.deleted == std::vector<uint32_t>{1U} && backend.drawn.back() == 2U,
                "the replacement of a new revision did not replace the resident texture");
    }
    {
        // A replacement that never comes: the game's pixels after the hold.
        PackProvider provider;
        provider.nativeStandIn = true;
        SizingBackend backend;
        N64UiFast3dRenderer renderer(backend, &provider);
        for (std::uint32_t call = 0; call < kUiHoldPolls; ++call) {
            std::string error;
            Require(renderer.Render(primitives, 400, 240, N64UiCanvasMode::NativeTopScreen400x240,
                                    renderer.PrepareTextures(primitives), &error),
                    "held UI rendering failed");
        }
        Require(backend.widths == std::vector<uint16_t>{2U} && backend.drawn.size() == 2U &&
                    renderer.Stats().holds_expired == 1U && provider.calls == int(kUiHoldPolls),
                "an expired hold did not draw the game's pixels");
        // Then polled every few calls as any provisional texture.
        for (int index = 0; index < 8; ++index) (void)renderer.PrepareTextures(primitives);
        Require(provider.calls == int(kUiHoldPolls) + 1, "an expired hold was still polled at every call");
    }
}

// test.13: each HD texture change (on, off, another size, the index ready)
// resolved every HUD texture again in one call and polled those still
// decoding at every call: 25-50 ms more game thread per frame for ~10 frames
// (hitches.txt:1629-1680). A new revision resolves the textures that draw at
// most kUiResolvesPerPrepare per call, each drawing its earlier texture until
// then.
void TestRevisionResolvesAreSpread() {
    using namespace oot3d::ui;
    std::vector<UiPrimitive> primitives;
    for (int index = 0; index < 20; ++index) {
        primitives.push_back(TexturedPrimitive("oot3d/native/t" + std::to_string(index)));
    }
    PackProvider provider;
    provider.nativeStandIn = true;
    provider.ready = true;
    SizingBackend backend;
    N64UiFast3dRenderer renderer(backend, &provider);
    int mostResolves = 0;
    const auto frame = [&] {
        const int calls = provider.calls;
        const size_t drawn = backend.drawn.size();
        std::string error;
        Require(renderer.Render(primitives, 400, 240, N64UiCanvasMode::NativeTopScreen400x240,
                                renderer.PrepareTextures(primitives), &error),
                "spread UI rendering failed: " + error);
        Require(backend.drawn.size() - drawn == primitives.size(), "a texture that draws was not drawn");
        mostResolves = std::max(mostResolves, provider.calls - calls);
    };
    frame();
    Require(provider.calls == 20 && backend.widths.size() == 20U, "first sights were not resolved at once");
    mostResolves = 0;

    // HD textures on (a new revision, the replacements decoding): the
    // textures keep drawing, resolved six per call, then polled.
    provider.revision = 2U;
    provider.ready = false;
    for (int call = 0; call < 4; ++call) frame();
    Require(provider.calls == 40 && renderer.Stats().deferred_resolves == 14U + 8U + 2U,
            "a new revision was not resolved six per call");
    for (int call = 0; call < 24; ++call) frame();
    Require(backend.widths.size() == 20U && backend.deleted.empty() && renderer.Stats().held_draws == 0U,
            "a texture that draws was held or replaced by the game's pixels");
    provider.ready = true;
    for (int call = 0; call < 16; ++call) frame();
    Require(backend.widths.size() == 40U && backend.deleted.size() == 20U,
            "the replacements did not all come");
    Require(mostResolves <= int(kUiResolvesPerPrepare), "a call resolved more than its budget");

    // HD textures off (another revision, final pixels at once): replaced six
    // per call, each drawing its pack image until then.
    provider.revision = 3U;
    mostResolves = 0;
    const int before = provider.calls;
    for (int call = 0; call < 4; ++call) frame();
    Require(provider.calls - before == 20 && backend.widths.size() == 60U && mostResolves == 6,
            "turning the pack off did not resolve every texture within four calls");
}

// A texture pack as the HUD sees it: an image is queued when asked for
// (ahead, or on its first Resolve) once the pack's index is ready, and is
// ready when the worker has decoded it (Decode). Until then the game's own
// pixels stand in (held at a first sight). The index becoming ready is a new
// revision.
class AsyncPackProvider final : public oot3d::ui::UiTextureProvider {
  public:
    bool Resolve(const oot3d::ui::UiTextureIdentity& identity, oot3d::ui::UiTexturePixels& pixels,
                 std::string*) const override {
        ++resolves;
        const bool isReady = ready.count(identity.semantic_name) != 0U;
        if (!isReady) {
            ++notReadyResolves;
            if (indexReady) queued.insert(identity.semantic_name);
        }
        const uint16_t extent = isReady ? 6U : 2U;
        pixels.width = extent;
        pixels.height = extent;
        pixels.rgba8.assign(std::size_t(extent) * extent * 4U, 0xFF);
        pixels.provisional = !isReady;
        pixels.native_stand_in = !isReady;
        return true;
    }
    std::uint64_t Revision() const noexcept override { return indexReady ? 2U : 1U; }
    void RequestAhead(const oot3d::ui::UiTextureIdentity& identity) const override {
        ++aheadCalls;
        if (indexReady && ready.count(identity.semantic_name) == 0U) queued.insert(identity.semantic_name);
    }
    // The pack worker decodes what is queued.
    void Decode() {
        ready.insert(queued.begin(), queued.end());
        queued.clear();
    }
    bool indexReady = false;
    mutable std::unordered_set<std::string> queued, ready;
    mutable int resolves = 0, notReadyResolves = 0, aheadCalls = 0;
};

// test.8: on the first start (empty HUD list) two HUD images were first seen
// in play and held up to 263 ms. The HUD's textures are asked for ahead from
// the title screen on (again once the pack's index is ready), so the HUD's
// first frame draws the pack's art; an item equipped for the first time is
// another region of the item atlas, already drawn.
void TestRequestAhead() {
    using namespace oot3d::ui;
    std::vector<UiTextureIdentity> hud(3U);
    hud[0].semantic_name = "oot3d/native/pause_shared/pause_top_page";
    hud[1].semantic_name = "oot3d/native/pause_shared/item_icons";
    hud[2].semantic_name = "oot3d/native/pause_shared/number_glyphs";
    std::vector<UiPrimitive> firstFrame;
    for (const auto& identity : hud) {
        firstFrame.push_back(TexturedPrimitive(identity.semantic_name));
        firstFrame.back().uv = {0.0F, 0.0F, 0.0625F, 0.0625F};
    }
    const auto frame = [](N64UiFast3dRenderer& renderer, const std::vector<UiPrimitive>& primitives) {
        std::string error;
        Require(renderer.Render(primitives, 400, 240, N64UiCanvasMode::NativeTopScreen400x240,
                                renderer.PrepareTextures(primitives), &error),
                "ahead UI rendering failed: " + error);
    };
    {
        AsyncPackProvider provider;
        SizingBackend backend;
        N64UiFast3dRenderer renderer(backend, &provider);
        // Title screen, the pack still indexing: one request per call.
        Require(!renderer.RequestAhead(hud) && !renderer.RequestAhead(hud) && renderer.RequestAhead(hud) &&
                    provider.aheadCalls == 3 && renderer.Stats().ahead_requests == 3U,
                "the HUD textures were not asked for ahead one per call");
        Require(renderer.RequestAhead(hud) && provider.aheadCalls == 3,
                "a texture was asked for ahead twice at one revision");
        Require(provider.queued.empty() && provider.resolves == 0, "asking ahead resolved pixels");
        // The index is ready (a new revision): asked again, now queued.
        provider.indexReady = true;
        for (int call = 0; call < 3; ++call) (void)renderer.RequestAhead(hud);
        Require(provider.aheadCalls == 6 && provider.queued.size() == 3U,
                "a new revision did not ask for the HUD textures again");
        // Decoded during the title screen; the HUD's first frame.
        provider.Decode();
        frame(renderer, firstFrame);
        Require(provider.notReadyResolves == 0 && renderer.Stats().held_draws == 0U &&
                    backend.widths == std::vector<uint16_t>({6U, 6U, 6U}) && backend.drawn.size() == 3U,
                "the HUD's first frame did not draw the pack's art");
        // An item equipped for the first time: another region of the atlas.
        std::vector<UiPrimitive> equipped = firstFrame;
        equipped[1].uv = {0.5F, 0.25F, 0.0625F, 0.0625F};
        const int resolves = provider.resolves;
        frame(renderer, equipped);
        Require(provider.resolves == resolves && backend.widths.size() == 3U && backend.drawn.size() == 6U &&
                    renderer.Stats().held_draws == 0U,
                "a first equip changed the HUD's item texture");
        // Drawn already: not asked for ahead again.
        Require(renderer.RequestAhead(hud) && provider.aheadCalls == 6, "a drawn texture was asked for ahead");
    }
    {
        // Without asking ahead (test.8): the first frame holds the HUD.
        AsyncPackProvider provider;
        provider.indexReady = true;
        SizingBackend backend;
        N64UiFast3dRenderer renderer(backend, &provider);
        frame(renderer, firstFrame);
        Require(renderer.Stats().held_draws == 3U && backend.widths.empty(),
                "the reference HUD without ahead requests was not held");
    }
}

} // namespace

int main() {
    using namespace oot3d::ui;
    TestProvisionalTextures();
    TestHeldFirstDraws();
    TestRevisionResolvesAreSpread();
    TestRequestAhead();
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
