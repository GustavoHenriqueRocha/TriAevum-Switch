#pragma once

#include "oot3d/renderer/azahar_texture_pack.h"
#include "oot3d_n64_ui_renderer.h"
#include "oot3d_native_pica_submission.h"
#include "oot3d_top_screen_texture_overrides.h"
#include "oot3d_ui/ui_localized_menu_resources.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Oot3dNativeGame {

struct Oot3dNativeUiTextureEncoding {
    std::uint8_t native_pica_format = 0;
    std::size_t encoded_byte_count = 0;
};

struct Oot3dNativeUiTextureDescriptor {
    oot3d::ui::UiPauseSharedTextureSlot slot =
        oot3d::ui::UiPauseSharedTextureSlot::Count;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::vector<Oot3dNativeUiTextureEncoding> encodings;

    const Oot3dNativeUiTextureEncoding* FindEncoding(
        std::uint8_t nativePicaFormat) const noexcept;
};

std::optional<Oot3dNativeUiTextureDescriptor>
ResolveOot3dNativeUiTextureDescriptor(std::string_view semanticName,
                                      std::string* error = nullptr);

struct Oot3dNativeUiTextureProviderStats {
    std::uint64_t resolve_calls = 0;
    std::uint64_t physical_reads = 0;
    std::uint64_t translated_guest_reads = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t runtime_metadata_reads = 0;
    std::uint64_t runtime_metadata_failures = 0;
    std::uint64_t contract_mismatches = 0;
    std::uint64_t decode_failures = 0;
    std::uint64_t override_checks = 0;
    std::uint64_t overrides_applied = 0;
    std::uint64_t overrides_already_applied = 0;
    std::uint64_t override_no_matches = 0;
    std::uint64_t profile_asset_resolves = 0;
    std::uint64_t profile_asset_decode_failures = 0;
    // TopScreen profile textures no pack provides (the release cannot ship
    // the mod they come from); each resolved to one transparent texel.
    std::uint64_t profile_asset_absent = 0;
    std::uint64_t custom_texture_replacements = 0;
    std::uint64_t custom_texture_pending = 0;
    // Textures offered to the custom source ahead of their first draw.
    std::uint64_t custom_texture_ahead = 0;
};

// A decoded UI texture offered to a custom texture source (a texture pack,
// the TopScreen HD UI atlas and HD images). Its identity is the one a native
// PICA upload of the game's own texture has: the encoded base level before
// this port's TopScreen CTXB override, since TopScreen 3.x leaves those
// CTXBs as the game has them (texture packs and the mod's own HD images are
// named after them). `rgba8` is that texture decoded.
struct Oot3dNativeUiCustomTextureRequest {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint8_t native_pica_format = 0;
    std::span<const std::uint8_t> encoded_base_level;
    std::span<const std::uint8_t> rgba8;
    // Asked for before the texture's first draw (RequestAhead): the source
    // starts preparing its replacement; the result is not used.
    bool ahead = false;
};

struct Oot3dNativeUiCustomTexture {
    // Pending: the final image is still decoding; the texture is resolved
    // again later. Its stand-in is `rgba8` when set, else the native pixels.
    enum class State : std::uint8_t { None, Pending, Ready };
    State state = State::None;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> rgba8;
};

struct Oot3dNativeUiCustomTextureSource {
    std::function<Oot3dNativeUiCustomTexture(
        const Oot3dNativeUiCustomTextureRequest&)> resolve;
    // Changes when replacements may differ from earlier ones.
    std::function<std::uint64_t()> revision;
};

// Bytes of a PICA texture's base level, or 0 for an unknown format.
std::size_t Oot3dNativePicaBaseLevelBytes(std::uint8_t nativePicaFormat,
                                          std::uint16_t width,
                                          std::uint16_t height) noexcept;

