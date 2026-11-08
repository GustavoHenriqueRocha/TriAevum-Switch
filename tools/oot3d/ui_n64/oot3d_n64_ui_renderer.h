#pragma once

#include "oot3d/renderer/ui_render_backend.h"
#include "oot3d_ui/ui_primitives.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oot3d::ui {

enum class N64UiCanvasMode : std::uint8_t {
    Widescreen16x9,
    NativeTopScreen400x240,
};

struct N64UiRendererStats {
    std::uint64_t frames = 0;
    std::uint64_t frames_with_primitives = 0;
    std::uint64_t primitives_received = 0;
    std::uint64_t primitives_drawn = 0;
    std::uint64_t solid_draws = 0;
    std::uint64_t textured_draws = 0;
    std::uint64_t draw_calls = 0;
    std::uint64_t triangles = 0;
    std::uint64_t texture_uploads = 0;
    std::uint64_t texture_cache_hits = 0;
    std::uint64_t native_texture_resolves = 0;
    std::uint64_t native_texture_failures = 0;
    std::uint64_t missing_texture_resources = 0;
    std::uint64_t unsupported_texture_resources = 0;
    // Primitives not drawn while their texture's replacement was still
    // decoding (held first draws), and textures whose hold ran out (drawn
    // with the game's own pixels until the replacement came).
    std::uint64_t held_draws = 0;
    std::uint64_t holds_expired = 0;
    // Textures asked for ahead of their first draw (RequestAhead).
    std::uint64_t ahead_requests = 0;
    // Resolves of textures that draw already (a new provider revision) left
    // for a later call: over kUiResolvesPerPrepare.
    std::uint64_t deferred_resolves = 0;
};

struct UiTexturePixels {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::vector<std::uint8_t> rgba8;
    // Pixels shared instead of `rgba8` (a texture pack image, a cached
    // decode): handed to the renderer without a copy on the game thread
    // (test.14: a 3x HUD image, up to 19 MB, was copied there). Data() is
    // whichever is set.
    std::shared_ptr<const std::vector<std::uint8_t>> shared_rgba8;
    [[nodiscard]] std::span<const std::uint8_t> Data() const noexcept {
        return shared_rgba8 ? std::span<const std::uint8_t>(*shared_rgba8)
                            : std::span<const std::uint8_t>(rgba8);
    }
    // Valid pixels standing in for a final payload that is still being
    // prepared (a custom texture decoding); the texture is resolved again.
    bool provisional = false;
    // Provisional pixels that are the game's own art (no HD stand-in): the
    // replacement the player expects (a texture pack image) is decoding, so
    // the renderer holds the texture's first draws instead of showing the old
    // art for a moment (kUiHoldPolls).
    bool native_stand_in = false;
};

// Backend-independent boundary for textures whose authoritative payload lives
// in the running OoT3D process rather than in an OTR archive.
class UiTextureProvider {
  public:
    virtual ~UiTextureProvider() = default;
    virtual bool Resolve(const UiTextureIdentity& identity,
                         UiTexturePixels& pixels,
                         std::string* error = nullptr) const = 0;
    // Changes whenever previously resolved payloads may have become stale
    // (for example a texture pack was turned on, off or reloaded).
    virtual std::uint64_t Revision() const noexcept { return 0U; }
    // Starts preparing the final payload of a texture before its first
    // Resolve (a texture pack image decodes on its worker meanwhile), without
    // returning pixels. Nothing to start by default.
    virtual void RequestAhead(const UiTextureIdentity& identity) const {
        (void)identity;
    }
};

// A texture payload read from its provider ahead of the Render that first
// uses it. Resolution reads the running process, so a threaded host prepares
// on the guest side and the renderer only consumes the result.
struct PreparedUiTexture {
    enum class Status : std::uint8_t { Resolved, MissingProvider, Invalid };
    std::string key;
    Status status = Status::Invalid;
    bool native = false;
    // A first sight held (UiTexturePixels::native_stand_in): no pixels; the
    // primitives using it are not drawn until its final pixels are staged. A
    // resident texture of the key (an earlier revision's) keeps drawing.
    bool held = false;
    UiTexturePixels pixels;
    std::string error;
};
using PreparedUiTextures = std::vector<PreparedUiTexture>;

// Calls of PrepareTextures that hold a texture's first draws at most (the
// texture is polled at each). A HUD image of the player's pack decodes on
// its own worker in a few frames; past this the game's own pixels are drawn
// until the replacement comes, as before.
inline constexpr std::uint32_t kUiHoldPolls = 30U;
// A provisional texture (its replacement decoding) is polled again after
// this many calls of PrepareTextures.
inline constexpr std::uint32_t kUiProvisionalPollInterval = 8U;
// Resolves one PrepareTextures call makes at most beyond first sights and
// held ones: a new provider revision (a texture pack turned on or off, its
// index ready, another size) resolves the textures that draw a few per call,
// each drawing its earlier texture until then, and provisional polls share
// it. With the provider's identity cache (oot3d_native_ui_texture_provider)
// such a resolve reads and hashes the game's texture, no decode.
inline constexpr std::uint32_t kUiResolvesPerPrepare = 6U;
// And the time those resolves may take in one call (checked before each): the
// TopScreen HUD prepares twice per frame, so a frame spends at most about
// twice this on them (plus one resolve each).
inline constexpr std::chrono::microseconds kUiResolveTimeBudget{1500};

