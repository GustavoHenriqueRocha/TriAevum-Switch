#pragma once

#include "fast/renderer3ds/pica_pbr_material.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Oot3d::Renderer {

inline constexpr uint64_t kOot3dEuropeanTitleId = 0x0004000000033600ULL;

struct AzaharTexturePackConfiguration {
    bool DumpTextures = false;
    bool LoadCustomTextures = false;
    uint64_t TitleId = kOot3dEuropeanTitleId;
    std::filesystem::path UserDirectory;
    std::filesystem::path LoadDirectory;
    std::filesystem::path DumpDirectory;
    // Pack images larger than native x MaxReplacementScale on an axis are
    // reduced to it on the pack worker (azahar_texture_scale.h); 0 = no cap.
    uint32_t MaxReplacementScale = 0;
    // The pack worker also makes the image's mip chain (Mips below).
    bool GenerateMipmaps = false;
    // HUD images to decode as soon as the index is ready, and where HUD images
    // decoded on first sight are added (azahar_hud_image_list.h). Empty = none.
    std::filesystem::path HudImageList;
    // World images remembered per scene (azahar_scene_image_list.h): decoded
    // when the game enters the scene (NoteScene); the images a scene asks for
    // are appended. Empty = no scene lists, no preload.
    std::filesystem::path SceneImageList;

    bool operator==(const AzaharTexturePackConfiguration&) const = default;
};

struct AzaharTextureRequest {
    uint16_t Width = 0;
    uint16_t Height = 0;
    uint8_t NativeFormat = 0;
    uint32_t MipLevel = 0;
    std::span<const uint8_t> NativeBytes;
    std::span<const uint8_t> NativeRgba8;
    // A HUD texture (the native UI renderer's): reduced to kUiTextureMaxScale
    // x native (azahar_texture_scale.h) whatever MaxReplacementScale says,
    // without a mip chain, and cached apart from a world texture of the same
    // hash. The UI renderer keeps its own copy; ReleaseDecoded does not apply.
    bool UserInterface = false;
    // A HUD texture asked for before its first draw (the UI renderer's
    // RequestAhead): only NativeBytes is needed; a copy is hashed on the HUD
    // worker, whose image is then queued as one decoded ahead (not a first
    // sight). Answers Pending without a hash. Only with UserInterface.
    bool Ahead = false;
    // With UserInterface: a texture of the game's own menus that the PICA
    // renderer draws (pause pages, text boxes; deko3d's `Ui` composition
    // domain, config/texture_pack_menu). Cached, queued and listed as a HUD
    // image (kept for the session, decoded ahead at the next start); its first
    // sights are counted apart (AzaharHudPackStatus::MenuOnSight*).
    bool Menu = false;
};

struct AzaharTextureReplacement {
    uint32_t Width = 0;
    uint32_t Height = 0;
    uint64_t NativeHash = 0;
    uint64_t PixelContentHash = 0;
    uint64_t Generation = 0;
    std::shared_ptr<const std::vector<uint8_t>> Rgba8;
    // With GenerateMipmaps: levels 1.. of Rgba8, each max(1, previous / 2).
    std::vector<std::vector<uint8_t>> Mips;
    std::filesystem::path SourcePath;
};

enum class AzaharTextureResolveState : uint8_t {
    Disabled = 0,
    Missing,
    Pending,
    Ready,
    Failed,
};

struct AzaharTextureResolveResult {
    AzaharTextureResolveState State = AzaharTextureResolveState::Disabled;
    uint64_t NativeHash = 0;
    std::shared_ptr<const AzaharTextureReplacement> Replacement;
    bool NativeHashAvailable = false;
    bool UseNewHash = true;
};

struct AzaharMaterialIdentity {
    uint64_t TitleId = 0;
    uint64_t NativeHash = 0;
    bool UseNewHash = true;

    bool operator==(const AzaharMaterialIdentity&) const = default;
};

struct AzaharPbrMaterial {
    AzaharMaterialIdentity Identity;
    uint64_t Generation = 0;
    Fast::Renderer3ds::PicaPbrMaterialParameters Parameters;
    std::array<std::shared_ptr<const AzaharTextureReplacement>, Fast::Renderer3ds::kPicaPbrTextureCount> Textures;
    std::shared_ptr<const AzaharTextureReplacement> ColorReplacement;
};
struct IndexedAzaharPbrMaterial {
    Fast::Renderer3ds::PicaPbrMaterialParameters Parameters;
    std::array<std::filesystem::path, Fast::Renderer3ds::kPicaPbrTextureCount> Textures;
    std::filesystem::path ConfigurationPath;
    std::filesystem::path BaseColorTexture;
    std::string Error;
};

struct AzaharMaterialCatalogEntry {
    uint64_t NativeHash = 0;
    IndexedAzaharPbrMaterial Definition;
};

