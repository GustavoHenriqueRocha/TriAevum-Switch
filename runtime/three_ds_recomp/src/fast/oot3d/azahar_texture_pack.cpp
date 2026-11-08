#include "oot3d/renderer/azahar_texture_pack.h"
#include "oot3d/renderer/azahar_pack_scale.h"
#include "oot3d/renderer/azahar_hud_image_list.h"
#include "oot3d/renderer/azahar_scene_image_list.h"
#include "oot3d/renderer/azahar_texture_scale.h"
#include "fast/renderer/content_hash.h"
#include "oot3d/renderer/pica_texture_decode.h"
#include "azahar_pbr_materials.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <zlib.h>
#if defined(__SWITCH__)
#include <switch.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace Oot3d::Renderer {
namespace {


bool IsPowerOfTwo(uint32_t value) noexcept {
    return value != 0U && (value & (value - 1U)) == 0U;
}

uint32_t Morton8(uint32_t x, uint32_t y) noexcept {
    return ((x & 0x1U) << 0U) | ((y & 0x1U) << 1U) | ((x & 0x2U) << 1U) | ((y & 0x2U) << 2U) | ((x & 0x4U) << 2U) |
           ((y & 0x4U) << 3U);
}

uint32_t AlignedTextureDimension(uint32_t value) noexcept {
    return std::max<uint32_t>(8U, (value + 7U) & ~7U);
}

void FlipRows(std::span<uint8_t> pixels, uint32_t width, uint32_t height, uint32_t bytesPerPixel) {
    const size_t rowBytes = static_cast<size_t>(width) * bytesPerPixel;
    if (pixels.size() != rowBytes * height) {
        throw std::runtime_error("texture row flip received an invalid byte count");
    }
    for (uint32_t row = 0; row < height / 2U; ++row) {
        auto first = pixels.begin() + static_cast<std::ptrdiff_t>(row * rowBytes);
        auto second = pixels.begin() + static_cast<std::ptrdiff_t>((height - row - 1U) * rowBytes);
        std::swap_ranges(first, first + rowBytes, second);
    }
}

std::vector<uint8_t> MakeLegacyHashBytes(const AzaharTextureRequest& request) {
    if (request.Width == 0U || request.Height == 0U || request.NativeFormat > 13U) {
        throw std::runtime_error("legacy Azahar hash received invalid texture metadata");
    }

    if (request.NativeFormat >= 5U) {
        const size_t expected = static_cast<size_t>(request.Width) * request.Height * 4U;
        if (request.NativeRgba8.size() != expected) {
            throw std::runtime_error("legacy Azahar hash requires decoded RGBA8 bytes");
        }
        std::vector<uint8_t> decoded(request.NativeRgba8.begin(), request.NativeRgba8.end());
        // Azahar's legacy path hashes its OpenGL-oriented detiled buffer.
        FlipRows(decoded, request.Width, request.Height, 4U);
        return decoded;
    }

    constexpr std::array<uint8_t, 5> bytesPerPixel{ 4U, 3U, 2U, 2U, 2U };
    const uint32_t pixelBytes = bytesPerPixel[request.NativeFormat];
    const uint32_t alignedWidth = AlignedTextureDimension(request.Width);
    std::vector<uint8_t> output(static_cast<size_t>(request.Width) * request.Height * pixelBytes);
    for (uint32_t y = 0; y < request.Height; ++y) {
        for (uint32_t x = 0; x < request.Width; ++x) {
            const uint32_t tile = (y / 8U) * (alignedWidth / 8U) + (x / 8U);
            const size_t source = static_cast<size_t>(tile * 64U + Morton8(x % 8U, y % 8U)) * pixelBytes;
            const size_t destination = (static_cast<size_t>(request.Height - y - 1U) * request.Width + x) * pixelBytes;
            if (source + pixelBytes > request.NativeBytes.size()) {
                throw std::runtime_error("legacy Azahar hash source is truncated");
            }
            std::copy_n(request.NativeBytes.begin() + static_cast<std::ptrdiff_t>(source), pixelBytes,
                        output.begin() + static_cast<std::ptrdiff_t>(destination));
        }
    }
    return output;
}

std::string Hex16(uint64_t value) {
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << value;
    return output.str();
}

std::string Lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

std::filesystem::path ResolveDefaultUserDirectory() {
    if (const char* overridePath = std::getenv("OOT3D_AZAHAR_USER_DIRECTORY");
        overridePath != nullptr && *overridePath != '\0') {
        return std::filesystem::path(overridePath);
    }
#ifdef _WIN32
    if (const char* appData = std::getenv("APPDATA"); appData != nullptr && *appData != '\0') {
        return std::filesystem::path(appData) / "Azahar";
    }
#else
    if (const char* xdgData = std::getenv("XDG_DATA_HOME"); xdgData != nullptr && *xdgData != '\0') {
        return std::filesystem::path(xdgData) / "azahar";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path(home) / ".local/share/azahar";
    }
#endif
    std::error_code error;
    const auto current = std::filesystem::current_path(error);
    return (error ? std::filesystem::path(".") : current) / "Azahar";
}

AzaharTexturePackConfiguration NormalizeConfiguration(AzaharTexturePackConfiguration configuration) {
    if (configuration.UserDirectory.empty()) {
        configuration.UserDirectory = ResolveDefaultUserDirectory();
    }
    configuration.UserDirectory = configuration.UserDirectory.lexically_normal();
    if (const char* loadDirectory = std::getenv("OOT3D_AZAHAR_LOAD_DIRECTORY");
        loadDirectory != nullptr && *loadDirectory != '\0') {
        configuration.LoadDirectory = std::filesystem::path(loadDirectory);
    }
    if (const char* dumpDirectory = std::getenv("OOT3D_AZAHAR_DUMP_DIRECTORY");
        dumpDirectory != nullptr && *dumpDirectory != '\0') {
        configuration.DumpDirectory = std::filesystem::path(dumpDirectory);
    }
    if (!configuration.LoadDirectory.empty()) {
        configuration.LoadDirectory = configuration.LoadDirectory.lexically_normal();
    }
    if (!configuration.DumpDirectory.empty()) {
        configuration.DumpDirectory = configuration.DumpDirectory.lexically_normal();
    }
    if (const char* titleId = std::getenv("OOT3D_AZAHAR_TITLE_ID"); titleId != nullptr && *titleId != '\0') {
        try {
            size_t parsed = 0;
            const uint64_t value = std::stoull(titleId, &parsed, 16);
            if (parsed == std::strlen(titleId)) {
                configuration.TitleId = value;
            }
        } catch (const std::exception&) {}
    }
    return configuration;
}

std::vector<uint8_t> ReadBinaryFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        throw std::runtime_error("unable to open custom texture " + path.string());
    }
    const std::streamsize size = input.tellg();
    if (size <= 0 || static_cast<uint64_t>(size) > static_cast<uint64_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("custom texture has an invalid file size");
    }
    input.seekg(0);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("unable to read custom texture " + path.string());
    }
    return bytes;
}

void AppendBigEndian32(std::vector<uint8_t>& destination, uint32_t value) {
    destination.push_back(static_cast<uint8_t>(value >> 24U));
    destination.push_back(static_cast<uint8_t>(value >> 16U));
    destination.push_back(static_cast<uint8_t>(value >> 8U));
    destination.push_back(static_cast<uint8_t>(value));
}

void AppendPngChunk(std::vector<uint8_t>& png, const std::array<char, 4>& type, std::span<const uint8_t> payload) {
    if (payload.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("PNG chunk is too large");
    }
    AppendBigEndian32(png, static_cast<uint32_t>(payload.size()));
    const size_t crcOffset = png.size();
    png.insert(png.end(), type.begin(), type.end());
    png.insert(png.end(), payload.begin(), payload.end());
    uLong crc =
        crc32(0L, reinterpret_cast<const Bytef*>(png.data() + crcOffset), static_cast<uInt>(4U + payload.size()));
    AppendBigEndian32(png, static_cast<uint32_t>(crc));
}

void WritePng(const std::filesystem::path& destination, uint32_t width, uint32_t height,
              std::span<const uint8_t> rgba8) {
    const uint64_t rowBytes = static_cast<uint64_t>(width) * 4U;
    const uint64_t filteredSize = (rowBytes + 1U) * height;
    if (width == 0U || height == 0U || rowBytes * height != rgba8.size() ||
        filteredSize > std::numeric_limits<uLong>::max() || filteredSize > std::numeric_limits<size_t>::max()) {
        throw std::runtime_error("PNG encoder received invalid RGBA8 data");
    }

    std::vector<uint8_t> filtered(static_cast<size_t>(filteredSize));
    for (uint32_t row = 0; row < height; ++row) {
        const size_t destinationRow = static_cast<size_t>(row) * (rowBytes + 1U);
        filtered[destinationRow] = 0U;
        std::copy_n(rgba8.begin() + static_cast<std::ptrdiff_t>(row * rowBytes), static_cast<size_t>(rowBytes),
                    filtered.begin() + static_cast<std::ptrdiff_t>(destinationRow + 1U));
    }

    uLongf compressedSize = compressBound(static_cast<uLong>(filtered.size()));
    std::vector<uint8_t> compressed(compressedSize);
    const int result = compress2(compressed.data(), &compressedSize, filtered.data(),
                                 static_cast<uLong>(filtered.size()), Z_BEST_SPEED);
    if (result != Z_OK) {
        throw std::runtime_error("zlib failed while encoding PNG");
    }
    compressed.resize(compressedSize);

    std::vector<uint8_t> png{ 0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU };
    std::vector<uint8_t> header;
    header.reserve(13U);
    AppendBigEndian32(header, width);
    AppendBigEndian32(header, height);
    header.insert(header.end(), { 8U, 6U, 0U, 0U, 0U });
    AppendPngChunk(png, { 'I', 'H', 'D', 'R' }, header);
    AppendPngChunk(png, { 'I', 'D', 'A', 'T' }, compressed);
    AppendPngChunk(png, { 'I', 'E', 'N', 'D' }, std::span<const uint8_t>{});

    std::error_code error;
    std::filesystem::create_directories(destination.parent_path(), error);
    if (error) {
        throw std::runtime_error("unable to create texture dump directory: " + error.message());
    }
    const auto temporary = std::filesystem::path(destination.string() + ".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output ||
            !output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()))) {
            std::filesystem::remove(temporary, error);
            throw std::runtime_error("unable to write texture dump " + destination.string());
        }
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::error_code existsError;
        if (std::filesystem::exists(destination, existsError)) {
            std::filesystem::remove(temporary, error);
            return;
        }
        std::filesystem::remove(temporary, error);
        throw std::runtime_error("unable to publish texture dump: " + error.message());
    }
}

struct PackState {
    bool ConfigurationFound = false;
    bool SkipMipmaps = false;
    bool FlipPngFiles = true;
    bool UseNewHash = true;
    std::unordered_map<std::string, std::vector<uint64_t>> MappedHashes;
};