// TopScreen's HD UI atlas (--topscreen-ui-atlas-hd): the pack's native
// oot3d/topscreen/3.0/ui_atlas redrawn at a whole multiple of its size.
// An HD image of one of the game's textures (--topscreen-hd-textures) has
// the same shape without `native_payload`: it is chosen by Azahar's hash.
struct Oot3dUiHdAtlas {
    std::shared_ptr<const std::vector<std::uint8_t>> rgba8;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // The encoded native atlas it replaces (the pack's profile payload).
    std::span<const std::uint8_t> native_payload;
};

// Whether `width` x `height` is the native size times a whole number >= 1
// on both axes (the same aspect).
bool IsUiHdAtlasSize(std::uint32_t width, std::uint32_t height,
                     std::uint32_t nativeWidth, std::uint32_t nativeHeight) noexcept;

// The HUD's custom texture for a request: the player's texture pack image
// when it has one; otherwise the HD atlas when the request is the exact
// native atlas, or `hdImage`, the mod's HD image the caller matched to the
// request. While the pack is still deciding (indexing or decoding), that
// image (or the native pixels) stands in. `hdAtlas` and `hdImage` may be null.
Oot3dNativeUiCustomTexture ChooseUiCustomTexture(
    const Oot3d::Renderer::AzaharTextureResolveResult& pack,
    const Oot3dNativeUiCustomTextureRequest& request,
    const Oot3dUiHdAtlas* hdAtlas, const Oot3dUiHdAtlas* hdImage = nullptr);

// Reads the CTXB payload selected by the original OoT3D resource loader from
// guest/PICA memory, then decodes its native tiled representation for upload.
class Oot3dNativeA32UiTextureProvider final
    : public oot3d::ui::UiTextureProvider {
  public:
    explicit Oot3dNativeA32UiTextureProvider(
        const Oot3dPicaPhysicalMemoryView& memory,
        const TopScreenTextureOverridePack* textureOverrides = nullptr) noexcept;

    bool Resolve(const oot3d::ui::UiTextureIdentity& identity,
                 oot3d::ui::UiTexturePixels& pixels,
                 std::string* error = nullptr) const override;
    std::uint64_t Revision() const noexcept override;
    // Identifies the texture as Resolve does and offers it to the custom
    // texture source (a texture pack queues its decode), without pixels.
    // Nothing without a custom texture source.
    void RequestAhead(const oot3d::ui::UiTextureIdentity& identity) const override;

    const Oot3dNativeUiTextureProviderStats& Stats() const noexcept;
    void SetGeneratedTextureTransform(Oot3dNativePicaSubmissionQueue::TextureSnapshotTransform transform) {
        mGeneratedTextureTransform = std::move(transform);
    }
    // Native and TopScreen profile textures (not generated text) are offered
    // to this source; a ready replacement is returned in their place, of any
    // size (UI coordinates are normalized).
    void SetCustomTextureSource(Oot3dNativeUiCustomTextureSource source) {
        mCustomTextures = std::move(source);
    }

  private:
    bool ResolvePixels(const oot3d::ui::UiTextureIdentity& identity,
                       oot3d::ui::UiTexturePixels& pixels, std::string* error,
                       bool ahead) const;
    // `ahead`: only to start the replacement's preparation, `pixels` not
    // decoded. `overridden`: `encoded` is the game's own texture, which the
    // TopScreen override replaced in `pixels`.
    void ApplyCustomTexture(std::uint8_t nativePicaFormat,
                            std::span<const std::uint8_t> encoded,
                            oot3d::ui::UiTexturePixels& pixels, bool ahead,
                            bool overridden) const;

    const Oot3dPicaPhysicalMemoryView& mMemory;
    const TopScreenTextureOverridePack* mTextureOverrides = nullptr;
    mutable Oot3dNativeUiTextureProviderStats mStats;
    Oot3dNativePicaSubmissionQueue::TextureSnapshotTransform mGeneratedTextureTransform;
    Oot3dNativeUiCustomTextureSource mCustomTextures;
};

} // namespace Oot3dNativeGame