std::uint64_t N64UiSolidShaderId0() noexcept;
std::uint64_t N64UiTexturedShaderId0() noexcept;
std::uint64_t N64UiShaderId1() noexcept;
std::string_view ResolveN64UiTexturePath(
    std::string_view semanticName) noexcept;
std::string BuildUiTextureCacheKey(const UiTextureIdentity& identity);

// Draws typed N64-layout UI primitives after the native PICA scene through a
// renderer-owned 2D backend contract.
class N64UiFast3dRenderer {
  public:
    explicit N64UiFast3dRenderer(
        Oot3d::Renderer::UiRenderBackend& renderingApi,
        const UiTextureProvider* textureProvider = nullptr) noexcept;
    ~N64UiFast3dRenderer();

    N64UiFast3dRenderer(const N64UiFast3dRenderer&) = delete;
    N64UiFast3dRenderer& operator=(const N64UiFast3dRenderer&) = delete;

    bool Render(std::span<const UiPrimitive> primitives,
                std::uint32_t framebufferWidth,
                std::uint32_t framebufferHeight, N64UiCanvasMode canvasMode,
                std::string* error = nullptr);
    // Resolves every texture a Render of these primitives would resolve for
    // the first time. Prepared keys are remembered (until Release), so each
    // payload is read once, as Render alone would; a provisional payload is
    // resolved again every kUiProvisionalPollInterval calls, and a new
    // provider Revision resolves every texture again, at most
    // kUiResolvesPerPrepare per call for those that draw already (they keep
    // drawing until then, and while only the game's own pixels stand in for
    // the replacement). A first sight with only the game's own pixels while
    // its replacement decodes is held (PreparedUiTexture::held).
    // Provisional/revision handling ported from upstream TriAevum
    // (coccofresco/TriAevum commit 68d527c6, GPL-3.0-or-later).
    [[nodiscard]] PreparedUiTextures
    PrepareTextures(std::span<const UiPrimitive> primitives);
    // Hands prepared payloads to the renderer without drawing (a frame that
    // renders nothing still owns them for its next Render). A payload
    // prepared again replaces the resident texture of the same key.
    void Stage(PreparedUiTextures prepared);
    // Asks the provider ahead for the first of `identities` (in order) not
    // asked for nor prepared at the provider's current Revision: one per
    // call, so the cost (a read and decode of the game's texture to identify
    // it) is spread over frames. A new Revision asks for every one again: a
    // texture pack that finished indexing queues their decodes then. Returns
    // whether every one of them has been asked for at this Revision.
    bool RequestAhead(std::span<const UiTextureIdentity> identities);
    // Render without reading the provider: first uses consume `prepared`.
    bool Render(std::span<const UiPrimitive> primitives,
                std::uint32_t framebufferWidth,
                std::uint32_t framebufferHeight, N64UiCanvasMode canvasMode,
                PreparedUiTextures prepared, std::string* error = nullptr);
    void SetTextureProvider(const UiTextureProvider* textureProvider) noexcept;
    void Release() noexcept;

    const N64UiRendererStats& Stats() const noexcept;

  private:
    struct ResidentTexture {
        std::uint32_t renderer_id = 0;
        std::uint16_t width = 0;
        std::uint16_t height = 0;
    };

    const ResidentTexture* ResolveTexture(const UiTextureIdentity& identity,
                                          std::string* error, bool* held);
    Oot3d::Renderer::UiRenderBackend& rendering_api_;
    const UiTextureProvider* texture_provider_ = nullptr;
    std::unordered_map<std::string, ResidentTexture> textures_;
    std::unordered_set<std::string> failed_textures_;
    // Preparing side: keys already handed to the renderer at this revision,
    // those handed with provisional pixels (with the call they were polled
    // at), keys handed drawable pixels at any revision (resident on the
    // rendering side), and the provider revision keys were resolved at.
    std::unordered_set<std::string> prepared_keys_;
    std::unordered_map<std::string, std::uint64_t> provisional_polled_;
    std::unordered_set<std::string> drawable_keys_;
    std::uint64_t prepared_revision_ = 0;
    std::uint64_t prepare_calls_ = 0;
    // Keys whose first draws are held, and the calls they were polled at.
    std::unordered_map<std::string, std::uint32_t> hold_polls_;
    // Keys asked for ahead and the provider revision they were asked at.
    std::unordered_set<std::string> ahead_keys_;
    std::uint64_t ahead_revision_ = 0;
    // Rendering side: held keys without a resident texture (not drawn).
    std::unordered_set<std::string> held_keys_;
    // Rendering side: prepared payloads not consumed yet.
    std::unordered_map<std::string, PreparedUiTexture> staged_textures_;
    // Per-frame scratch (storage kept between frames).
    std::vector<const UiPrimitive*> ordered_scratch_;
    std::string key_scratch_;
    N64UiRendererStats stats_;
};

} // namespace oot3d::ui