PackState ReadPackState(const std::filesystem::path& packPath, std::string* error) {
    PackState state;
    std::error_code existsError;
    if (!std::filesystem::exists(packPath, existsError) || existsError) {
        // These are Azahar's exact no-pack legacy defaults.
        state.SkipMipmaps = true;
        state.FlipPngFiles = true;
        state.UseNewHash = false;
        return state;
    }
    try {
        std::ifstream input(packPath);
        if (!input) {
            throw std::runtime_error("unable to open pack.json");
        }
        const nlohmann::json document = nlohmann::json::parse(input);
        if (!document.is_object()) {
            throw std::runtime_error("pack.json root is not an object");
        }
        state.ConfigurationFound = true;
        if (const auto options = document.find("options"); options != document.end() && options->is_object()) {
            if (const auto value = options->find("skip_mipmap"); value != options->end() && value->is_boolean()) {
                state.SkipMipmaps = value->get<bool>();
            }
            if (const auto value = options->find("flip_png_files"); value != options->end() && value->is_boolean()) {
                state.FlipPngFiles = value->get<bool>();
            }
            if (const auto value = options->find("use_new_hash"); value != options->end() && value->is_boolean()) {
                state.UseNewHash = value->get<bool>();
            }
        }
        if (const auto textures = document.find("textures"); textures != document.end() && textures->is_object()) {
            for (const auto& item : textures->items()) {
                try {
                    size_t parsed = 0;
                    const uint64_t hash = std::stoull(item.key(), &parsed, 16);
                    if (parsed != item.key().size()) {
                        continue;
                    }
                    const auto addMapping = [&](const std::string& path) {
                        const std::string filename = Lowercase(std::filesystem::path(path).filename().string());
                        if (!filename.empty()) {
                            state.MappedHashes[filename].push_back(hash);
                        }
                    };
                    if (item.value().is_string()) {
                        addMapping(item.value().get<std::string>());
                    } else if (item.value().is_array()) {
                        for (const auto& path : item.value()) {
                            if (path.is_string()) {
                                addMapping(path.get<std::string>());
                            }
                        }
                    }
                } catch (const std::exception&) {}
            }
        }
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = "unable to parse " + packPath.string() + ": " + exception.what();
        }
        state = {};
        state.SkipMipmaps = true;
        state.FlipPngFiles = true;
        state.UseNewHash = false;
    }
    return state;
}

void EnsureDumpPack(const std::filesystem::path& dumpDirectory, bool useNewHash) {
    std::error_code error;
    std::filesystem::create_directories(dumpDirectory, error);
    if (error) {
        throw std::runtime_error("unable to create Azahar dump directory: " + error.message());
    }
    const auto packPath = dumpDirectory / "pack.json";
    if (std::filesystem::exists(packPath, error)) {
        return;
    }
    if (error) {
        throw std::runtime_error("unable to inspect Azahar pack.json: " + error.message());
    }
    const nlohmann::ordered_json document{
        { "author", "citra" },
        { "version", "1.0.0" },
        { "description", "A graphics pack" },
        { "options",
          {
              { "skip_mipmap", false },
              { "flip_png_files", true },
              { "use_new_hash", useNewHash },
          } },
    };
    std::ofstream output(packPath);
    if (!output) {
        throw std::runtime_error("unable to create Azahar pack.json");
    }
    output << document.dump(4);
    if (!output) {
        throw std::runtime_error("unable to write Azahar pack.json");
    }
}

struct IndexedTexture {
    std::filesystem::path Path;
    bool FlipPngFiles = true;
};


std::unordered_map<uint64_t, IndexedTexture> IndexTexturePack(const std::filesystem::path& loadDirectory,
                                                              const PackState& pack, size_t& unsupportedFiles,
                                                              std::unordered_map<uint64_t, IndexedAzaharPbrMaterial>& materials,
                                                              std::string* error) {
    std::unordered_map<uint64_t, IndexedTexture> index;
    unsupportedFiles = 0U;
    std::error_code directoryError;
    if (!std::filesystem::exists(loadDirectory, directoryError) || directoryError) {
        if (directoryError && error != nullptr) {
            *error = "unable to inspect Azahar texture pack: " + directoryError.message();
        }
        return index;
    }
    std::vector<std::filesystem::path> files;
    std::error_code scanError;
    for (std::filesystem::recursive_directory_iterator
             iterator(loadDirectory, std::filesystem::directory_options::skip_permission_denied, scanError),
         end;
         iterator != end && !scanError; iterator.increment(scanError)) {
        if (iterator->is_regular_file()) {
            files.push_back(iterator->path());
        }
    }
    if (scanError && error != nullptr) {
        *error = "unable to scan Azahar texture pack: " + scanError.message();
    }
    std::sort(files.begin(), files.end());
    materials = IndexAzaharPbrMaterials(loadDirectory, files, pack.MappedHashes);
    for (const auto& path : files) {
        const std::string extension = Lowercase(path.extension().string());
        if (extension != ".png" && extension != ".dds" && extension != ".ktx") {
            continue;
        }
        if (IsAzaharPbrSidecar(path)) {
            continue;
        }

        std::vector<uint64_t> hashes;
        const std::string filename = Lowercase(path.filename().string());
        if (const auto mapped = pack.MappedHashes.find(filename); mapped != pack.MappedHashes.end()) {
            hashes = mapped->second;
        }
        if (const auto parsed = ParseAzaharTextureFilenameHash(path);
            parsed.has_value() && std::find(hashes.begin(), hashes.end(), *parsed) == hashes.end()) {
            hashes.push_back(*parsed);
        }
        if (hashes.empty()) {
            continue;
        }
        if (extension != ".png") {
            ++unsupportedFiles;
            continue;
        }
        for (const uint64_t hash : hashes) {
            index.try_emplace(hash, IndexedTexture{ path, pack.FlipPngFiles });
        }
    }
    return index;
}

// What the worker does to a pack image after decoding it (the job's copy of
// the configuration and the native texture's size; zero = unknown).
struct DecodeOptions {
    TextureSize Native;
    uint32_t MaxScale = 0;
    bool Mipmaps = false;
};

std::shared_ptr<const AzaharTextureReplacement> DecodeReplacement(
    const IndexedTexture& indexed, uint64_t hash, uint64_t generation, const DecodeOptions& options = {}) {
    const auto input = ReadBinaryFile(indexed.Path);
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded =
        stbi_load_from_memory(input.data(), static_cast<int>(input.size()), &width, &height, &channels, 4);
    if (decoded == nullptr || width <= 0 || height <= 0) {
        const std::string reason =
            stbi_failure_reason() != nullptr ? stbi_failure_reason() : "unknown PNG decode failure";
        if (decoded != nullptr) {
            stbi_image_free(decoded);
        }
        throw std::runtime_error("unable to decode custom texture " + indexed.Path.string() + ": " + reason);
    }
    const uint64_t byteCount = static_cast<uint64_t>(width) * height * 4U;
    if (byteCount > std::numeric_limits<size_t>::max()) {
        stbi_image_free(decoded);
        throw std::runtime_error("custom texture dimensions are too large");
    }
    auto pixels = std::make_shared<std::vector<uint8_t>>(decoded, decoded + static_cast<size_t>(byteCount));
    stbi_image_free(decoded);

    // Azahar flips PNGs into its bottom-left OpenGL staging layout. The
    // native NRI/Vulkan decoder already matches the on-disk dump layout,
    // so only packs that explicitly disable Azahar's flip need conversion.
    if (!indexed.FlipPngFiles) {
        FlipRows(*pixels, static_cast<uint32_t>(width), static_cast<uint32_t>(height), 4U);
    }

    TextureSize size{static_cast<uint32_t>(width), static_cast<uint32_t>(height)};
    if (const TextureSize capped = CappedReplacementSize(options.Native, size, options.MaxScale); capped != size) {
        pixels = std::make_shared<std::vector<uint8_t>>(DownscaleRgba8(*pixels, size, capped));
        size = capped;
    }

    auto replacement = std::make_shared<AzaharTextureReplacement>();
    replacement->Width = size.Width;
    replacement->Height = size.Height;
    replacement->NativeHash = hash;
    replacement->PixelContentHash = AzaharCityHash64(*pixels);
    replacement->Generation = generation;
    if (options.Mipmaps) {
        replacement->Mips = BuildRgba8MipChain(*pixels, size, FullMipLevelCount(size));
    }
    replacement->Rgba8 = std::move(pixels);
    replacement->SourcePath = indexed.Path;
    return replacement;
}

struct DumpJob {
    uint64_t Hash = 0;
    uint32_t Width = 0;
    uint32_t Height = 0;
    std::filesystem::path Destination;
    std::vector<uint8_t> Rgba8;
};

struct LoadJob {
    uint64_t Hash = 0;
    uint64_t Generation = 0;
    IndexedTexture Texture;
    DecodeOptions Options;
    bool UserInterface = false;
    // A HUD image decoded ahead of its first draw (from the HUD list, or
    // asked for Ahead by the UI).
    bool Preload = false;
    // An ahead HUD request not identified yet (AzaharTextureRequest::Ahead):
    // its encoded base level, PICA format and the pack's hash mode. The HUD
    // worker hashes it (decoding it for a legacy hash) and queues its image,
    // so asking ahead costs the caller a copy, not a decode.
    std::vector<uint8_t> AheadNative;
    uint8_t AheadFormat = 0;
    bool AheadUsesNewHash = true;
    // A world image of the current scene's list, decoded when the game
    // entered it (NoteScene), and the visit (scene serial) a world job was
    // queued in: the queued ones of an earlier visit are dropped at a change.
    bool ScenePreload = false;
    uint64_t SceneSerial = 0;
    std::optional<IndexedAzaharPbrMaterial> Material;
    AzaharMaterialIdentity Identity;
    std::shared_ptr<std::promise<AzaharImageLoadResult>> EditorImage;
};

// One folder of the pack, for its fingerprint (azahar_pack_scale.h): the size
// of each regular file, and its folders. On the Switch from the directory
// reads themselves (fsDirRead returns each file's size: no file is opened,
// where a stat would open each one); elsewhere, or if that fails,
// std::filesystem.
struct PackFolderListing {
    std::vector<std::pair<std::string, uint64_t>> Files;
    std::vector<std::string> Folders;
};

PackFolderListing ListPackFolder(const std::filesystem::path& folder) {
    PackFolderListing listing;
#if defined(__SWITCH__)
    FsFileSystem* device = nullptr;
    char native[FS_MAX_PATH] = {};
    FsDir directory;
    if (fsdevTranslatePath(folder.string().c_str(), &device, native) != -1 && device != nullptr &&
        R_SUCCEEDED(fsFsOpenDirectory(device, native, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &directory))) {
        std::vector<FsDirectoryEntry> entries(64U);
        s64 count = 0;
        while (R_SUCCEEDED(fsDirRead(&directory, &count, entries.size(), entries.data())) && count > 0) {
            for (s64 index = 0; index < count; ++index) {
                const auto& entry = entries[static_cast<size_t>(index)];
                if (entry.type == FsDirEntryType_Dir) {
                    listing.Folders.emplace_back(entry.name);
                } else if (entry.type == FsDirEntryType_File) {
                    listing.Files.emplace_back(entry.name, static_cast<uint64_t>(std::max<s64>(0, entry.file_size)));
                }
            }
        }
        fsDirClose(&directory);
        return listing;
    }
#endif
    std::error_code error;
    for (std::filesystem::directory_iterator
             iterator(folder, std::filesystem::directory_options::skip_permission_denied, error),
         end;
         iterator != end && !error; iterator.increment(error)) {
        std::error_code entryError;
        if (iterator->is_directory(entryError)) {
            listing.Folders.push_back(iterator->path().filename().string());
        } else if (iterator->is_regular_file(entryError)) {
            const uint64_t size = iterator->file_size(entryError);
            if (!entryError) listing.Files.emplace_back(iterator->path().filename().string(), size);
        }
    }
    return listing;
}