struct AzaharImageLoadResult {
    std::shared_ptr<const AzaharTextureReplacement> Image;
    std::string Error;
};

struct AzaharMaterialParameterPreview {
    uint64_t NativeHash = 0;
    Fast::Renderer3ds::PicaPbrMaterialParameters Parameters;
    bool operator==(const AzaharMaterialParameterPreview&) const = default;
};

struct AzaharMaterialResolveResult {
    AzaharTextureResolveState State = AzaharTextureResolveState::Disabled;
    std::shared_ptr<const AzaharPbrMaterial> Material;
    std::optional<Fast::Renderer3ds::PicaPbrMaterialParameters> Preview;
};

struct AzaharTexturePackStatus {
    bool DumpTextures = false;
    bool LoadCustomTextures = false;
    bool PackConfigurationFound = false;
    bool PackUsesNewHash = true;
    uint64_t Generation = 1;
    size_t IndexedTextures = 0;
    size_t LoadedTextures = 0;
    size_t DumpedTextures = 0;
    size_t PendingDumps = 0;
    size_t PendingLoads = 0;
    size_t FailedLoads = 0;
    size_t UnsupportedTextureFiles = 0;
    bool IndexPending = false;
    size_t IndexedMaterials = 0;
    size_t LoadedMaterials = 0;
    size_t FailedMaterials = 0;
    std::filesystem::path LoadDirectory;
    std::filesystem::path DumpDirectory;
    std::string LastError;
};

// HUD images (AzaharTextureRequest::UserInterface) since the index was last
// built: those decoded ahead of their first draw, from the configuration's
// HudImageList or asked for Ahead by the UI (the start-up cost: time from the
// index to the last of them, and their RGBA8 size), and those the HUD asked
// for before they were decoded (each one drawn late: the longest wait from
// that request to its image).
struct AzaharHudPackStatus {
    size_t Listed = 0;
    size_t PreloadQueued = 0;
    size_t PreloadDecoded = 0;
    uint64_t PreloadMicros = 0;
    uint64_t PreloadBytes = 0;
    size_t OnSightDecodes = 0;
    uint64_t OnSightWaitMaxMicros = 0;
    // The same for the PICA menu textures (AzaharTextureRequest::Menu).
    size_t MenuOnSightDecodes = 0;
    uint64_t MenuOnSightWaitMaxMicros = 0;
};

// The world images of one visit to a scene (NoteScene to the next change):
// those of its list decoded ahead of its first draw, how many a draw then used
// (or found already on the GPU), the ones never used (dropped at the change)
// or left out over the RAM budget, and the images its draws asked for before
// they were decoded.
struct AzaharScenePackStatus {
    int Scene = -1;
    uint64_t Serial = 0;
    size_t Listed = 0;            // the scene's list lines the pack has
    size_t PreloadQueued = 0;
    size_t PreloadDecoded = 0;
    size_t PreloadUsed = 0;       // decoded ahead, then asked for by a draw
    size_t PreloadResident = 0;   // decoded ahead, but the draw's image was still on the GPU
    size_t PreloadUnused = 0;     // decoded ahead and never asked for (dropped at the change)
    size_t PreloadOverBudget = 0; // not decoded: the scene's unused images were over the RAM budget
    uint64_t PreloadMicros = 0;   // from the change to the last image decoded ahead
    uint64_t PreloadBytes = 0;    // RGBA8 and mips decoded ahead
    size_t OnSight = 0;           // asked for by a draw before decoded (the native drawn first)
    size_t Added = 0;             // new list lines
    size_t Dropped = 0;           // decodes of the scene before still queued at the change
};

// RAM the images decoded ahead for a scene may hold before a draw uses them
// (released after the upload, as every world image): 3x cap with mips, a
// dungeon's whole list is ~65 MiB.
inline constexpr uint64_t kScenePreloadBudgetBytes = 128ULL << 20;
// World decode workers (service threads, cores 1|2): a scene's first images
// decode two at a time.
inline constexpr size_t kWorldDecodeWorkers = 2U;

// Azahar's modern custom-texture identity is CityHash64 over the untouched
// native PICA upload bytes.
[[nodiscard]] uint64_t AzaharCityHash64(std::span<const uint8_t> bytes) noexcept;

// Computes either Azahar's modern native-byte hash or its legacy detiled hash.
[[nodiscard]] uint64_t ComputeAzaharTextureHash(const AzaharTextureRequest& request, bool useNewHash,
                                                std::string* error = nullptr);

[[nodiscard]] std::string MakeAzaharTextureFilename(uint16_t width, uint16_t height, uint64_t hash,
                                                    uint8_t nativeFormat, uint32_t mipLevel);