// The pack's image scale (azahar_pack_scale.h) is measured a few PNG headers
// at a time, so that a decode queued meanwhile waits for at most these; while
// decodes are still running, the pack worker looks again this often.
constexpr size_t kImageScaleHeadersPerStep = 16U;
constexpr std::chrono::milliseconds kImageScaleRecheck{100};

} // namespace

uint64_t AzaharCityHash64(std::span<const uint8_t> bytes) noexcept {
    return ::Fast::Renderer::ContentHash64(bytes);
}

uint64_t ComputeAzaharTextureHash(const AzaharTextureRequest& request, bool useNewHash, std::string* error) {
    if (error != nullptr) {
        error->clear();
    }
    try {
        if (request.NativeBytes.empty()) {
            throw std::runtime_error("Azahar texture hash requires native PICA bytes");
        }
        if (useNewHash) {
            return AzaharCityHash64(request.NativeBytes);
        }
        const auto legacy = MakeLegacyHashBytes(request);
        return AzaharCityHash64(legacy);
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = exception.what();
        }
        return 0U;
    }
}

std::string MakeAzaharTextureFilename(uint16_t width, uint16_t height, uint64_t hash, uint8_t nativeFormat,
                                      uint32_t mipLevel) {
    return "tex1_" + std::to_string(width) + "x" + std::to_string(height) + "_" + Hex16(hash) + "_" +
           std::to_string(nativeFormat) + "_mip" + std::to_string(mipLevel) + ".png";
}

class AzaharTexturePackRuntime::Impl {
  public:
    // HUD images have their own worker: a HUD texture seen for the first time
    // never waits behind the world images of a scene being loaded (1080p
    // packs: tens of PNGs, some of them 4096 wide), only behind other HUD
    // images, which are small. World images decode on the pack worker and
    // kWorldDecodeWorkers - 1 more threads that only decode.
    Impl() {
        mWorker = std::thread([this] { WorkerMain(); });
        mUiWorker = std::thread([this] { UiWorkerMain(); });
        for (size_t index = 1U; index < kWorldDecodeWorkers; ++index)
            mWorldWorkers.emplace_back([this] { WorldWorkerMain(); });
    }
    ~Impl() {
        {
            std::scoped_lock lock(mMutex);
            mStopping = true;
        }
        mWorkAvailable.notify_all();
        mUiWorkAvailable.notify_all();
        mWorker.join();
        mUiWorker.join();
        for (auto& worker : mWorldWorkers) worker.join();
    }

    void Configure(AzaharTexturePackConfiguration configuration) {
        configuration = NormalizeConfiguration(std::move(configuration));
        std::scoped_lock lock(mMutex);
        if (mConfigured && configuration == mConfiguration) return;
        mConfiguration = std::move(configuration);
        mConfigured = true;
        ++mGeneration;
        ++mMaterialRevision;
        mPreview.reset();
        mPreviewSeamStatus.clear();
        mIndexReady = false;
        mIndex.clear();
        mMaterialIndex.clear();
        mReplacementCache.clear();
        mUiReplacementCache.clear();
        mMaterialCache.clear();
        mFailedLoadHashes.clear();
        mFailedUiLoadHashes.clear();
        mHudWaiting.clear();
        mMenuWaiting.clear();
        mFailedMaterialHashes.clear();
        mDumpedHashes.clear();
        ResetScenePreloadLocked();
        mSceneListAppend.clear();
        const auto title = Hex16(mConfiguration.TitleId);
        const auto previousLoadDirectory = std::move(mLoadDirectory);
        mLoadDirectory = mConfiguration.LoadDirectory.empty()
            ? mConfiguration.UserDirectory / "load" / "textures" / title : mConfiguration.LoadDirectory;
        mDumpDirectory = mConfiguration.DumpDirectory.empty()
            ? mConfiguration.UserDirectory / "dump" / "textures" / title : mConfiguration.DumpDirectory;
        // A size cap change indexes the same folder again: the Video tab keeps
        // the pack's sizes meanwhile. Another folder, or HD textures off,
        // forgets them.
        if (!mConfiguration.LoadCustomTextures || mLoadDirectory != previousLoadDirectory) ForgetImageScaleLocked();
        RequestRebuildLocked();
    }

    void Reload() {
        std::scoped_lock lock(mMutex);
        if (mConfigured) RequestRebuildLocked();
    }

    std::shared_ptr<const AzaharTextureReplacement> ResolveAndMaybeDump(const AzaharTextureRequest& request) {
        WaitForPendingLoads();
        const auto result = ResolveOrQueue(request);
        WaitForPendingLoads();
        return PollQueued(result.NativeHash).Replacement;
    }

    AzaharTextureResolveResult ResolveOrQueue(const AzaharTextureRequest& request) {
        bool dumpEnabled, loadEnabled, dumpUsesNewHash, loadUsesNewHash;
        uint64_t generation;
        std::filesystem::path dumpDirectory;
        {
            std::scoped_lock lock(mMutex);
            if (!mConfigured) return {};
            if (!mIndexReady) {
                // HD textures off (and no dump): nothing to wait for, the
                // caller draws its own pixels at once; the hash (for
                // authoring) is there once the index is.
                if (!mConfiguration.LoadCustomTextures && !mConfiguration.DumpTextures) return {};
                return {AzaharTextureResolveState::Pending, 0U, nullptr};
            }
            if (request.UserInterface && request.Ahead) return QueueAheadLocked(request);
            dumpEnabled = mConfiguration.DumpTextures;
            loadEnabled = mConfiguration.LoadCustomTextures;
            dumpUsesNewHash = mDumpUsesNewHash;
            loadUsesNewHash = mPack.UseNewHash;
            generation = mGeneration;
            if (dumpEnabled) dumpDirectory = mDumpDirectory;
        }
        std::string error;
        std::optional<uint64_t> modern, legacy;
        const auto hashFor = [&](bool modernMode) {
            auto& value = modernMode ? modern : legacy;
            if (!value) value = ComputeAzaharTextureHash(request, modernMode, &error);
            return *value;
        };
        if (dumpEnabled) {
            const auto hash = hashFor(dumpUsesNewHash);
            if (error.empty()) QueueDump(request, hash, generation, dumpDirectory);
        }
        if (!error.empty()) {
            SetError(std::move(error));
            return {AzaharTextureResolveState::Failed, 0U, nullptr};
        }
        const auto hash = hashFor(loadUsesNewHash);
        if (!error.empty()) {
            SetError(std::move(error));
            return {AzaharTextureResolveState::Failed, 0U, nullptr};
        }
        if (!loadEnabled) return {AzaharTextureResolveState::Disabled, hash, nullptr, true, loadUsesNewHash};
        std::scoped_lock lock(mMutex);
        if (generation != mGeneration || !mConfiguration.LoadCustomTextures) return {};
        return QueueOrPollLocked(hash, {request.Width, request.Height}, request.UserInterface, false,
                                 request.UserInterface && request.Menu);
    }

    AzaharTextureResolveResult PollQueued(uint64_t hash) const {
        std::scoped_lock lock(mMutex);
        return PollLocked(hash);
    }

    AzaharTextureResolveResult PollOrQueue(uint64_t hash, TextureSize native, bool userInterface, bool menu) {
        std::scoped_lock lock(mMutex);
        if (!mConfigured || !mConfiguration.LoadCustomTextures) return {};
        if (!mIndexReady) return {AzaharTextureResolveState::Pending, hash, nullptr, false, mPack.UseNewHash};
        return QueueOrPollLocked(hash, native, userInterface, false, userInterface && menu);
    }

    void ReleaseDecoded(uint64_t hash, uint64_t generation) {
        std::scoped_lock lock(mMutex);
        if (const auto cached = mReplacementCache.find(hash);
            cached != mReplacementCache.end() && cached->second->Generation == generation) {
            mReplacementCache.erase(cached);
            ForgetScenePreloadLocked(hash);
        }
    }

    AzaharMaterialResolveResult ResolveMaterialOrQueue(uint64_t hash, uint64_t expectedGeneration) {
        std::scoped_lock lock(mMutex);
        if (!mConfiguration.LoadCustomTextures || expectedGeneration != mGeneration) return {};
        if (!mIndexReady) return {AzaharTextureResolveState::Pending, nullptr};
        const auto indexed = mMaterialIndex.find(hash);
        if (indexed == mMaterialIndex.end()) return {AzaharTextureResolveState::Missing, nullptr};
        const auto cached = mMaterialCache.find(hash);
        if (!indexed->second.Error.empty()) {
            mFailedMaterialHashes.insert(hash);
        } else if ((cached == mMaterialCache.end() || cached->second->Generation != mGeneration) &&
                   !mFailedMaterialHashes.contains(hash) && !mPendingMaterialGenerations.contains(hash)) {
            LoadJob job;
            job.Hash = hash;
            job.Generation = mGeneration;
            job.Texture.FlipPngFiles = mPack.FlipPngFiles;
            job.Material = indexed->second;
            job.Identity = {mConfiguration.TitleId, hash, mPack.UseNewHash};
            if (const auto color = mIndex.find(hash); color != mIndex.end())
                job.Texture = color->second;
            if (!indexed->second.BaseColorTexture.empty())
                job.Texture.Path = indexed->second.BaseColorTexture;
            mPendingMaterialGenerations.emplace(hash, mGeneration);
            mLoadJobs.push_back(std::move(job));
            ++mPendingLoads;
            mWorkAvailable.notify_all();
        }
        if (cached != mMaterialCache.end())
            return {AzaharTextureResolveState::Ready, cached->second,
                    mPreview && mPreview->NativeHash == hash
                        ? std::optional(mPreview->Parameters) : std::nullopt};
        return {mFailedMaterialHashes.contains(hash) ? AzaharTextureResolveState::Failed : AzaharTextureResolveState::Pending, nullptr};
    }

    void ReportMaterialUploadFailure(uint64_t hash, uint64_t generation, std::string error) {
        std::scoped_lock lock(mMutex);
        if (generation != mGeneration) return;
        mFailedMaterialHashes.insert(hash);
        mLastError = std::move(error);
    }

    AzaharTexturePackStatus Snapshot() const {
        std::scoped_lock lock(mMutex);
        AzaharTexturePackStatus status;
        status.DumpTextures = mConfiguration.DumpTextures;
        status.LoadCustomTextures = mConfiguration.LoadCustomTextures;
        status.PackConfigurationFound = mPack.ConfigurationFound;
        status.PackUsesNewHash = mPack.UseNewHash;
        status.Generation = mGeneration;
        status.IndexedTextures = mIndex.size();
        status.LoadedTextures = mReplacementCache.size() + mUiReplacementCache.size();
        status.DumpedTextures = mDumpedCount;
        status.PendingDumps = mPendingDumps;
        status.PendingLoads = mPendingLoads;
        status.FailedLoads = mFailedLoadHashes.size() + mFailedUiLoadHashes.size();
        status.UnsupportedTextureFiles = mUnsupportedFiles;
        status.IndexPending = mRebuildRequested || mBuilding;
        status.IndexedMaterials = mMaterialIndex.size();
        status.LoadedMaterials = mMaterialCache.size();
        status.FailedMaterials = mFailedMaterialHashes.size();
        status.LoadDirectory = mLoadDirectory;
        status.DumpDirectory = mDumpDirectory;
        status.LastError = mLastError;
        return status;
    }
    uint64_t Generation() const {
        std::scoped_lock lock(mMutex);
        return mGeneration;
    }
    bool IndexReady() const {
        std::scoped_lock lock(mMutex);
        return mConfigured && mIndexReady;
    }
    uint32_t LargestImageScale() const { return mLargestImageScale.load(std::memory_order_relaxed); }
    uint64_t ImageScaleSerial() const { return mImageScaleSerial.load(std::memory_order_acquire); }
    AzaharImageScaleStatus ImageScaleStatus() const {
        std::scoped_lock lock(mMutex);
        return mImageScaleStatus;
    }
    AzaharHudPackStatus HudStatus() const {
        std::scoped_lock lock(mMutex);
        return mHud;
    }
    std::vector<AzaharMaterialCatalogEntry> MaterialCatalog() const {
        std::scoped_lock lock(mMutex);
        std::vector<AzaharMaterialCatalogEntry> result;
        result.reserve(mMaterialIndex.size());
        for (const auto& [hash, definition] : mMaterialIndex)
            result.push_back({hash, definition});
        std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.NativeHash < b.NativeHash; });
        return result;
    }
    std::future<AzaharImageLoadResult> LoadEditorImage(std::filesystem::path path) {
        LoadJob job;
        job.Texture.Path = std::move(path);
        job.EditorImage = std::make_shared<std::promise<AzaharImageLoadResult>>();
        auto result = job.EditorImage->get_future();
        std::scoped_lock lock(mMutex);
        mLoadJobs.push_back(std::move(job));
        ++mPendingLoads;
        mWorkAvailable.notify_all();
        return result;
    }
    void SetMaterialPreview(std::optional<AzaharMaterialParameterPreview> preview) {
        std::scoped_lock lock(mMutex);
        if (mPreview == preview) return;
        mPreview = std::move(preview);
        mPreviewSeamStatus.clear();
        ++mMaterialRevision;
    }
    uint64_t MaterialRevision() const {
        std::scoped_lock lock(mMutex);
        return mMaterialRevision;
    }
    void ReportMaterialSeams(uint64_t hash, uint64_t generation, std::string_view status) {
        std::scoped_lock lock(mMutex);
        if (generation == mGeneration && mPreview && mPreview->NativeHash == hash &&
            mPreviewSeamStatus != status)
            mPreviewSeamStatus = status;
    }
    std::string MaterialSeamStatus(uint64_t hash) const {
        std::scoped_lock lock(mMutex);
        return mPreview && mPreview->NativeHash == hash && !mPreviewSeamStatus.empty()
            ? mPreviewSeamStatus : "Awaiting a native draw with this material";
    }
    void WaitForPendingDumps() {
        std::unique_lock lock(mMutex);
        mIdle.wait(lock, [&] { return !mBuilding && !mRebuildRequested && mPendingDumps == 0U; });
    }
    void WaitForPendingLoads() {
        std::unique_lock lock(mMutex);
        mIdle.wait(lock, [&] {
            return !mBuilding && !mRebuildRequested && mPendingLoads == 0U && mSceneListAppend.empty() &&
                   !mSceneListWriting;
        });
    }

    void NoteScene(int scene) {
        std::scoped_lock lock(mMutex);
        if (scene == mSceneId) return;
        // The visit ends: its images decoded ahead that no draw asked for go,
        // and so do its world decodes still queued (a draw of the new scene
        // asks again for what it needs, from the front of the queue).
        for (const auto& [hash, preload] : mScenePreloaded) {
            if (preload.Used) continue;
            ++mScene.PreloadUnused;
            if (const auto cached = mReplacementCache.find(hash);
                cached != mReplacementCache.end() && cached->second->Generation == mGeneration)
                mReplacementCache.erase(cached);
        }
        mScenePreloaded.clear();
        mScenePreloadBytes = 0U;
        ++mSceneSerial;
        mScene.Dropped += DropEarlierSceneJobsLocked();
        mPreviousScene = mScene;
        mSceneId = scene;
        mScene = {};
        mScene.Scene = scene;
        mScene.Serial = mSceneSerial;
        mSceneStart = std::chrono::steady_clock::now();
        mSceneOnSight.clear();
        mSceneSerialPublished.store(mSceneSerial, std::memory_order_release);
        QueueScenePreloadLocked();
        mIdle.notify_all();
    }
    uint64_t SceneSerial() const { return mSceneSerialPublished.load(std::memory_order_acquire); }
    AzaharScenePackStatus SceneStatus() const {
        std::scoped_lock lock(mMutex);
        return mScene;
    }
    AzaharScenePackStatus PreviousSceneStatus() const {
        std::scoped_lock lock(mMutex);
        return mPreviousScene;
    }
    size_t PendingLoads() const {
        std::scoped_lock lock(mMutex);
        return mPendingLoads;
    }
    void NoteSceneImage(uint64_t hash, TextureSize native) {
        std::scoped_lock lock(mMutex);
        if (!mConfigured || !mConfiguration.LoadCustomTextures || !mIndexReady || !mIndex.contains(hash)) return;
        RecordSceneImageLocked(hash, native);
        const auto preloaded = mScenePreloaded.find(hash);
        if (preloaded == mScenePreloaded.end() || preloaded->second.Used) return;
        ++mScene.PreloadResident;
        if (const auto cached = mReplacementCache.find(hash);
            cached != mReplacementCache.end() && cached->second->Generation == mGeneration)
            mReplacementCache.erase(cached);
        ForgetScenePreloadLocked(hash);
    }
    size_t WaitForImages(std::span<const uint64_t> hashes, std::chrono::microseconds timeout) {
        std::unique_lock lock(mMutex);
        // On screen now: before every other queued world image.
        for (const uint64_t hash : hashes) {
            if (!mPendingLoadGenerations.contains(hash)) continue;
            const auto queued = std::find_if(mLoadJobs.begin(), mLoadJobs.end(), [&](const LoadJob& job) {
                return job.Hash == hash && !job.Material && !job.EditorImage;
            });
            if (queued != mLoadJobs.end() && queued != mLoadJobs.begin()) {
                LoadJob job = std::move(*queued);
                mLoadJobs.erase(queued);
                mLoadJobs.push_front(std::move(job));
            }
        }
        const auto decoding = [&] {
            return static_cast<size_t>(std::count_if(hashes.begin(), hashes.end(), [&](uint64_t hash) {
                return mPendingLoadGenerations.contains(hash);
            }));
        };
        mIdle.wait_for(lock, timeout, [&] { return mStopping || decoding() == 0U; });
        return decoding();
    }

  private:
    // An ahead HUD request (index ready): a copy of its encoded base level is
    // identified on the HUD worker (IdentifyAhead); its image is queued then.
    AzaharTextureResolveResult QueueAheadLocked(const AzaharTextureRequest& request) {
        if (!mConfiguration.LoadCustomTextures) return {};
        if (request.NativeBytes.empty()) return {AzaharTextureResolveState::Failed, 0U, nullptr};
        LoadJob job;
        job.Generation = mGeneration;
        job.Options = DecodeOptions{{request.Width, request.Height}, kUiTextureMaxScale, false};
        job.UserInterface = true;
        job.Preload = true;
        job.AheadNative.assign(request.NativeBytes.begin(), request.NativeBytes.end());
        job.AheadFormat = request.NativeFormat;
        job.AheadUsesNewHash = mPack.UseNewHash;
        mUiLoadJobs.push_back(std::move(job));
        ++mPendingLoads;
        mUiWorkAvailable.notify_one();
        return {AzaharTextureResolveState::Pending, 0U, nullptr, false, mPack.UseNewHash};
    }
    // The pack's hash of an ahead HUD request, then its image queued ahead
    // (if the pack has one) behind the HUD images already queued.
    void IdentifyAhead(const LoadJob& job) {
        AzaharTextureRequest request{static_cast<uint16_t>(job.Options.Native.Width),
                                     static_cast<uint16_t>(job.Options.Native.Height), job.AheadFormat, 0U,
                                     job.AheadNative, {}};
        std::vector<uint8_t> rgba8;
        std::string error;
        uint64_t hash = 0U;
        // Azahar's legacy hash of a format without a byte layout of its own
        // (5 and up) is over the decoded texels.
        if (job.AheadUsesNewHash || job.AheadFormat < 5U ||
            DecodePicaTextureRgba8(job.AheadFormat, request.Width, request.Height, job.AheadNative, rgba8, &error)) {
            request.NativeRgba8 = rgba8;
            hash = ComputeAzaharTextureHash(request, job.AheadUsesNewHash, &error);
        }
        std::scoped_lock lock(mMutex);
        if (!error.empty()) {
            mLastError = "HUD texture asked for ahead: " + error;
        } else if (job.Generation == mGeneration && mConfiguration.LoadCustomTextures) {
            (void)QueueOrPollLocked(hash, job.Options.Native, true, true);
        }
        --mPendingLoads;
        mIdle.notify_all();
    }
    // Queues the decode of an indexed image that is neither cached, failed
    // nor already queued, then reports its state. HUD images have their own
    // cache (another size cap and no mips than a world texture of the hash)
    // and their own queue and worker; a HUD request (not ahead) that finds
    // its image still decoding is remembered as a first sight drawn late
    // (`menu`: a PICA menu texture, counted apart). A texture on screen goes
    // before the HUD images asked for ahead: queued at the front, or moved
    // there when it was queued ahead and has not started. Likewise a world
    // texture on screen goes before the scene's images decoded ahead, and is
    // listed for the scene (RecordSceneImageLocked).
    AzaharTextureResolveResult QueueOrPollLocked(uint64_t hash, TextureSize native, bool userInterface = false,
                                                 bool preload = false, bool menu = false) {
        const auto indexed = mIndex.find(hash);
        if (indexed == mIndex.end()) return {AzaharTextureResolveState::Missing, hash, nullptr, true, mPack.UseNewHash};
        auto& cache = userInterface ? mUiReplacementCache : mReplacementCache;
        auto& pending = userInterface ? mPendingUiLoadGenerations : mPendingLoadGenerations;
        const auto& failed = userInterface ? mFailedUiLoadHashes : mFailedLoadHashes;
        const auto cached = cache.find(hash);
        const bool onSight = userInterface && !preload;
        const bool worldOnSight = !userInterface && !preload;
        if ((cached == cache.end() || cached->second->Generation != mGeneration) && !failed.contains(hash) &&
            !pending.contains(hash)) {
            pending.emplace(hash, mGeneration);
            LoadJob job;
            job.Hash = hash;
            job.Generation = mGeneration;
            job.Texture = indexed->second;
            job.Options = userInterface
                              ? DecodeOptions{native, kUiTextureMaxScale, false}
                              : DecodeOptions{native, mConfiguration.MaxReplacementScale, mConfiguration.GenerateMipmaps};
            job.UserInterface = userInterface;
            job.Preload = preload;
            job.SceneSerial = mSceneSerial;
            if (preload) ++mHud.PreloadQueued;
            if (onSight) {
                mUiLoadJobs.push_front(std::move(job));
            } else if (worldOnSight) {
                const auto ahead = std::find_if(mLoadJobs.begin(), mLoadJobs.end(),
                                                [](const LoadJob& queued) { return queued.ScenePreload; });
                mLoadJobs.insert(ahead, std::move(job));
            } else {
                (userInterface ? mUiLoadJobs : mLoadJobs).push_back(std::move(job));
            }
            ++mPendingLoads;
            if (userInterface) {
                mUiWorkAvailable.notify_one();
            } else {
                mWorkAvailable.notify_all();
            }
        } else if (onSight && pending.contains(hash)) {
            const auto queued = std::find_if(mUiLoadJobs.begin(), mUiLoadJobs.end(), [&](const LoadJob& job) {
                return job.Hash == hash && job.AheadNative.empty();
            });
            if (queued != mUiLoadJobs.end() && queued != mUiLoadJobs.begin()) {
                LoadJob job = std::move(*queued);
                mUiLoadJobs.erase(queued);
                mUiLoadJobs.push_front(std::move(job));
            }
        } else if (worldOnSight && pending.contains(hash)) {
            const auto queued = std::find_if(mLoadJobs.begin(), mLoadJobs.end(), [&](const LoadJob& job) {
                return job.Hash == hash && job.ScenePreload;
            });
            if (queued != mLoadJobs.end() && queued != mLoadJobs.begin()) {
                LoadJob job = std::move(*queued);
                mLoadJobs.erase(queued);
                mLoadJobs.push_front(std::move(job));
            }
        }
        auto result = PollLocked(hash, userInterface);
        if (onSight && result.State == AzaharTextureResolveState::Pending)
            (menu ? mMenuWaiting : mHudWaiting).try_emplace(hash, std::chrono::steady_clock::now());
        if (worldOnSight) {
            RecordSceneImageLocked(hash, native);
            if (result.State == AzaharTextureResolveState::Pending && mSceneOnSight.insert(hash).second)
                ++mScene.OnSight;
            if (const auto preloaded = mScenePreloaded.find(hash);
                result.State == AzaharTextureResolveState::Ready && preloaded != mScenePreloaded.end() &&
                !preloaded->second.Used) {
                preloaded->second.Used = true;
                ++mScene.PreloadUsed;
            }
        }
        return result;
    }
    // A world image a draw of the current scene asked for, not in the
    // scene's list yet: listed (the file is appended by the pack worker),
    // also for a later visit in this session.
    void RecordSceneImageLocked(uint64_t hash, TextureSize native) {
        if (mConfiguration.SceneImageList.empty() || mSceneId < 0 ||
            static_cast<uint32_t>(mSceneId) > kSceneImageListMaxScene || native.Width == 0U ||
            native.Height == 0U || native.Width > 1024U || native.Height > 1024U ||
            mSceneListLines >= kSceneImageListLimit)
            return;
        auto& listed = mSceneListed[static_cast<uint16_t>(mSceneId)];
        if (listed.size() >= kSceneImageListPerScene || !listed.insert(hash).second) return;
        const HudImageEntry image{hash, static_cast<uint16_t>(native.Width), static_cast<uint16_t>(native.Height)};
        mSceneLists[static_cast<uint16_t>(mSceneId)].push_back(image);
        ++mSceneListLines;
        ++mScene.Added;
        mSceneListAppend += FormatSceneImageEntry({static_cast<uint16_t>(mSceneId), image});
        mWorkAvailable.notify_all();
    }
    // The current scene's listed images on the world workers, behind any
    // image already on screen; not those decoded, failed or already queued.
    void QueueScenePreloadLocked() {
        if (!mConfigured || !mIndexReady || !mConfiguration.LoadCustomTextures || mSceneId < 0) return;
        const auto list = mSceneLists.find(static_cast<uint16_t>(mSceneId));
        if (list == mSceneLists.end()) return;
        mScene.Listed = list->second.size();
        for (const auto& entry : list->second) {
            const auto indexed = mIndex.find(entry.Hash);
            if (indexed == mIndex.end() || mFailedLoadHashes.contains(entry.Hash) ||
                mPendingLoadGenerations.contains(entry.Hash))
                continue;
            if (const auto cached = mReplacementCache.find(entry.Hash);
                cached != mReplacementCache.end() && cached->second->Generation == mGeneration)
                continue;
            mPendingLoadGenerations.emplace(entry.Hash, mGeneration);
            LoadJob job;
            job.Hash = entry.Hash;
            job.Generation = mGeneration;
            job.Texture = indexed->second;
            job.Options = DecodeOptions{{entry.Width, entry.Height}, mConfiguration.MaxReplacementScale,
                                        mConfiguration.GenerateMipmaps};
            job.ScenePreload = true;
            job.SceneSerial = mSceneSerial;
            mLoadJobs.push_back(std::move(job));
            ++mPendingLoads;
            ++mScene.PreloadQueued;
        }
        mWorkAvailable.notify_all();
    }
    // World decodes queued in an earlier visit (not started): dropped, and
    // no longer pending, so a draw that still needs one queues it again.
    size_t DropEarlierSceneJobsLocked() {
        size_t dropped = 0U;
        for (auto job = mLoadJobs.begin(); job != mLoadJobs.end();) {
            if (job->Material || job->EditorImage || job->SceneSerial == mSceneSerial) {
                ++job;
                continue;
            }
            if (const auto found = mPendingLoadGenerations.find(job->Hash);
                found != mPendingLoadGenerations.end() && found->second == job->Generation)
                mPendingLoadGenerations.erase(found);
            job = mLoadJobs.erase(job);
            --mPendingLoads;
            ++dropped;
        }
        return dropped;
    }
    // An image decoded ahead that left the cache (uploaded, or replaced).
    void ForgetScenePreloadLocked(uint64_t hash) {
        if (const auto preloaded = mScenePreloaded.find(hash); preloaded != mScenePreloaded.end()) {
            mScenePreloadBytes -= std::min(mScenePreloadBytes, preloaded->second.Bytes);
            mScenePreloaded.erase(preloaded);
        }
    }
    void ResetScenePreloadLocked() {
        mScenePreloaded.clear();
        mScenePreloadBytes = 0U;
        mSceneOnSight.clear();
    }
    AzaharTextureResolveResult PollLocked(uint64_t hash, bool userInterface = false) const {
        if (!mConfigured || !mConfiguration.LoadCustomTextures) return {};
        const auto& cache = userInterface ? mUiReplacementCache : mReplacementCache;
        const auto& pending = userInterface ? mPendingUiLoadGenerations : mPendingLoadGenerations;
        const auto& failed = userInterface ? mFailedUiLoadHashes : mFailedLoadHashes;
        if (const auto cached = cache.find(hash); cached != cache.end())
            return {AzaharTextureResolveState::Ready, hash, cached->second, true, mPack.UseNewHash};
        if (failed.contains(hash)) return {AzaharTextureResolveState::Failed, hash, nullptr, true, mPack.UseNewHash};
        if (!mIndexReady || pending.contains(hash)) return {AzaharTextureResolveState::Pending, hash, nullptr, mIndexReady, mPack.UseNewHash};
        return {AzaharTextureResolveState::Missing, hash, nullptr, true, mPack.UseNewHash};
    }
    void RequestRebuildLocked() {
        ++mRebuildSerial;
        mRebuildRequested = true;
        // The new index starts its own measure (StartImageScaleScanLocked).
        mImageScaleScan.Active = false;
        mWorkAvailable.notify_all();
    }

    // The pack's largest world/model image scale (azahar_pack_scale.h): from
    // the new index on, one step at a time on the pack worker, only while
    // ImageScaleScanRunnableLocked. Steps: plan the files to measure; list
    // the pack's folders (fingerprint); compare with the result kept for this
    // folder, then ImageScaleRecord; read the PNG headers; publish and
    // remember.
    struct ImageScaleScan {
        bool Active = false;
        uint64_t Generation = 0;
        std::filesystem::path Root, Record;
        std::vector<std::filesystem::path> Indexed; // the index's files, until planned
        bool Planned = false;
        PackScalePlan Plan;
        std::vector<std::string> Folders; // still to list, below Root ("" = Root)
        PackFingerprint Fingerprint;
        bool RecordChecked = false;
        size_t Next = 0; // Plan.Files measured
        uint32_t Largest = 0;
        uint64_t Measured = 0;
        size_t Unreadable = 0;
        uint64_t ListMicros = 0, BusyMicros = 0;
        std::chrono::steady_clock::time_point Start;
    };
    void StartImageScaleScanLocked(const AzaharTexturePackConfiguration& configuration,
                                   const std::filesystem::path& root, std::vector<std::filesystem::path> indexed) {
        mImageScaleScan = {};
        mImageScaleScan.Active = true;
        mImageScaleScan.Generation = mGeneration;
        mImageScaleScan.Root = root;
        mImageScaleScan.Record = configuration.ImageScaleRecord;
        mImageScaleScan.Indexed = std::move(indexed);
        mImageScaleScan.Folders.emplace_back();
        mImageScaleScan.Start = std::chrono::steady_clock::now();
    }
    void ForgetImageScaleLocked() {
        mImageScaleKnown.reset();
        mLargestImageScale.store(0U, std::memory_order_relaxed);
    }
    void FinishImageScaleScanLocked(const PackScaleRecord& record, bool remembered) {
        auto& scan = mImageScaleScan;
        mImageScaleKnown = record;
        AzaharImageScaleStatus status;
        status.Serial = mImageScaleStatus.Serial + 1U;
        status.Largest = record.Largest;
        status.Remembered = remembered;
        status.Measured = record.Measured;
        status.Hud = scan.Plan.Hud;
        status.Unnamed = scan.Plan.Unnamed;
        status.Unreadable = scan.Unreadable;
        status.Files = scan.Fingerprint.Files;
        status.ListMicros = scan.ListMicros;
        status.BusyMicros = scan.BusyMicros;
        status.Micros = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - scan.Start)
                .count());
        mImageScaleStatus = status;
        mLargestImageScale.store(record.Largest, std::memory_order_relaxed);
        mImageScaleSerial.store(status.Serial, std::memory_order_release);
        scan.Active = false;
    }
    // Lowest priority: nothing else for any pack worker, decodes running included.
    bool ImageScaleScanRunnableLocked() const {
        return mImageScaleScan.Active && mImageScaleScan.Generation == mGeneration && mPendingLoads == 0U &&
               mLoadJobs.empty() && mUiLoadJobs.empty() && mDumpJobs.empty() && !mRebuildRequested && !mBuilding &&
               mSceneListAppend.empty() && !mSceneListWriting;
    }
    void StepImageScaleScan() {
        enum class Step : uint8_t { Plan, List, Record, Headers, Finish };
        Step step = Step::Finish;
        uint64_t generation = 0;
        std::filesystem::path root, record;
        std::vector<std::filesystem::path> indexed;
        std::string folder;
        std::vector<PackScaleFile> batch;
        {
            std::scoped_lock lock(mMutex);
            auto& scan = mImageScaleScan;
            if (!scan.Active || scan.Generation != mGeneration) return;
            generation = scan.Generation;
            root = scan.Root;
            record = scan.Record;
            if (!scan.Planned) {
                step = Step::Plan;
                indexed = std::move(scan.Indexed);
            } else if (!scan.Folders.empty()) {
                step = Step::List;
                folder = std::move(scan.Folders.back());
                scan.Folders.pop_back();
            } else if (!scan.RecordChecked) {
                step = Step::Record;
            } else if (scan.Next < scan.Plan.Files.size()) {
                step = Step::Headers;
                const size_t end = std::min(scan.Plan.Files.size(), scan.Next + kImageScaleHeadersPerStep);
                batch.assign(scan.Plan.Files.begin() + static_cast<std::ptrdiff_t>(scan.Next),
                             scan.Plan.Files.begin() + static_cast<std::ptrdiff_t>(end));
            }
        }
        // Off the lock.
        const auto begin = std::chrono::steady_clock::now();
        PackScalePlan plan;
        PackFolderListing listing;
        std::optional<PackScaleRecord> remembered;
        uint32_t largest = 0U;
        uint64_t measured = 0U;
        size_t unreadable = 0U;
        switch (step) {
        case Step::Plan: plan = PlanPackScaleScan(root, std::move(indexed)); break;
        case Step::List: listing = ListPackFolder(folder.empty() ? root : root / folder); break;
        case Step::Record:
            if (std::ifstream file(record, std::ios::binary); !record.empty() && file) {
                std::string line;
                std::getline(file, line);
                remembered = ParsePackScaleRecord(line);
            }
            break;
        case Step::Headers:
            for (const auto& file : batch) {
                if (const auto size = ReadPngFileSize(file.Path)) {
                    largest = std::max(largest, ImageScaleOf(file.Native, *size));
                    ++measured;
                } else {
                    ++unreadable;
                }
            }
            break;
        case Step::Finish: break;
        }
        const auto micros = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());
        std::optional<PackScaleRecord> write;
        {
            std::scoped_lock lock(mMutex);
            auto& scan = mImageScaleScan;
            if (!scan.Active || scan.Generation != generation) return;
            scan.BusyMicros += micros;
            switch (step) {
            case Step::Plan:
                scan.Plan = std::move(plan);
                scan.Planned = true;
                break;
            case Step::List:
                scan.ListMicros += micros;
                for (auto& name : listing.Folders)
                    scan.Folders.push_back(folder.empty() ? std::move(name) : folder + "/" + name);
                for (const auto& [name, size] : listing.Files)
                    scan.Fingerprint.Add(folder.empty() ? name : folder + "/" + name, size);
                break;
            case Step::Record:
                scan.RecordChecked = true;
                if (mImageScaleKnown && mImageScaleKnown->Fingerprint == scan.Fingerprint) {
                    scan.Active = false; // the same pack, indexed again (a size cap change)
                } else if (remembered && remembered->Fingerprint == scan.Fingerprint) {
                    FinishImageScaleScanLocked(*remembered, true);
                } else {
                    ForgetImageScaleLocked(); // another pack in the folder: not known until measured
                }
                break;
            case Step::Headers:
                scan.Next += batch.size();
                scan.Largest = std::max(scan.Largest, largest);
                scan.Measured += measured;
                scan.Unreadable += unreadable;
                break;
            case Step::Finish:
                write = PackScaleRecord{scan.Fingerprint, scan.Largest, scan.Measured};
                FinishImageScaleScanLocked(*write, false);
                if (record.empty()) write.reset();
                break;
            }
        }
        // An SD write, off the lock.
        if (write) std::ofstream(record, std::ios::binary | std::ios::trunc) << FormatPackScaleRecord(*write);
    }

    void Rebuild(const AzaharTexturePackConfiguration& configuration, uint64_t serial,
                 const std::filesystem::path& loadDirectory, const std::filesystem::path& dumpDirectory) {
        // The paths the new index uses, for the image scale measured after it.
        std::vector<std::filesystem::path> indexedPaths;
        PackState pack;
        bool dumpModern = true;
        size_t unsupported = 0;
        std::string error;
        std::unordered_map<uint64_t, IndexedTexture> index;
        std::unordered_map<uint64_t, IndexedAzaharPbrMaterial> materials;
        try {
            pack = ReadPackState(loadDirectory / "pack.json", &error);
            if (!error.empty()) throw std::runtime_error(error);
            index = IndexTexturePack(loadDirectory, pack, unsupported, materials, &error);
            if (!error.empty()) throw std::runtime_error(error);
            if (configuration.DumpTextures) {
                const auto dumpPack = ReadPackState(dumpDirectory / "pack.json", &error);
                if (!error.empty()) throw std::runtime_error(error);
                dumpModern = dumpPack.ConfigurationFound ? dumpPack.UseNewHash : true;
                EnsureDumpPack(dumpDirectory, dumpModern);
            }
        } catch (const std::exception& exception) { error = exception.what(); }
        // The HUD list is read with the index, off the lock.
        std::vector<HudImageEntry> hudList;
        if (error.empty() && configuration.LoadCustomTextures && !configuration.HudImageList.empty()) {
            if (std::ifstream file(configuration.HudImageList, std::ios::binary); file) {
                const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                hudList = ParseHudImageList(text);
            }
        }
        // So are the scene lists.
        std::vector<SceneImageEntry> sceneList;
        if (error.empty() && configuration.LoadCustomTextures && !configuration.SceneImageList.empty()) {
            if (std::ifstream file(configuration.SceneImageList, std::ios::binary); file) {
                const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                sceneList = ParseSceneImageList(text);
            }
        }
        if (error.empty() && configuration.LoadCustomTextures) {
            indexedPaths.reserve(index.size());
            for (const auto& [hash, texture] : index) indexedPaths.push_back(texture.Path);
        }
        std::optional<std::vector<HudImageEntry>> hudListRewrite;
        std::optional<std::vector<SceneImageEntry>> sceneListRewrite;
        std::scoped_lock lock(mMutex);
        mBuilding = false;
        if (serial == mRebuildSerial) {
            mLastError = error;
            if (error.empty() || !mIndexReady) {
                if (!error.empty()) { index.clear(); materials.clear(); }
                if (mIndexReady && pack.UseNewHash != mPack.UseNewHash) {
                    mReplacementCache.clear();
                    mUiReplacementCache.clear();
                    mMaterialCache.clear();
                }
                mPack = std::move(pack);
                mIndex = std::move(index);
                mMaterialIndex = std::move(materials);
                mDumpUsesNewHash = dumpModern;
                mUnsupportedFiles = unsupported;
                mIndexReady = true;
                ++mGeneration;
                mPreviewSeamStatus.clear();
                ++mMaterialRevision;
                mFailedLoadHashes.clear();
                mFailedUiLoadHashes.clear();
                mFailedMaterialHashes.clear();
                mPendingLoadGenerations.clear();
                mPendingUiLoadGenerations.clear();
                mPendingMaterialGenerations.clear();
                std::erase_if(mReplacementCache, [&](const auto& entry) { return !mIndex.contains(entry.first); });
                std::erase_if(mUiReplacementCache, [&](const auto& entry) { return !mIndex.contains(entry.first); });
                std::erase_if(mMaterialCache, [&](const auto& entry) { return !mMaterialIndex.contains(entry.first); });
                uint64_t firstError = std::numeric_limits<uint64_t>::max();
                for (const auto& [hash, material] : mMaterialIndex) {
                    if (material.Error.empty()) continue;
                    mFailedMaterialHashes.insert(hash);
                    if (hash <= firstError) {
                        firstError = hash;
                        mLastError = "PBR material " + Hex16(hash) + ": " + material.Error;
                    }
                }
                if (error.empty()) hudListRewrite = PreloadHudImagesLocked(configuration, hudList);
                if (error.empty()) sceneListRewrite = LoadSceneListsLocked(configuration, sceneList);
                if (error.empty() && configuration.LoadCustomTextures) {
                    StartImageScaleScanLocked(configuration, loadDirectory, std::move(indexedPaths));
                } else {
                    ForgetImageScaleLocked();
                }
            }
        }
        // Lines of a pack no longer installed are dropped from the list, under
        // the lock so that no HUD worker append lands in between (rare: once
        // after the pack changed).
        if (hudListRewrite) {
            std::string text;
            for (const auto& entry : *hudListRewrite) text += FormatHudImageEntry(entry);
            std::ofstream(configuration.HudImageList, std::ios::binary | std::ios::trunc) << text;
        }
        if (sceneListRewrite) {
            std::string text;
            for (const auto& entry : *sceneListRewrite) text += FormatSceneImageEntry(entry);
            std::ofstream(configuration.SceneImageList, std::ios::binary | std::ios::trunc) << text;
        }
        mIdle.notify_all();
    }

    // Queues the listed HUD images the new index has on the HUD worker
    // (state under mMutex) and starts the HUD statistics of this index.
    // Returns the list to write back when some lines were dropped.
    std::optional<std::vector<HudImageEntry>> PreloadHudImagesLocked(const AzaharTexturePackConfiguration& configuration,
                                                                    const std::vector<HudImageEntry>& listed) {
        mHud = {};
        mHudListed.clear();
        mHudWaiting.clear();
        mMenuWaiting.clear();
        mHudPreloadStart = std::chrono::steady_clock::now();
        if (configuration.HudImageList.empty()) return std::nullopt;
        auto kept = PruneHudImageList(listed, [&](uint64_t hash) { return mIndex.contains(hash); });
        for (const auto& entry : kept) {
            mHudListed.insert(entry.Hash);
            (void)QueueOrPollLocked(entry.Hash, {entry.Width, entry.Height}, true, true);
        }
        mHud.Listed = kept.size();
        if (kept.size() == listed.size()) return std::nullopt;
        return kept;
    }

    // The scene lists of the new index (lines of images the pack no longer
    // has dropped), then the current scene's images queued. Returns the list
    // to write back when some lines were dropped.
    std::optional<std::vector<SceneImageEntry>> LoadSceneListsLocked(const AzaharTexturePackConfiguration& configuration,
                                                                     const std::vector<SceneImageEntry>& listed) {
        mSceneLists.clear();
        mSceneListed.clear();
        mSceneListLines = 0U;
        mSceneListAppend.clear();
        ResetScenePreloadLocked();
        if (configuration.SceneImageList.empty()) return std::nullopt;
        auto kept = PruneSceneImageList(listed, [&](uint64_t hash) { return mIndex.contains(hash); });
        for (const auto& entry : kept) {
            mSceneLists[entry.Scene].push_back(entry.Image);
            mSceneListed[entry.Scene].insert(entry.Image.Hash);
        }
        mSceneListLines = kept.size();
        QueueScenePreloadLocked();
        if (kept.size() == listed.size()) return std::nullopt;
        return kept;
    }

    void QueueDump(const AzaharTextureRequest& request, uint64_t hash, uint64_t generation,
                   const std::filesystem::path& directory) {
        if (!IsPowerOfTwo(request.Width) || !IsPowerOfTwo(request.Height)) return;
        if (request.NativeRgba8.size() != static_cast<size_t>(request.Width) * request.Height * 4U) {
            SetError("Azahar texture dump requires decoded RGBA8 bytes");
            return;
        }
        std::scoped_lock lock(mMutex);
        if (generation != mGeneration || !mConfiguration.DumpTextures || !mDumpedHashes.insert(hash).second) return;
        DumpJob job;
        job.Hash = hash;
        job.Width = request.Width;
        job.Height = request.Height;
        job.Destination = directory / MakeAzaharTextureFilename(request.Width, request.Height, hash, request.NativeFormat, request.MipLevel);
        job.Rgba8.assign(request.NativeRgba8.begin(), request.NativeRgba8.end());
        mDumpJobs.push_back(std::move(job));
        ++mPendingDumps;
        mWorkAvailable.notify_all();
    }
    void SetError(std::string error) {
        std::scoped_lock lock(mMutex);
        mLastError = std::move(error);
    }
    void Load(const LoadJob& job) {
        if (job.EditorImage) {
            AzaharImageLoadResult result;
            try { result.Image = DecodeReplacement(job.Texture, 0, 0); }
            catch (const std::exception& exception) { result.Error = exception.what(); }
            job.EditorImage->set_value(std::move(result));
            std::scoped_lock lock(mMutex);
            --mPendingLoads;
            mIdle.notify_all();
            return;
        }
        std::shared_ptr<const AzaharTextureReplacement> replacement;
        std::shared_ptr<const AzaharPbrMaterial> material;
        std::string failure;
        try {
            if (job.Material) {
                auto decoded = std::make_shared<AzaharPbrMaterial>();
                decoded->Identity = job.Identity;
                decoded->Generation = job.Generation;
                decoded->Parameters = job.Material->Parameters;
                if (!job.Texture.Path.empty()) {
                    {
                        std::scoped_lock lock(mMutex);
                        const auto cached = mReplacementCache.find(job.Hash);
                        if (cached != mReplacementCache.end() && cached->second->Generation == job.Generation &&
                            cached->second->SourcePath == job.Texture.Path)
                            decoded->ColorReplacement = cached->second;
                    }
                    if (!decoded->ColorReplacement)
                        decoded->ColorReplacement = DecodeReplacement(job.Texture, job.Hash, job.Generation);
                }
                for (size_t role = 0; role < decoded->Textures.size(); ++role) {
                    if (job.Material->Textures[role].empty()) continue;
                    decoded->Textures[role] = DecodeReplacement(
                        {job.Material->Textures[role], job.Texture.FlipPngFiles}, job.Hash, job.Generation);
                }
                material = std::move(decoded);
            } else {
                replacement = DecodeReplacement(job.Texture, job.Hash, job.Generation, job.Options);
            }
        } catch (const std::exception& exception) { failure = exception.what(); }
        std::filesystem::path hudList;
        HudImageEntry hudEntry;
        {
            std::scoped_lock lock(mMutex);
            auto& pending = job.Material ? mPendingMaterialGenerations
                                         : (job.UserInterface ? mPendingUiLoadGenerations : mPendingLoadGenerations);
            auto& failed = job.Material ? mFailedMaterialHashes
                                        : (job.UserInterface ? mFailedUiLoadHashes : mFailedLoadHashes);
            const auto found = pending.find(job.Hash);
            // An image decoded ahead for the visit now running counts; one of
            // an earlier visit (the scene changed while it decoded) is dropped.
            const bool currentPreload = job.ScenePreload && job.SceneSerial == mSceneSerial &&
                                        job.Generation == mGeneration;
            if (found != pending.end() && found->second == job.Generation) {
                if (job.Generation == mGeneration && mConfiguration.LoadCustomTextures) {
                    if (material && material->ColorReplacement && job.Material->BaseColorTexture.empty())
                        mReplacementCache.insert_or_assign(job.Hash, material->ColorReplacement);
                    if (material) mMaterialCache.insert_or_assign(job.Hash, std::move(material));
                    if (replacement && job.UserInterface &&
                        RecordHudImageLocked(job, *replacement, hudEntry))
                        hudList = mConfiguration.HudImageList;
                    // A preload of an earlier visit that the new scene lists
                    // too (still decoding when it was queued again) counts
                    // for the new one.
                    const bool listedNow =
                        job.ScenePreload && mSceneId >= 0 &&
                        mSceneListed.contains(static_cast<uint16_t>(mSceneId)) &&
                        mSceneListed.at(static_cast<uint16_t>(mSceneId)).contains(job.Hash);
                    if (replacement && job.ScenePreload && !currentPreload && !listedNow) replacement.reset();
                    if (replacement && job.ScenePreload) RecordScenePreloadLocked(job.Hash, *replacement);
                    if (replacement)
                        (job.UserInterface ? mUiReplacementCache : mReplacementCache)
                            .insert_or_assign(job.Hash, std::move(replacement));
                    if (!failure.empty()) {
                        failed.insert(job.Hash);
                        mLastError = std::move(failure);
                        if (job.UserInterface) {
                            mHudWaiting.erase(job.Hash);
                            mMenuWaiting.erase(job.Hash);
                        }
                    }
                }
                pending.erase(found);
            }
            if (hudList.empty()) {
                --mPendingLoads;
                mIdle.notify_all();
                return;
            }
        }
        // A HUD image not listed yet: the next start decodes it as soon as the
        // index is ready, before the UI has the game's texture to ask for.
        // Off the lock (an SD write), before the load counts as done.
        std::ofstream(hudList, std::ios::binary | std::ios::app) << FormatHudImageEntry(hudEntry);
        std::scoped_lock lock(mMutex);
        --mPendingLoads;
        mIdle.notify_all();
    }
    // HUD statistics of a decoded HUD image (decoded ahead, and/or asked for
    // by the HUD before it was ready); true (with its list line in `entry`)
    // when the HUD list does not have it yet.
    bool RecordHudImageLocked(const LoadJob& job, const AzaharTextureReplacement& replacement, HudImageEntry& entry) {
        const auto now = std::chrono::steady_clock::now();
        const auto micros = [](auto duration) {
            return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
        };
        if (job.Preload) {
            ++mHud.PreloadDecoded;
            mHud.PreloadBytes += replacement.Rgba8 ? replacement.Rgba8->size() : 0U;
            mHud.PreloadMicros = micros(now - mHudPreloadStart);
        }
        if (const auto waiting = mHudWaiting.find(job.Hash); waiting != mHudWaiting.end()) {
            ++mHud.OnSightDecodes;
            mHud.OnSightWaitMaxMicros = std::max(mHud.OnSightWaitMaxMicros, micros(now - waiting->second));
            mHudWaiting.erase(waiting);
        }
        if (const auto waiting = mMenuWaiting.find(job.Hash); waiting != mMenuWaiting.end()) {
            ++mHud.MenuOnSightDecodes;
            mHud.MenuOnSightWaitMaxMicros = std::max(mHud.MenuOnSightWaitMaxMicros, micros(now - waiting->second));
            mMenuWaiting.erase(waiting);
        }
        const auto width = job.Options.Native.Width, height = job.Options.Native.Height;
        if (mConfiguration.HudImageList.empty() || mHudListed.size() >= kHudImageListLimit || width == 0U ||
            height == 0U || width > 1024U || height > 1024U || !mHudListed.insert(job.Hash).second)
            return false;
        entry = {job.Hash, static_cast<uint16_t>(width), static_cast<uint16_t>(height)};
        ++mHud.Listed;
        return true;
    }
    // A world image decoded ahead for the current scene: held for its first
    // draw (until ReleaseDecoded, NoteSceneImage or the next change).
    void RecordScenePreloadLocked(uint64_t hash, const AzaharTextureReplacement& replacement) {
        uint64_t bytes = replacement.Rgba8 ? replacement.Rgba8->size() : 0U;
        for (const auto& mip : replacement.Mips) bytes += mip.size();
        ForgetScenePreloadLocked(hash);
        mScenePreloaded.emplace(hash, ScenePreload{bytes, false});
        mScenePreloadBytes += bytes;
        ++mScene.PreloadDecoded;
        mScene.PreloadBytes += bytes;
        mScene.PreloadMicros = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - mSceneStart)
                .count());
    }
    // The next world job (under mMutex): false for a job not to run, a stale
    // one or an image decoded ahead while the scene's unused images are over
    // the RAM budget (left to its first draw).
    bool TakeLoadJobLocked(LoadJob& load) {
        load = std::move(mLoadJobs.front());
        mLoadJobs.pop_front();
        if (!load.EditorImage && load.Generation != mGeneration) {
            --mPendingLoads;
            mIdle.notify_all();
            return false;
        }
        if (load.ScenePreload && mScenePreloadBytes >= kScenePreloadBudgetBytes) {
            if (const auto found = mPendingLoadGenerations.find(load.Hash);
                found != mPendingLoadGenerations.end() && found->second == load.Generation)
                mPendingLoadGenerations.erase(found);
            if (load.SceneSerial == mSceneSerial) ++mScene.PreloadOverBudget;
            --mPendingLoads;
            mIdle.notify_all();
            return false;
        }
        return true;
    }
    // World images only (kWorldDecodeWorkers - 1 of these): the pack worker
    // also indexes, dumps and writes the scene lists.
    void WorldWorkerMain() {
        for (;;) {
            LoadJob load;
            {
                std::unique_lock lock(mMutex);
                mWorkAvailable.wait(lock, [&] { return mStopping || !mLoadJobs.empty(); });
                if (mLoadJobs.empty()) return; // stopping
                if (!TakeLoadJobLocked(load)) continue;
            }
            Load(load);
        }
    }
    // HUD images only: a HUD texture never waits for a world image.
    void UiWorkerMain() {
        for (;;) {
            LoadJob load;
            {
                std::unique_lock lock(mMutex);
                mUiWorkAvailable.wait(lock, [&] { return mStopping || !mUiLoadJobs.empty(); });
                if (mUiLoadJobs.empty()) return; // stopping
                load = std::move(mUiLoadJobs.front());
                mUiLoadJobs.pop_front();
                if (load.Generation != mGeneration) {
                    --mPendingLoads;
                    mIdle.notify_all();
                    continue;
                }
            }
            if (!load.AheadNative.empty()) {
                IdentifyAhead(load);
            } else {
                Load(load);
            }
        }
    }
    void WorkerMain() {
        for (;;) {
            LoadJob load;
            DumpJob dump;
            AzaharTexturePackConfiguration configuration;
            std::filesystem::path loadDirectory, dumpDirectory;
            uint64_t rebuild = 0;
            bool loading = false, measuring = false;
            std::string sceneLines;
            std::filesystem::path sceneList;
            {
                std::unique_lock lock(mMutex);
                const auto ready = [&] {
                    return mStopping || mRebuildRequested || !mLoadJobs.empty() || !mDumpJobs.empty() ||
                           !mSceneListAppend.empty() || ImageScaleScanRunnableLocked();
                };
                // A measure waiting for the other workers' decodes looks again
                // now and then (they do not wake this worker).
                if (mImageScaleScan.Active) {
                    mWorkAvailable.wait_for(lock, kImageScaleRecheck, ready);
                } else {
                    mWorkAvailable.wait(lock, ready);
                }
                if (mRebuildRequested) {
                    rebuild = mRebuildSerial;
                    configuration = mConfiguration;
                    loadDirectory = mLoadDirectory;
                    dumpDirectory = mDumpDirectory;
                    mRebuildRequested = false;
                    mBuilding = true;
                } else if (!mSceneListAppend.empty()) {
                    sceneLines = std::move(mSceneListAppend);
                    mSceneListAppend.clear();
                    sceneList = mConfiguration.SceneImageList;
                    mSceneListWriting = true;
                } else if (!mLoadJobs.empty()) {
                    if (!TakeLoadJobLocked(load)) continue;
                    loading = true;
                } else if (!mDumpJobs.empty()) {
                    dump = std::move(mDumpJobs.front());
                    mDumpJobs.pop_front();
                } else if (mStopping) {
                    return;
                } else if (ImageScaleScanRunnableLocked()) {
                    measuring = true;
                } else {
                    continue;
                }
            }
            if (rebuild != 0) { Rebuild(configuration, rebuild, loadDirectory, dumpDirectory); continue; }
            if (measuring) { StepImageScaleScan(); continue; }
            // New scene list lines: an SD append, off the lock.
            if (!sceneLines.empty()) {
                if (!sceneList.empty()) std::ofstream(sceneList, std::ios::binary | std::ios::app) << sceneLines;
                std::scoped_lock lock(mMutex);
                mSceneListWriting = false;
                mIdle.notify_all();
                continue;
            }
            if (loading) { Load(load); continue; }
            bool written = false;
            std::string failure;
            try {
                if (!std::filesystem::exists(dump.Destination)) {
                    WritePng(dump.Destination, dump.Width, dump.Height, dump.Rgba8);
                    written = true;
                }
            } catch (const std::exception& exception) { failure = exception.what(); }
            std::scoped_lock lock(mMutex);
            if (written) ++mDumpedCount;
            if (!failure.empty()) {
                mDumpedHashes.erase(dump.Hash);
                mLastError = std::move(failure);
            }
            --mPendingDumps;
            mIdle.notify_all();
        }
    }

    mutable std::mutex mMutex;
    std::condition_variable mWorkAvailable, mUiWorkAvailable, mIdle;
    std::thread mWorker, mUiWorker;
    std::vector<std::thread> mWorldWorkers;
    bool mStopping = false, mConfigured = false, mIndexReady = false;
    bool mRebuildRequested = false, mBuilding = false;
    uint64_t mGeneration = 1U, mRebuildSerial = 0U;
    uint64_t mMaterialRevision = 1U;
    std::optional<AzaharMaterialParameterPreview> mPreview;
    std::string mPreviewSeamStatus;
    AzaharTexturePackConfiguration mConfiguration;
    std::filesystem::path mLoadDirectory, mDumpDirectory;
    PackState mPack;
    bool mDumpUsesNewHash = true;
    std::unordered_map<uint64_t, IndexedTexture> mIndex;
    std::unordered_map<uint64_t, IndexedAzaharPbrMaterial> mMaterialIndex;
    std::unordered_map<uint64_t, std::shared_ptr<const AzaharTextureReplacement>> mReplacementCache;
    // HUD images (AzaharTextureRequest::UserInterface), apart from world ones.
    std::unordered_map<uint64_t, std::shared_ptr<const AzaharTextureReplacement>> mUiReplacementCache;
    std::unordered_map<uint64_t, std::shared_ptr<const AzaharPbrMaterial>> mMaterialCache;
    std::unordered_map<uint64_t, uint64_t> mPendingLoadGenerations, mPendingUiLoadGenerations,
        mPendingMaterialGenerations;
    std::unordered_set<uint64_t> mFailedLoadHashes, mFailedUiLoadHashes, mFailedMaterialHashes, mDumpedHashes;
    std::deque<LoadJob> mLoadJobs, mUiLoadJobs;
    // HUD statistics since the index was built, the HUD list's hashes, when
    // the index was ready (the start of the ahead decodes), and the HUD images
    // asked for by the HUD while still decoding, with the first such request.
    AzaharHudPackStatus mHud;
    std::unordered_set<uint64_t> mHudListed;
    std::chrono::steady_clock::time_point mHudPreloadStart;
    std::unordered_map<uint64_t, std::chrono::steady_clock::time_point> mHudWaiting, mMenuWaiting;
    std::deque<DumpJob> mDumpJobs;
    size_t mPendingLoads = 0U, mPendingDumps = 0U, mDumpedCount = 0U, mUnsupportedFiles = 0U;
    std::string mLastError;
    // Scenes (NoteScene): the current one (-1 outside gameplay) and its visit
    // (serial, start, statistics; the visit before kept for the log), the
    // scene lists (in order, and as sets), lines not yet written, and the
    // world images decoded ahead for this visit (bytes, and whether a draw
    // asked for each), with the jobs still to finish.
    struct ScenePreload {
        uint64_t Bytes = 0;
        bool Used = false;
    };
    int mSceneId = -1;
    uint64_t mSceneSerial = 0U;
    std::atomic<uint64_t> mSceneSerialPublished{0U};
    std::chrono::steady_clock::time_point mSceneStart;
    AzaharScenePackStatus mScene, mPreviousScene;
    std::unordered_map<uint16_t, std::vector<HudImageEntry>> mSceneLists;
    std::unordered_map<uint16_t, std::unordered_set<uint64_t>> mSceneListed;
    size_t mSceneListLines = 0U;
    std::string mSceneListAppend;
    bool mSceneListWriting = false;
    std::unordered_map<uint64_t, ScenePreload> mScenePreloaded;
    std::unordered_set<uint64_t> mSceneOnSight;
    uint64_t mScenePreloadBytes = 0U;
    // The pack's image scale: the measure running, the last result for this
    // folder (kept through a size cap change), its details and the published
    // values (0 = not known).
    ImageScaleScan mImageScaleScan;
    std::optional<PackScaleRecord> mImageScaleKnown;
    AzaharImageScaleStatus mImageScaleStatus;
    std::atomic<uint32_t> mLargestImageScale{0U};
    std::atomic<uint64_t> mImageScaleSerial{0U};
};