// Uses the same lossless PNG encoder as native texture dumps.
void WriteAzaharTexturePng(const std::filesystem::path& path, uint32_t width,
                          uint32_t height, std::span<const uint8_t> rgba8);

class AzaharTexturePackRuntime final {
  public:
    AzaharTexturePackRuntime();
    ~AzaharTexturePackRuntime();

    AzaharTexturePackRuntime(const AzaharTexturePackRuntime&) = delete;
    AzaharTexturePackRuntime& operator=(const AzaharTexturePackRuntime&) = delete;

    static AzaharTexturePackRuntime& Instance();

    // Indexing and decoding run on the pack worker. Reload retains valid
    // resources until replacements are ready; generation changes are atomic.
    void Configure(AzaharTexturePackConfiguration configuration);
    void Reload();

    [[nodiscard]] std::shared_ptr<const AzaharTextureReplacement>
    ResolveAndMaybeDump(const AzaharTextureRequest& request);
    // The renderer uses the non-blocking path: native OOT3D pixels remain
    // valid while a matching replacement is decoded by the pack worker.
    [[nodiscard]] AzaharTextureResolveResult
    ResolveOrQueue(const AzaharTextureRequest& request);
    [[nodiscard]] AzaharTextureResolveResult
    PollQueued(uint64_t nativeHash) const;
    // PollQueued that queues the decode again when the image was released
    // (ReleaseDecoded) or never queued; native size as in the request.
    [[nodiscard]] AzaharTextureResolveResult
    PollOrQueue(uint64_t nativeHash, uint16_t nativeWidth, uint16_t nativeHeight, bool userInterface = false,
                bool menu = false);
    // Drops the decoded image of `nativeHash` from the cache once a renderer
    // holds it on the GPU; a later request decodes it again.
    void ReleaseDecoded(uint64_t nativeHash, uint64_t generation);
    [[nodiscard]] AzaharMaterialResolveResult
    ResolveMaterialOrQueue(uint64_t nativeHash, uint64_t expectedGeneration);
    void ReportMaterialUploadFailure(uint64_t nativeHash, uint64_t generation, std::string error);
    [[nodiscard]] AzaharTexturePackStatus Snapshot() const;
    [[nodiscard]] AzaharHudPackStatus HudStatus() const;
    [[nodiscard]] uint64_t Generation() const;
    // The index of the configured pack is built (Configure starts a new one on
    // the pack worker). Until then a request answers Pending without a hash
    // (AzaharTextureResolveResult::NativeHashAvailable false): the caller has to
    // ask again once it is ready, nothing remembers it.
    [[nodiscard]] bool IndexReady() const;
    // The game's scene (render thread, in frame order, at each change; -1
    // outside gameplay). The queued world decodes of the scene before are
    // dropped (a draw asks again for what it still needs), its unused images
    // decoded ahead are released, and the new scene's listed images are queued.
    void NoteScene(int scene);
    // Grows at each NoteScene change.
    [[nodiscard]] uint64_t SceneSerial() const;
    // The current visit, and the visit the last change ended.
    [[nodiscard]] AzaharScenePackStatus SceneStatus() const;
    [[nodiscard]] AzaharScenePackStatus PreviousSceneStatus() const;
    // Decodes queued or running (Snapshot().PendingLoads without the copy):
    // the pacing governor's entry work (config/pacer_entry_recover).
    [[nodiscard]] size_t PendingLoads() const;
    // A world texture of the current scene drawn with its pack image still on
    // the GPU: listed for the scene, and its copy decoded ahead released.
    void NoteSceneImage(uint64_t nativeHash, uint16_t nativeWidth, uint16_t nativeHeight);
    // The renderer's hold (config/texture_pack_hold): moves these world
    // images to the front of the queue and blocks until none is decoding, at
    // most `timeout`. Returns how many are still decoding.
    size_t WaitForImages(std::span<const uint64_t> nativeHashes, std::chrono::microseconds timeout);
    [[nodiscard]] std::vector<AzaharMaterialCatalogEntry> MaterialCatalog() const;
    // Image imports share the existing pack worker; no file decoding in UI/frame code.
    [[nodiscard]] std::future<AzaharImageLoadResult> LoadEditorImage(std::filesystem::path path);
    void SetMaterialPreview(std::optional<AzaharMaterialParameterPreview> preview);
    [[nodiscard]] uint64_t MaterialRevision() const;
    // Diagnostics only for the selected live material preview; no saved state.
    void ReportMaterialSeams(uint64_t nativeHash, uint64_t generation, std::string_view status);
    [[nodiscard]] std::string MaterialSeamStatus(uint64_t nativeHash) const;

    // Intended for deterministic tests and orderly shutdown, not frame code.
    void WaitForPendingDumps();
    void WaitForPendingLoads();

  private:
    class Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace Oot3d::Renderer