AzaharTexturePackRuntime::AzaharTexturePackRuntime() : mImpl(std::make_unique<Impl>()) {
}

AzaharTexturePackRuntime::~AzaharTexturePackRuntime() = default;

AzaharTexturePackRuntime& AzaharTexturePackRuntime::Instance() {
    static AzaharTexturePackRuntime runtime;
    return runtime;
}

void AzaharTexturePackRuntime::Configure(AzaharTexturePackConfiguration configuration) {
    mImpl->Configure(std::move(configuration));
}

void AzaharTexturePackRuntime::Reload() {
    mImpl->Reload();
}

std::shared_ptr<const AzaharTextureReplacement>
AzaharTexturePackRuntime::ResolveAndMaybeDump(const AzaharTextureRequest& request) {
    return mImpl->ResolveAndMaybeDump(request);
}

AzaharTextureResolveResult
AzaharTexturePackRuntime::ResolveOrQueue(
    const AzaharTextureRequest& request) {
    return mImpl->ResolveOrQueue(request);
}

AzaharTextureResolveResult
AzaharTexturePackRuntime::PollQueued(
    uint64_t nativeHash) const {
    return mImpl->PollQueued(nativeHash);
}

AzaharTextureResolveResult AzaharTexturePackRuntime::PollOrQueue(uint64_t nativeHash, uint16_t nativeWidth,
                                                                  uint16_t nativeHeight, bool userInterface,
                                                                  bool menu) {
    return mImpl->PollOrQueue(nativeHash, {nativeWidth, nativeHeight}, userInterface, menu);
}

void AzaharTexturePackRuntime::ReleaseDecoded(uint64_t nativeHash, uint64_t generation) {
    mImpl->ReleaseDecoded(nativeHash, generation);
}

AzaharMaterialResolveResult AzaharTexturePackRuntime::ResolveMaterialOrQueue(
    uint64_t nativeHash, uint64_t expectedGeneration) {
    return mImpl->ResolveMaterialOrQueue(nativeHash, expectedGeneration);
}

void AzaharTexturePackRuntime::ReportMaterialUploadFailure(
    uint64_t nativeHash, uint64_t generation, std::string error) {
    mImpl->ReportMaterialUploadFailure(nativeHash, generation, std::move(error));
}

AzaharTexturePackStatus AzaharTexturePackRuntime::Snapshot() const {
    return mImpl->Snapshot();
}

uint64_t AzaharTexturePackRuntime::Generation() const {
    return mImpl->Generation();
}

bool AzaharTexturePackRuntime::IndexReady() const {
    return mImpl->IndexReady();
}

uint32_t AzaharTexturePackRuntime::LargestImageScale() const {
    return mImpl->LargestImageScale();
}

uint64_t AzaharTexturePackRuntime::ImageScaleSerial() const {
    return mImpl->ImageScaleSerial();
}

AzaharImageScaleStatus AzaharTexturePackRuntime::ImageScaleStatus() const {
    return mImpl->ImageScaleStatus();
}

AzaharHudPackStatus AzaharTexturePackRuntime::HudStatus() const {
    return mImpl->HudStatus();
}

void AzaharTexturePackRuntime::NoteScene(int scene) {
    mImpl->NoteScene(scene);
}

uint64_t AzaharTexturePackRuntime::SceneSerial() const {
    return mImpl->SceneSerial();
}

AzaharScenePackStatus AzaharTexturePackRuntime::SceneStatus() const {
    return mImpl->SceneStatus();
}

AzaharScenePackStatus AzaharTexturePackRuntime::PreviousSceneStatus() const {
    return mImpl->PreviousSceneStatus();
}

size_t AzaharTexturePackRuntime::PendingLoads() const {
    return mImpl->PendingLoads();
}

void AzaharTexturePackRuntime::NoteSceneImage(uint64_t nativeHash, uint16_t nativeWidth, uint16_t nativeHeight) {
    mImpl->NoteSceneImage(nativeHash, {nativeWidth, nativeHeight});
}

size_t AzaharTexturePackRuntime::WaitForImages(std::span<const uint64_t> nativeHashes,
                                               std::chrono::microseconds timeout) {
    return mImpl->WaitForImages(nativeHashes, timeout);
}

void AzaharTexturePackRuntime::WaitForPendingDumps() {
    mImpl->WaitForPendingDumps();
}

void AzaharTexturePackRuntime::WaitForPendingLoads() {
    mImpl->WaitForPendingLoads();
}

std::vector<AzaharMaterialCatalogEntry> AzaharTexturePackRuntime::MaterialCatalog() const {
    return mImpl->MaterialCatalog();
}

std::future<AzaharImageLoadResult> AzaharTexturePackRuntime::LoadEditorImage(std::filesystem::path path) {
    return mImpl->LoadEditorImage(std::move(path));
}

void AzaharTexturePackRuntime::SetMaterialPreview(std::optional<AzaharMaterialParameterPreview> preview) {
    mImpl->SetMaterialPreview(std::move(preview));
}

void AzaharTexturePackRuntime::ReportMaterialSeams(uint64_t hash, uint64_t generation, std::string_view status) {
    mImpl->ReportMaterialSeams(hash, generation, status);
}
std::string AzaharTexturePackRuntime::MaterialSeamStatus(uint64_t hash) const {
    return mImpl->MaterialSeamStatus(hash);
}

uint64_t AzaharTexturePackRuntime::MaterialRevision() const {
    return mImpl->MaterialRevision();
}

void WriteAzaharTexturePng(const std::filesystem::path& path, uint32_t width,
                          uint32_t height, std::span<const uint8_t> rgba8) {
    WritePng(path, width, height, rgba8);
}

} // namespace Oot3d::Renderer
