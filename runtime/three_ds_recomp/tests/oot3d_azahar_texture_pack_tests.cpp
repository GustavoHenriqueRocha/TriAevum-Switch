#include "oot3d/renderer/azahar_texture_pack.h"
#include "oot3d/renderer/azahar_hud_image_list.h"
#include "oot3d/renderer/azahar_scene_image_list.h"
#include "oot3d/renderer/azahar_texture_pack_settings.h"
#include "oot3d/renderer/pica_texture_decode.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {

class Oot3dAzaharTexturePackTest : public testing::Test {
  protected:
    void SetUp() override {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        Root = std::filesystem::temp_directory_path() / ("oot3d_azahar_texture_pack_" + std::to_string(suffix));
        std::filesystem::create_directories(Root);
    }

    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(Root, ignored);
    }

    std::filesystem::path Root;
};

TEST(Oot3dAzaharTexturePack, MatchesAzaharCityHashVectors) {
    using Oot3d::Renderer::AzaharCityHash64;
    const auto bytes = [](std::string_view text) {
        return std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    };
    EXPECT_EQ(AzaharCityHash64(bytes("")), 0x9AE16A3B2F90404FULL);
    EXPECT_EQ(AzaharCityHash64(bytes("a")), 0xB3454265B6DF75E3ULL);
    EXPECT_EQ(AzaharCityHash64(bytes("abc")), 0x24A5B3A074E7F369ULL);
    EXPECT_EQ(AzaharCityHash64(bytes("hello")), 0xB48BE5A931380CE8ULL);

    std::array<uint8_t, 256> sequence{};
    for (size_t index = 0; index < sequence.size(); ++index) {
        sequence[index] = static_cast<uint8_t>(index);
    }
    EXPECT_EQ(AzaharCityHash64(sequence), 0x0C693049D8C2C68DULL);
}

TEST(Oot3dAzaharTexturePack, UsesExactDumpFilenameContract) {
    EXPECT_EQ(Oot3d::Renderer::MakeAzaharTextureFilename(128U, 128U, 0x083287DDE382AE8FULL, 13U, 0U),
              "tex1_128x128_083287DDE382AE8F_13_mip0.png");
}

TEST(Oot3dAzaharTexturePack, RepeatedWorkerStartupAndShutdownIsStable) {
    for (size_t iteration = 0; iteration < 256U; ++iteration) {
        Oot3d::Renderer::AzaharTexturePackRuntime runtime;
    }
}

TEST(Oot3dAzaharTexturePack, MatchesLegacyDetiledHash) {
    std::array<uint8_t, 256> native{};
    for (size_t index = 0; index < native.size(); ++index) {
        native[index] = static_cast<uint8_t>(index);
    }
    std::array<uint8_t, 256> rgba{};
    const Oot3d::Renderer::AzaharTextureRequest request{ 8U, 8U, 0U, 0U, native, rgba };
    std::string error;
    EXPECT_EQ(Oot3d::Renderer::ComputeAzaharTextureHash(request, false, &error), 0x34487FA49AF68E6BULL) << error;
}

TEST_F(Oot3dAzaharTexturePackTest, DumpsAndLoadsModernAzaharPngWithoutOrientationLoss) {
    constexpr uint64_t titleId = 0x0004000000000042ULL;
    std::array<uint8_t, 256> native{};
    for (size_t index = 0; index < native.size(); ++index) {
        native[index] = static_cast<uint8_t>((index * 37U) & 0xFFU);
    }
    std::vector<uint8_t> rgba(8U * 8U * 4U);
    for (uint32_t y = 0; y < 8U; ++y) {
        for (uint32_t x = 0; x < 8U; ++x) {
            const size_t offset = (static_cast<size_t>(y) * 8U + x) * 4U;
            rgba[offset] = static_cast<uint8_t>(y * 31U);
            rgba[offset + 1U] = static_cast<uint8_t>(x * 29U);
            rgba[offset + 2U] = static_cast<uint8_t>(x + y);
            rgba[offset + 3U] = 255U;
        }
    }
    const Oot3d::Renderer::AzaharTextureRequest request{ 8U, 8U, 0U, 0U, native, rgba };
    const uint64_t hash = Oot3d::Renderer::AzaharCityHash64(native);
    const auto dumpDirectory = Root / "chosen_dump";

    Oot3d::Renderer::AzaharTexturePackRuntime runtime;
    runtime.Configure({
        .DumpTextures = true,
        .LoadCustomTextures = false,
        .TitleId = titleId,
        .UserDirectory = Root,
        .DumpDirectory = dumpDirectory,
    });
    EXPECT_EQ(runtime.ResolveAndMaybeDump(request), nullptr);
    runtime.WaitForPendingDumps();

    const auto status = runtime.Snapshot();
    const auto filename = Oot3d::Renderer::MakeAzaharTextureFilename(8U, 8U, hash, 0U, 0U);
    const auto dumped = status.DumpDirectory / filename;
    EXPECT_EQ(status.DumpDirectory, dumpDirectory);
    ASSERT_TRUE(std::filesystem::exists(dumped));
    ASSERT_TRUE(std::filesystem::exists(status.DumpDirectory / "pack.json"));
    EXPECT_EQ(status.DumpedTextures, 1U);

    const auto loadDirectory = Root / "chosen_load";
    std::filesystem::create_directories(loadDirectory);
    std::filesystem::copy_file(dumped, loadDirectory / filename);
    std::filesystem::copy_file(status.DumpDirectory / "pack.json", loadDirectory / "pack.json");

    runtime.Configure({
        .DumpTextures = false,
        .LoadCustomTextures = true,
        .TitleId = titleId,
        .UserDirectory = Root,
        .LoadDirectory = loadDirectory,
    });
    const auto replacement = runtime.ResolveAndMaybeDump(request);
    ASSERT_NE(replacement, nullptr);
    ASSERT_NE(replacement->Rgba8, nullptr);
    EXPECT_EQ(replacement->Width, 8U);
    EXPECT_EQ(replacement->Height, 8U);
    EXPECT_EQ(*replacement->Rgba8, rgba);
    EXPECT_EQ(replacement->SourcePath, loadDirectory / filename);
    EXPECT_EQ(runtime.Snapshot().LoadDirectory, loadDirectory);
    EXPECT_EQ(runtime.Snapshot().LoadedTextures, 1U);
}

TEST_F(Oot3dAzaharTexturePackTest,
       QueuesReplacementDecodeWithoutBlockingTheCaller) {
    constexpr uint64_t titleId = 0x0004000000000044ULL;
    std::array<uint8_t, 256> native{};
    for (size_t index = 0; index < native.size(); ++index) {
        native[index] =
            static_cast<uint8_t>((index * 19U) & 0xFFU);
    }
    std::vector<uint8_t> rgba(8U * 8U * 4U);
    for (size_t index = 0; index < rgba.size(); index += 4U) {
        rgba[index] = static_cast<uint8_t>(index);
        rgba[index + 1U] = 91U;
        rgba[index + 2U] = 173U;
        rgba[index + 3U] = 255U;
    }
    const Oot3d::Renderer::AzaharTextureRequest request{
        8U, 8U, 0U, 0U, native, rgba};
    const uint64_t hash =
        Oot3d::Renderer::AzaharCityHash64(native);

    Oot3d::Renderer::AzaharTexturePackRuntime runtime;
    runtime.Configure({
        .DumpTextures = true,
        .TitleId = titleId,
        .UserDirectory = Root,
    });
    (void)runtime.ResolveAndMaybeDump(request);
    runtime.WaitForPendingDumps();
    const auto dumped = runtime.Snapshot().DumpDirectory /
                        Oot3d::Renderer::MakeAzaharTextureFilename(
                            8U, 8U, hash, 0U, 0U);

    const auto loadDirectory = Root / "async_load";
    std::filesystem::create_directories(loadDirectory);
    std::filesystem::copy_file(
        dumped, loadDirectory / dumped.filename());
    std::filesystem::copy_file(
        runtime.Snapshot().DumpDirectory / "pack.json",
        loadDirectory / "pack.json");
    runtime.Configure({
        .LoadCustomTextures = true,
        .TitleId = titleId,
        .UserDirectory = Root,
        .LoadDirectory = loadDirectory,
    });
    runtime.WaitForPendingLoads();

    const auto queued = runtime.ResolveOrQueue(request);
    EXPECT_EQ(
        queued.State,
        Oot3d::Renderer::AzaharTextureResolveState::Pending);
    EXPECT_EQ(queued.NativeHash, hash);
    EXPECT_TRUE(queued.NativeHashAvailable);
    EXPECT_TRUE(queued.UseNewHash);
    EXPECT_EQ(queued.Replacement, nullptr);
    runtime.WaitForPendingLoads();

    const auto ready = runtime.PollQueued(hash);
    ASSERT_EQ(
        ready.State,
        Oot3d::Renderer::AzaharTextureResolveState::Ready);
    ASSERT_NE(ready.Replacement, nullptr);
    ASSERT_NE(ready.Replacement->Rgba8, nullptr);
    EXPECT_EQ(*ready.Replacement->Rgba8, rgba);
    EXPECT_EQ(runtime.Snapshot().PendingLoads, 0U);
    EXPECT_EQ(runtime.Snapshot().FailedLoads, 0U);

    // Authoring still needs the native pack identity when replacement loading is off.
    runtime.Configure({
        .LoadCustomTextures = false,
        .TitleId = titleId,
        .UserDirectory = Root,
        .LoadDirectory = loadDirectory,
    });
    runtime.WaitForPendingLoads();
    const auto disabled = runtime.ResolveOrQueue(request);
    EXPECT_EQ(disabled.State, Oot3d::Renderer::AzaharTextureResolveState::Disabled);
    ASSERT_TRUE(disabled.NativeHashAvailable);
    EXPECT_EQ(disabled.NativeHash, hash);
    EXPECT_TRUE(disabled.UseNewHash);
}

TEST_F(Oot3dAzaharTexturePackTest, HonorsPackJsonFilenameMappingAndFlipSemantics) {
    constexpr uint64_t titleId = 0x0004000000000043ULL;
    std::array<uint8_t, 256> native{};
    for (size_t index = 0; index < native.size(); ++index) {
        native[index] = static_cast<uint8_t>((index * 13U) & 0xFFU);
    }
    std::vector<uint8_t> rgba(8U * 8U * 4U, 255U);
    for (uint32_t y = 0; y < 8U; ++y) {
        for (uint32_t x = 0; x < 8U; ++x) {
            rgba[(static_cast<size_t>(y) * 8U + x) * 4U] = static_cast<uint8_t>(y * 20U + x);
        }
    }
    const Oot3d::Renderer::AzaharTextureRequest request{ 8U, 8U, 0U, 0U, native, rgba };
    const uint64_t hash = Oot3d::Renderer::AzaharCityHash64(native);

    Oot3d::Renderer::AzaharTexturePackRuntime runtime;
    runtime.Configure({
        .DumpTextures = true,
        .TitleId = titleId,
        .UserDirectory = Root,
    });
    (void)runtime.ResolveAndMaybeDump(request);
    runtime.WaitForPendingDumps();
    const auto dumpStatus = runtime.Snapshot();
    const auto dumped = dumpStatus.DumpDirectory / Oot3d::Renderer::MakeAzaharTextureFilename(8U, 8U, hash, 0U, 0U);

    const auto loadDirectory = Root / "load" / "textures" / "0004000000000043";
    const auto customPath = loadDirectory / "nested" / "custom.png";
    std::filesystem::create_directories(customPath.parent_path());
    std::filesystem::copy_file(dumped, customPath);
    const nlohmann::json pack{
        { "options",
          {
              { "skip_mipmap", false },
              { "flip_png_files", false },
              { "use_new_hash", true },
          } },
        { "textures",
          { { [hash] {
                 std::ostringstream output;
                 output << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << hash;
                 return output.str();
             }(),
              "nested/custom.png" } } },
    };
    {
        std::ofstream output(loadDirectory / "pack.json");
        output << pack.dump(2);
    }

    runtime.Configure({
        .LoadCustomTextures = true,
        .TitleId = titleId,
        .UserDirectory = Root,
    });
    const auto replacement = runtime.ResolveAndMaybeDump(request);
    ASSERT_NE(replacement, nullptr);
    ASSERT_NE(replacement->Rgba8, nullptr);
    for (uint32_t y = 0; y < 8U; ++y) {
        for (uint32_t x = 0; x < 8U; ++x) {
            const size_t actual = (static_cast<size_t>(y) * 8U + x) * 4U;
            const size_t expected = (static_cast<size_t>(7U - y) * 8U + x) * 4U;
            EXPECT_EQ((*replacement->Rgba8)[actual], rgba[expected]);
        }
    }
}

TEST_F(Oot3dAzaharTexturePackTest, MaterialReloadIsAtomicAndExplicitNullOverridesSuffixes) {
    using namespace Oot3d::Renderer;
    AzaharTexturePackRuntime runtime;
    const auto dump = Root / "dump";
    runtime.Configure({.DumpTextures = true, .UserDirectory = Root, .DumpDirectory = dump});
    std::array<uint8_t, 256> native{};
    std::array<uint8_t, 256> pixels{};
    const auto makeImage = [&](uint8_t value) {
        native.fill(value);
        pixels.fill(value);
        (void)runtime.ResolveAndMaybeDump({8, 8, 0, 0, native, pixels});
        runtime.WaitForPendingDumps();
        return dump / MakeAzaharTextureFilename(8, 8, AzaharCityHash64(native), 0, 0);
    };
    const auto normal = makeImage(127U);
    const auto orm = makeImage(63U);
    const auto emission = makeImage(191U);
    const auto height = makeImage(223U);
    native.fill(19U);
    const auto hash = AzaharCityHash64(native);
    std::ostringstream formatted;
    formatted << std::uppercase << std::hex << std::setw(16) << std::setfill('0') << hash;
    const auto name = formatted.str();
    const auto pack = Root / "pack";
    const auto materials = pack / "materials";
    std::filesystem::create_directories(materials);
    std::filesystem::copy_file(dump / "pack.json", pack / "pack.json");
    std::filesystem::copy_file(normal, materials / (name + "_n.png"));
    std::filesystem::copy_file(orm, materials / (name + "_orm.png"));
    std::filesystem::copy_file(emission, materials / (name + "_e.png"));
    std::filesystem::copy_file(height, materials / (name + "_h.png"));
    runtime.Configure({.LoadCustomTextures = true, .UserDirectory = Root, .LoadDirectory = pack});
    runtime.WaitForPendingLoads();
    const auto generation = runtime.Generation();
    EXPECT_EQ(runtime.ResolveOrQueue({8, 8, 0, 0, native, pixels}).State, AzaharTextureResolveState::Missing);
    EXPECT_EQ(runtime.ResolveMaterialOrQueue(hash, generation).State, AzaharTextureResolveState::Pending);
    runtime.WaitForPendingLoads();
    const auto ready = runtime.ResolveMaterialOrQueue(hash, generation);
    ASSERT_EQ(ready.State, AzaharTextureResolveState::Ready);
    ASSERT_NE(ready.Material, nullptr);
    for (const auto& texture : ready.Material->Textures) ASSERT_NE(texture, nullptr);
    EXPECT_EQ((*ready.Material->Textures[0]->Rgba8)[0], 127U);
    EXPECT_EQ((*ready.Material->Textures[1]->Rgba8)[0], 63U);
    EXPECT_EQ((*ready.Material->Textures[2]->Rgba8)[0], 191U);
    EXPECT_EQ((*ready.Material->Textures[3]->Rgba8)[0], 223U);
    EXPECT_EQ(ready.Material->Identity.NativeHash, hash);
    EXPECT_TRUE(ready.Material->Identity.UseNewHash);
    const auto jsonPath = materials / (name + ".material.json");
    nlohmann::json metadata{{"schema", "triaevum.pbr-material"}, {"version", 1},
        {"orm", {{"texture", "missing.png"}}}};
    { std::ofstream output(jsonPath); output << metadata; }
    runtime.Reload();
    runtime.WaitForPendingLoads();
    const auto failed = runtime.ResolveMaterialOrQueue(hash, runtime.Generation());
    ASSERT_NE(failed.Material, nullptr);
    EXPECT_EQ(failed.Material->Generation, ready.Material->Generation);
    EXPECT_EQ(runtime.Snapshot().FailedMaterials, 1U);
    metadata["normal"] = nullptr;
    metadata["orm"] = {{"texture", nullptr}, {"metallicFactor", 0.75F}};
    metadata["height"] = nullptr;
    { std::ofstream output(jsonPath); output << metadata; }
    const auto colorPath = pack / MakeAzaharTextureFilename(8, 8, hash, 0, 0);
    std::filesystem::copy_file(emission, colorPath);
    runtime.Reload();
    runtime.WaitForPendingLoads();
    (void)runtime.ResolveMaterialOrQueue(hash, runtime.Generation());
    runtime.WaitForPendingLoads();
    const auto changed = runtime.ResolveMaterialOrQueue(hash, runtime.Generation());
    ASSERT_NE(changed.Material, nullptr);
    EXPECT_EQ(changed.Material->Textures[0], nullptr);
    EXPECT_EQ(changed.Material->Textures[1], nullptr);
    ASSERT_NE(changed.Material->Textures[2], nullptr);
    EXPECT_EQ(changed.Material->Textures[3], nullptr);
    ASSERT_NE(changed.Material->ColorReplacement, nullptr);
    EXPECT_EQ((*changed.Material->ColorReplacement->Rgba8)[0], 191U);
    EXPECT_EQ(changed.Material->Parameters.MetallicFactor, 0.75F);
    EXPECT_GT(changed.Material->Generation, ready.Material->Generation);
    EXPECT_EQ(runtime.ResolveMaterialOrQueue(hash, generation).State, AzaharTextureResolveState::Disabled);
    { std::ofstream brokenColor(colorPath, std::ios::binary | std::ios::trunc); brokenColor << "invalid PNG"; }
    runtime.Reload();
    runtime.WaitForPendingLoads();
    (void)runtime.ResolveMaterialOrQueue(hash, runtime.Generation());
    runtime.WaitForPendingLoads();
    const auto broken = runtime.ResolveMaterialOrQueue(hash, runtime.Generation());
    ASSERT_NE(broken.Material, nullptr);
    EXPECT_EQ(broken.Material->Generation, changed.Material->Generation);
    EXPECT_EQ((*broken.Material->ColorReplacement->Rgba8)[0], 191U);
    EXPECT_EQ(runtime.Snapshot().FailedMaterials, 1U);
}

TEST_F(Oot3dAzaharTexturePackTest, RejectsEscapingMaterialPathsAndNormalAlbedoCollisions) {
    using namespace Oot3d::Renderer;
    const auto pack = Root / "pack";
    std::filesystem::create_directories(pack);
    std::array<uint8_t, 256> native{};
    const auto hash = ComputeAzaharTextureHash({8, 8, 0, 0, native, {}}, false);
    std::ostringstream formatted;
    formatted << std::hex << hash;
    const auto name = formatted.str();
    { std::ofstream image(pack / ("tex1_8x8_" + name + "_0_n.png")); image << "not albedo"; }
    { std::ofstream outside(Root / "outside.png"); outside << "private"; }
    {
        std::ofstream metadata(pack / (name + ".material.json"));
        metadata << nlohmann::json{{"schema", "triaevum.pbr-material"}, {"version", 1},
            {"normal", {{"texture", "../outside.png"}}}};
    }
    AzaharTexturePackRuntime runtime;
    runtime.Configure({.LoadCustomTextures = true, .UserDirectory = Root, .LoadDirectory = pack});
    runtime.WaitForPendingLoads();
    EXPECT_EQ(runtime.ResolveOrQueue({8, 8, 0, 0, native, {}}).State, AzaharTextureResolveState::Missing);
    EXPECT_EQ(runtime.ResolveMaterialOrQueue(hash, runtime.Generation()).State, AzaharTextureResolveState::Failed);
    EXPECT_EQ(runtime.Snapshot().LoadedMaterials, 0U);
}

TEST_F(Oot3dAzaharTexturePackTest, CapsMipmapsAndReleasesReplacementsOnTheWorker) {
    using namespace Oot3d::Renderer;
    std::array<uint8_t, 256> native{};
    for (size_t index = 0; index < native.size(); ++index) native[index] = static_cast<uint8_t>(index * 7U);
    const uint64_t hash = AzaharCityHash64(native);
    const auto pack = Root / "capped";
    std::filesystem::create_directories(pack);
    {
        std::ofstream output(pack / "pack.json");
        output << nlohmann::json{{"options", {{"skip_mipmap", true}, {"flip_png_files", true}, {"use_new_hash", true}}}};
    }
    // A 32x32 image (4x) for an 8x8 native texture: left half red, right half blue.
    std::vector<uint8_t> large(32U * 32U * 4U);
    for (size_t texel = 0; texel < 32U * 32U; ++texel) {
        const bool right = texel % 32U >= 16U;
        large[texel * 4U] = right ? 0U : 255U;
        large[texel * 4U + 2U] = right ? 255U : 0U;
        large[texel * 4U + 3U] = 255U;
    }
    WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(8U, 8U, hash, 0U, 0U), 32U, 32U, large);
    const AzaharTextureRequest request{8U, 8U, 0U, 0U, native, {}};

    AzaharTexturePackRuntime runtime;
    runtime.Configure({.LoadCustomTextures = true, .UserDirectory = Root, .LoadDirectory = pack,
                       .MaxReplacementScale = 2U, .GenerateMipmaps = true});
    runtime.WaitForPendingLoads();
    EXPECT_EQ(runtime.ResolveOrQueue(request).State, AzaharTextureResolveState::Pending);
    runtime.WaitForPendingLoads();
    const auto capped = runtime.PollQueued(hash);
    ASSERT_EQ(capped.State, AzaharTextureResolveState::Ready);
    EXPECT_EQ(capped.Replacement->Width, 16U);
    EXPECT_EQ(capped.Replacement->Height, 16U);
    ASSERT_EQ(capped.Replacement->Rgba8->size(), 16U * 16U * 4U);
    EXPECT_EQ((*capped.Replacement->Rgba8)[0], 255U);          // still red on the left
    EXPECT_EQ((*capped.Replacement->Rgba8)[15U * 4U + 2U], 255U); // blue on the right
    ASSERT_EQ(capped.Replacement->Mips.size(), 4U);              // 8x8 4x4 2x2 1x1
    EXPECT_EQ(capped.Replacement->Mips[0].size(), 8U * 8U * 4U);
    EXPECT_EQ(capped.Replacement->Mips[3].size(), 4U);
    EXPECT_EQ(capped.Replacement->Mips[3][0], 128U); // half red, half blue

    // A renderer that holds the image on the GPU drops the decoded copy; the
    // next poll decodes it again.
    runtime.ReleaseDecoded(hash, capped.Replacement->Generation);
    EXPECT_EQ(runtime.Snapshot().LoadedTextures, 0U);
    EXPECT_EQ(runtime.PollQueued(hash).State, AzaharTextureResolveState::Missing);
    EXPECT_EQ(runtime.PollOrQueue(hash, 8U, 8U).State, AzaharTextureResolveState::Pending);
    runtime.WaitForPendingLoads();
    const auto again = runtime.PollOrQueue(hash, 8U, 8U);
    ASSERT_EQ(again.State, AzaharTextureResolveState::Ready);
    EXPECT_EQ(again.Replacement->Width, 16U);

    // Cap 0 keeps the pack's own size; no mipmaps unless asked for.
    runtime.Configure({.LoadCustomTextures = true, .UserDirectory = Root, .LoadDirectory = pack});
    runtime.WaitForPendingLoads();
    (void)runtime.ResolveOrQueue(request);
    runtime.WaitForPendingLoads();
    const auto full = runtime.PollQueued(hash);
    ASSERT_EQ(full.State, AzaharTextureResolveState::Ready);
    EXPECT_EQ(full.Replacement->Width, 32U);
    EXPECT_TRUE(full.Replacement->Mips.empty());
    EXPECT_EQ(*full.Replacement->Rgba8, large);

    // Turning loading off drops the decoded images.
    runtime.Configure({.LoadCustomTextures = false, .UserDirectory = Root, .LoadDirectory = pack});
    runtime.WaitForPendingLoads();
    EXPECT_EQ(runtime.Snapshot().LoadedTextures, 0U);
    EXPECT_EQ(runtime.PollOrQueue(hash, 8U, 8U).State, AzaharTextureResolveState::Disabled);
}

TEST_F(Oot3dAzaharTexturePackTest, CapsHudTexturesAtThreeTimesWithoutMipmaps) {
    using namespace Oot3d::Renderer;
    std::array<uint8_t, 256> native{};
    for (size_t index = 0; index < native.size(); ++index) native[index] = static_cast<uint8_t>(index * 11U);
    const uint64_t hash = AzaharCityHash64(native);
    const auto pack = Root / "hud";
    std::filesystem::create_directories(pack);
    {
        std::ofstream output(pack / "pack.json");
        output << nlohmann::json{{"options", {{"skip_mipmap", true}, {"flip_png_files", true}, {"use_new_hash", true}}}};
    }
    // A 64x64 image (8x) for an 8x8 native HUD texture.
    std::vector<uint8_t> large(64U * 64U * 4U, 255U);
    WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(8U, 8U, hash, 0U, 0U), 64U, 64U, large);
    AzaharTextureRequest hud{8U, 8U, 0U, 0U, native, {}};
    hud.UserInterface = true;
    const AzaharTextureRequest world{8U, 8U, 0U, 0U, native, {}};

    // "HD texture size" 2x with mipmaps for world textures.
    AzaharTexturePackRuntime runtime;
    runtime.Configure({.LoadCustomTextures = true, .UserDirectory = Root, .LoadDirectory = pack,
                       .MaxReplacementScale = 2U, .GenerateMipmaps = true});
    runtime.WaitForPendingLoads();
    EXPECT_EQ(runtime.ResolveOrQueue(hud).State, AzaharTextureResolveState::Pending);
    runtime.WaitForPendingLoads();
    const auto hudImage = runtime.ResolveOrQueue(hud);
    ASSERT_EQ(hudImage.State, AzaharTextureResolveState::Ready);
    EXPECT_EQ(hudImage.Replacement->Width, 24U);
    EXPECT_EQ(hudImage.Replacement->Height, 24U);
    EXPECT_TRUE(hudImage.Replacement->Mips.empty());
    // The same hash as a world texture keeps the world cap and mipmaps.
    EXPECT_EQ(runtime.ResolveOrQueue(world).State, AzaharTextureResolveState::Pending);
    runtime.WaitForPendingLoads();
    const auto worldImage = runtime.ResolveOrQueue(world);
    ASSERT_EQ(worldImage.State, AzaharTextureResolveState::Ready);
    EXPECT_EQ(worldImage.Replacement->Width, 16U);
    EXPECT_FALSE(worldImage.Replacement->Mips.empty());
    EXPECT_EQ(runtime.ResolveOrQueue(hud).Replacement->Width, 24U);
    // A world release does not drop the HUD copy.
    runtime.ReleaseDecoded(hash, worldImage.Replacement->Generation);
    EXPECT_EQ(runtime.ResolveOrQueue(hud).State, AzaharTextureResolveState::Ready);

    // HD textures off: HUD replacements go at once, with a new generation.
    const uint64_t generation = runtime.Generation();
    runtime.Configure({.LoadCustomTextures = false, .UserDirectory = Root, .LoadDirectory = pack});
    EXPECT_NE(runtime.Generation(), generation);
    runtime.WaitForPendingLoads();
    EXPECT_EQ(runtime.ResolveOrQueue(hud).State, AzaharTextureResolveState::Disabled);
    EXPECT_EQ(runtime.Snapshot().LoadedTextures, 0U);
}

// A HUD texture seen while a scene's world images decode must not wait for
// them (test.7: the TopScreen item icons drew native for ~0.5 s at every
// start, queued behind the world PNGs of the 1080p pack).
TEST_F(Oot3dAzaharTexturePackTest, HudImagesDoNotWaitBehindWorldImages) {
    using namespace Oot3d::Renderer;
    const auto pack = Root / "busy";
    std::filesystem::create_directories(pack);
    {
        std::ofstream output(pack / "pack.json");
        output << nlohmann::json{{"options", {{"skip_mipmap", true}, {"flip_png_files", true}, {"use_new_hash", true}}}};
    }
    // Large world images (noise: slow to decode) and one small HUD image.
    constexpr size_t kWorldImages = 16U;
    constexpr uint32_t kWorldExtent = 512U;
    std::vector<std::array<uint8_t, 256>> worldNatives(kWorldImages);
    std::vector<uint8_t> noise(kWorldExtent * kWorldExtent * 4U);
    uint32_t state = 12345U;
    for (auto& value : noise) value = static_cast<uint8_t>((state = state * 1664525U + 1013904223U) >> 24U);
    for (size_t image = 0; image < kWorldImages; ++image) {
        for (size_t index = 0; index < 256U; ++index) worldNatives[image][index] = static_cast<uint8_t>(index * 3U + image);
        WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(8U, 8U, AzaharCityHash64(worldNatives[image]), 0U, 0U),
                              kWorldExtent, kWorldExtent, noise);
    }
    std::array<uint8_t, 256> hudNative{};
    for (size_t index = 0; index < hudNative.size(); ++index) hudNative[index] = static_cast<uint8_t>(index * 13U + 1U);
    WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(8U, 8U, AzaharCityHash64(hudNative), 0U, 0U), 24U, 24U,
                          std::vector<uint8_t>(24U * 24U * 4U, 200U));

    AzaharTexturePackRuntime runtime;
    runtime.Configure({.LoadCustomTextures = true, .UserDirectory = Root, .LoadDirectory = pack,
                       .MaxReplacementScale = 0U, .GenerateMipmaps = true});
    runtime.WaitForPendingLoads();
    for (const auto& native : worldNatives)
        EXPECT_EQ(runtime.ResolveOrQueue({8U, 8U, 0U, 0U, native, {}}).State, AzaharTextureResolveState::Pending);
    AzaharTextureRequest hud{8U, 8U, 0U, 0U, hudNative, {}};
    hud.UserInterface = true;
    EXPECT_EQ(runtime.ResolveOrQueue(hud).State, AzaharTextureResolveState::Pending);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (runtime.ResolveOrQueue(hud).State == AzaharTextureResolveState::Pending &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(runtime.ResolveOrQueue(hud).State, AzaharTextureResolveState::Ready);
    // Most world images are still decoding when the HUD image is ready.
    size_t worldPending = 0U;
    for (const auto& native : worldNatives)
        worldPending += runtime.PollQueued(AzaharCityHash64(native)).State == AzaharTextureResolveState::Pending;
    EXPECT_GE(worldPending, kWorldImages / 2U);
    const auto status = runtime.HudStatus();
    EXPECT_EQ(status.OnSightDecodes, 1U);
    EXPECT_EQ(status.PreloadQueued, 0U);
    runtime.WaitForPendingLoads();
}

// The HUD images of a session are listed and decoded at the next start
// before the HUD asks for them; lines of another pack are dropped.
TEST_F(Oot3dAzaharTexturePackTest, HudImagesSeenOnceAreDecodedAtTheNextStart) {
    using namespace Oot3d::Renderer;
    const auto pack = Root / "hudlist";
    std::filesystem::create_directories(pack);
    {
        std::ofstream output(pack / "pack.json");
        output << nlohmann::json{{"options", {{"skip_mipmap", true}, {"flip_png_files", true}, {"use_new_hash", true}}}};
    }
    std::array<uint8_t, 512> native{};
    for (size_t index = 0; index < native.size(); ++index) native[index] = static_cast<uint8_t>(index * 5U + 3U);
    const uint64_t hash = AzaharCityHash64(native);
    // 16x8 native, 8x in the pack: 48x24 at the HUD's 3x cap.
    WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(16U, 8U, hash, 0U, 0U), 128U, 64U,
                          std::vector<uint8_t>(128U * 64U * 4U, 90U));
    AzaharTextureRequest hud{16U, 8U, 0U, 0U, native, {}};
    hud.UserInterface = true;
    const auto list = Root / "texture_pack_hud";
    const AzaharTexturePackConfiguration configuration{.LoadCustomTextures = true, .UserDirectory = Root,
                                                       .LoadDirectory = pack, .MaxReplacementScale = 2U,
                                                       .GenerateMipmaps = true, .HudImageList = list};
    const auto readList = [&] {
        std::ifstream file(list, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    };
    {
        // First session: no list; the HUD image decodes on first sight and is listed.
        AzaharTexturePackRuntime runtime;
        runtime.Configure(configuration);
        runtime.WaitForPendingLoads();
        EXPECT_EQ(runtime.ResolveOrQueue(hud).State, AzaharTextureResolveState::Pending);
        runtime.WaitForPendingLoads();
        EXPECT_EQ(runtime.ResolveOrQueue(hud).State, AzaharTextureResolveState::Ready);
        EXPECT_EQ(readList(), FormatHudImageEntry({hash, 16U, 8U}));
        EXPECT_EQ(runtime.HudStatus().OnSightDecodes, 1U);
        // Asked again (another scene): no second line.
        (void)runtime.ResolveOrQueue(hud);
        runtime.WaitForPendingLoads();
        EXPECT_EQ(readList(), FormatHudImageEntry({hash, 16U, 8U}));
    }
    // A line of another pack in the list.
    std::ofstream(list, std::ios::binary | std::ios::app) << FormatHudImageEntry({0x0123456789ABCDEFULL, 8U, 8U});
    {
        // Next start: decoded with the index, ready on the HUD's first request,
        // at the HUD size (3x of the listed native size).
        AzaharTexturePackRuntime runtime;
        runtime.Configure(configuration);
        runtime.WaitForPendingLoads();
        const auto first = runtime.ResolveOrQueue(hud);
        ASSERT_EQ(first.State, AzaharTextureResolveState::Ready);
        EXPECT_EQ(first.Replacement->Width, 48U);
        EXPECT_EQ(first.Replacement->Height, 24U);
        EXPECT_TRUE(first.Replacement->Mips.empty());
        const auto status = runtime.HudStatus();
        EXPECT_EQ(status.Listed, 1U);
        EXPECT_EQ(status.PreloadQueued, 1U);
        EXPECT_EQ(status.PreloadDecoded, 1U);
        EXPECT_EQ(status.PreloadBytes, 48U * 24U * 4U);
        EXPECT_EQ(status.OnSightDecodes, 0U);
        EXPECT_EQ(readList(), FormatHudImageEntry({hash, 16U, 8U}));
        // The world texture of the same hash is not preloaded.
        EXPECT_EQ(runtime.PollQueued(hash).State, AzaharTextureResolveState::Missing);
    }
    {
        // HD textures off: nothing decoded, the list kept for later.
        auto off = configuration;
        off.LoadCustomTextures = false;
        AzaharTexturePackRuntime runtime;
        runtime.Configure(off);
        runtime.WaitForPendingLoads();
        EXPECT_EQ(runtime.HudStatus().PreloadQueued, 0U);
        EXPECT_EQ(readList(), FormatHudImageEntry({hash, 16U, 8U}));
    }
}

// First start, empty HUD list (test.8: `listed=2 preloaded=0/0; first
// sight=2 (wait max 263 ms)`): the UI asks for the HUD's textures ahead
// (title screen), so the HUD's first request finds its image ready; the image
// is listed for the next start. A first sight while the ahead decode still
// runs counts as a late one.
TEST_F(Oot3dAzaharTexturePackTest, HudImagesAskedAheadAreReadyAtFirstSight) {
    using namespace Oot3d::Renderer;
    const auto pack = Root / "hudahead";
    std::filesystem::create_directories(pack);
    {
        std::ofstream output(pack / "pack.json");
        output << nlohmann::json{{"options", {{"skip_mipmap", true}, {"flip_png_files", true}, {"use_new_hash", true}}}};
    }
    std::array<uint8_t, 512> native{};
    for (size_t index = 0; index < native.size(); ++index) native[index] = static_cast<uint8_t>(index * 7U + 1U);
    const uint64_t hash = AzaharCityHash64(native);
    WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(16U, 8U, hash, 0U, 0U), 128U, 64U,
                          std::vector<uint8_t>(128U * 64U * 4U, 70U));
    // A second HUD image, large noise (slow to decode), for the late case.
    std::array<uint8_t, 512> slowNative{};
    for (size_t index = 0; index < slowNative.size(); ++index) slowNative[index] = static_cast<uint8_t>(index * 11U + 5U);
    std::vector<uint8_t> noise(2048U * 1024U * 4U);
    uint32_t state = 777U;
    for (auto& value : noise) value = static_cast<uint8_t>((state = state * 1664525U + 1013904223U) >> 24U);
    WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(16U, 8U, AzaharCityHash64(slowNative), 0U, 0U), 2048U,
                          1024U, noise);
    const auto list = Root / "texture_pack_hud";
    std::filesystem::remove(list);
    AzaharTexturePackRuntime runtime;
    runtime.Configure({.LoadCustomTextures = true, .UserDirectory = Root, .LoadDirectory = pack,
                       .MaxReplacementScale = 2U, .GenerateMipmaps = true, .HudImageList = list});
    runtime.WaitForPendingLoads();
    EXPECT_EQ(runtime.HudStatus().PreloadQueued, 0U);

    AzaharTextureRequest ahead{16U, 8U, 0U, 0U, native, {}};
    ahead.UserInterface = true;
    ahead.Ahead = true;
    EXPECT_EQ(runtime.ResolveOrQueue(ahead).State, AzaharTextureResolveState::Pending);
    runtime.WaitForPendingLoads();
    // The HUD's first frame.
    AzaharTextureRequest hud = ahead;
    hud.Ahead = false;
    const auto first = runtime.ResolveOrQueue(hud);
    ASSERT_EQ(first.State, AzaharTextureResolveState::Ready);
    EXPECT_EQ(first.Replacement->Width, 48U);
    EXPECT_TRUE(first.Replacement->Mips.empty());
    auto status = runtime.HudStatus();
    EXPECT_EQ(status.PreloadQueued, 1U);
    EXPECT_EQ(status.PreloadDecoded, 1U);
    EXPECT_EQ(status.PreloadBytes, 48U * 24U * 4U);
    EXPECT_EQ(status.OnSightDecodes, 0U);
    EXPECT_EQ(status.Listed, 1U);
    {
        std::ifstream file(list, std::ios::binary);
        EXPECT_EQ(std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()),
                  FormatHudImageEntry({hash, 16U, 8U}));
    }
    // Asked ahead again (the next revision): nothing more to decode.
    EXPECT_EQ(runtime.ResolveOrQueue(ahead).State, AzaharTextureResolveState::Pending);
    runtime.WaitForPendingLoads();
    EXPECT_EQ(runtime.HudStatus().PreloadQueued, 1U);

    // Ahead, but the HUD comes before the image is ready: a late first sight.
    AzaharTextureRequest slowAhead{16U, 8U, 0U, 0U, slowNative, {}};
    slowAhead.UserInterface = true;
    slowAhead.Ahead = true;
    (void)runtime.ResolveOrQueue(slowAhead);
    AzaharTextureRequest slowHud = slowAhead;
    slowHud.Ahead = false;
    const bool late = runtime.ResolveOrQueue(slowHud).State == AzaharTextureResolveState::Pending;
    runtime.WaitForPendingLoads();
    EXPECT_EQ(runtime.ResolveOrQueue(slowHud).State, AzaharTextureResolveState::Ready);
    status = runtime.HudStatus();
    EXPECT_EQ(status.PreloadDecoded, status.PreloadQueued);
    EXPECT_EQ(status.OnSightDecodes, late ? 1U : 0U);
    EXPECT_TRUE(late);
}

// An ahead request carries only the encoded bytes; for a legacy-hash pack
// (Henriko's 1080p pack) the HUD worker decodes them to hash, so the image
// found is the one the HUD's own request (with its decoded texels) asks for.
TEST_F(Oot3dAzaharTexturePackTest, HudImagesAskedAheadMatchALegacyHashPack) {
    using namespace Oot3d::Renderer;
    const auto pack = Root / "hudlegacy";
    std::filesystem::create_directories(pack);
    {
        std::ofstream output(pack / "pack.json");
        output << nlohmann::json{{"options", {{"skip_mipmap", true}, {"flip_png_files", true}, {"use_new_hash", false}}}};
    }
    // An 8x8 ETC1A4 texture (format 13, the item icon atlas's): 4 blocks.
    std::array<uint8_t, 64> native{};
    for (size_t index = 0; index < native.size(); ++index) native[index] = static_cast<uint8_t>(index * 29U + 7U);
    std::vector<uint8_t> rgba8;
    ASSERT_TRUE(DecodePicaTextureRgba8(13U, 8U, 8U, native, rgba8));
    const AzaharTextureRequest decoded{8U, 8U, 13U, 0U, native, rgba8};
    const uint64_t legacy = ComputeAzaharTextureHash(decoded, false);
    ASSERT_NE(legacy, 0U);
    ASSERT_NE(legacy, ComputeAzaharTextureHash(decoded, true));
    WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(8U, 8U, legacy, 13U, 0U), 32U, 32U,
                          std::vector<uint8_t>(32U * 32U * 4U, 120U));
    AzaharTexturePackRuntime runtime;
    runtime.Configure({.LoadCustomTextures = true, .UserDirectory = Root, .LoadDirectory = pack,
                       .MaxReplacementScale = 2U, .GenerateMipmaps = true});
    runtime.WaitForPendingLoads();
    AzaharTextureRequest ahead{8U, 8U, 13U, 0U, native, {}};
    ahead.UserInterface = true;
    ahead.Ahead = true;
    EXPECT_EQ(runtime.ResolveOrQueue(ahead).State, AzaharTextureResolveState::Pending);
    runtime.WaitForPendingLoads();
    AzaharTextureRequest hud = decoded;
    hud.UserInterface = true;
    const auto first = runtime.ResolveOrQueue(hud);
    ASSERT_EQ(first.State, AzaharTextureResolveState::Ready);
    EXPECT_EQ(first.NativeHash, legacy);
    EXPECT_EQ(first.Replacement->Width, 24U);
    const auto status = runtime.HudStatus();
    EXPECT_EQ(status.PreloadQueued, 1U);
    EXPECT_EQ(status.PreloadDecoded, 1U);
    EXPECT_EQ(status.OnSightDecodes, 0U);
}

// The pause pages drawn through PICA (deko3d, config/texture_pack_menu) ask
// for their pack images as HUD images (UserInterface + Menu): a texture the
// HUD already asked for ahead (menu_item_parts00, whose tabs swapped from
// "GEAR MAP ITEMS" to the pack's art on every page in logs17's video) is
// ready at the page's first draw; the image stays decoded after the
// renderer's upload (a page at another guest address finds it at once); a
// menu texture seen first is decoded on the HUD worker, counted apart from the
// HUD's own first sights and listed for the next start.
TEST_F(Oot3dAzaharTexturePackTest, MenuTexturesShareTheHudImagesAndStayDecoded) {
    using namespace Oot3d::Renderer;
    const auto pack = Root / "menu";
    std::filesystem::create_directories(pack);
    {
        std::ofstream output(pack / "pack.json");
        output << nlohmann::json{{"options", {{"skip_mipmap", true}, {"flip_png_files", true}, {"use_new_hash", false}}}};
    }
    // Two 8x8 ETC1A4 textures (format 13, menu_item_parts00's), legacy hash as Henriko's pack.
    const auto texture = [](uint8_t seed) {
        std::array<uint8_t, 64> native{};
        for (size_t index = 0; index < native.size(); ++index) native[index] = static_cast<uint8_t>(index * 29U + seed);
        return native;
    };
    const auto tabs = texture(7U), page = texture(91U);
    std::vector<uint8_t> tabsRgba, pageRgba;
    ASSERT_TRUE(DecodePicaTextureRgba8(13U, 8U, 8U, tabs, tabsRgba));
    ASSERT_TRUE(DecodePicaTextureRgba8(13U, 8U, 8U, page, pageRgba));
    const uint64_t tabsHash = ComputeAzaharTextureHash({8U, 8U, 13U, 0U, tabs, tabsRgba}, false);
    const uint64_t pageHash = ComputeAzaharTextureHash({8U, 8U, 13U, 0U, page, pageRgba}, false);
    ASSERT_NE(tabsHash, pageHash);
    WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(8U, 8U, tabsHash, 13U, 0U), 32U, 32U,
                          std::vector<uint8_t>(32U * 32U * 4U, 120U));
    WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(8U, 8U, pageHash, 13U, 0U), 32U, 32U,
                          std::vector<uint8_t>(32U * 32U * 4U, 60U));
    const auto list = Root / "texture_pack_hud";
    AzaharTexturePackRuntime runtime;
    runtime.Configure({.LoadCustomTextures = true, .UserDirectory = Root, .LoadDirectory = pack,
                       .MaxReplacementScale = 2U, .GenerateMipmaps = true, .HudImageList = list});
    runtime.WaitForPendingLoads();
    // The HUD asks for the tabs' texture ahead (title screen).
    AzaharTextureRequest ahead{8U, 8U, 13U, 0U, tabs, {}};
    ahead.UserInterface = true;
    ahead.Ahead = true;
    (void)runtime.ResolveOrQueue(ahead);
    runtime.WaitForPendingLoads();

    // The Items page's first draw (PICA, decoded texels as deko3d passes them).
    AzaharTextureRequest tabsMenu{8U, 8U, 13U, 0U, tabs, tabsRgba};
    tabsMenu.UserInterface = true;
    tabsMenu.Menu = true;
    const auto first = runtime.ResolveOrQueue(tabsMenu);
    ASSERT_EQ(first.State, AzaharTextureResolveState::Ready);
    EXPECT_EQ(first.Replacement->Width, 24U); // the HUD's 3x cap, not the world's 2x
    EXPECT_TRUE(first.Replacement->Mips.empty());
    // Uploaded by the renderer; a world release does not drop it. The Map page
    // (same texture at another guest address) finds it again at once.
    runtime.ReleaseDecoded(tabsHash, first.Replacement->Generation);
    EXPECT_EQ(runtime.PollOrQueue(tabsHash, 8U, 8U, true, true).State, AzaharTextureResolveState::Ready);
    EXPECT_EQ(runtime.ResolveOrQueue(tabsMenu).State, AzaharTextureResolveState::Ready);
    // The world path still has its own cache (another cap, mips).
    EXPECT_EQ(runtime.PollQueued(tabsHash).State, AzaharTextureResolveState::Missing);

    // A page texture the HUD never asked for: drawn native first, decoded on
    // the HUD worker, counted as a menu first sight, listed.
    AzaharTextureRequest pageMenu{8U, 8U, 13U, 0U, page, pageRgba};
    pageMenu.UserInterface = true;
    pageMenu.Menu = true;
    EXPECT_EQ(runtime.ResolveOrQueue(pageMenu).State, AzaharTextureResolveState::Pending);
    runtime.WaitForPendingLoads();
    EXPECT_EQ(runtime.PollOrQueue(pageHash, 8U, 8U, true, true).State, AzaharTextureResolveState::Ready);
    const auto status = runtime.HudStatus();
    EXPECT_EQ(status.OnSightDecodes, 0U);
    EXPECT_EQ(status.MenuOnSightDecodes, 1U);
    EXPECT_EQ(status.Listed, 2U);
    std::ifstream file(list, std::ios::binary);
    EXPECT_EQ(std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()),
              FormatHudImageEntry({tabsHash, 8U, 8U}) + FormatHudImageEntry({pageHash, 8U, 8U}));
}

// A texture on screen (HUD or menu) does not wait behind the HUD images
// decoded ahead: with a long HUD list (the menu textures of earlier sessions),
// a texture asked for at its draw goes to the front of the HUD worker's queue,
// also when it was itself queued ahead further back.
TEST_F(Oot3dAzaharTexturePackTest, TexturesOnScreenGoBeforeHudImagesAskedAhead) {
    using namespace Oot3d::Renderer;
    const auto pack = Root / "front";
    std::filesystem::create_directories(pack);
    {
        std::ofstream output(pack / "pack.json");
        output << nlohmann::json{{"options", {{"skip_mipmap", true}, {"flip_png_files", true}, {"use_new_hash", true}}}};
    }
    constexpr size_t kSlowImages = 8U;
    std::vector<uint8_t> noise(2048U * 1024U * 4U);
    uint32_t state = 4242U;
    for (auto& value : noise) value = static_cast<uint8_t>((state = state * 1664525U + 1013904223U) >> 24U);
    std::string listText;
    for (size_t image = 0; image < kSlowImages; ++image) {
        std::array<uint8_t, 512> native{};
        for (size_t index = 0; index < native.size(); ++index)
            native[index] = static_cast<uint8_t>(index * 3U + image * 17U + 1U);
        const uint64_t hash = AzaharCityHash64(native);
        WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(16U, 8U, hash, 0U, 0U), 2048U, 1024U, noise);
        listText += FormatHudImageEntry({hash, 16U, 8U});
    }
    std::array<uint8_t, 512> onScreen{};
    for (size_t index = 0; index < onScreen.size(); ++index) onScreen[index] = static_cast<uint8_t>(index * 9U + 4U);
    const uint64_t onScreenHash = AzaharCityHash64(onScreen);
    WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(16U, 8U, onScreenHash, 0U, 0U), 48U, 24U,
                          std::vector<uint8_t>(48U * 24U * 4U, 33U));
    listText += FormatHudImageEntry({onScreenHash, 16U, 8U}); // last in the list
    const auto list = Root / "texture_pack_hud";
    std::ofstream(list, std::ios::binary) << listText;

    AzaharTexturePackRuntime runtime;
    runtime.Configure({.LoadCustomTextures = true, .UserDirectory = Root, .LoadDirectory = pack,
                       .MaxReplacementScale = 2U, .GenerateMipmaps = true, .HudImageList = list});
    AzaharTextureRequest menu{16U, 8U, 0U, 0U, onScreen, {}};
    menu.UserInterface = true;
    menu.Menu = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    // Until the index is ready the request has no hash.
    while (!runtime.ResolveOrQueue(menu).NativeHashAvailable && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (runtime.ResolveOrQueue(menu).State == AzaharTextureResolveState::Pending &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_EQ(runtime.ResolveOrQueue(menu).State, AzaharTextureResolveState::Ready);
    // Most of the slow images ahead of it in the list are still decoding.
    EXPECT_LE(runtime.HudStatus().PreloadDecoded, kSlowImages / 2U + 1U);
    runtime.WaitForPendingLoads();
    const auto status = runtime.HudStatus();
    EXPECT_EQ(status.PreloadQueued, kSlowImages + 1U);
    EXPECT_EQ(status.PreloadDecoded, kSlowImages + 1U);
    EXPECT_EQ(status.OnSightDecodes, 0U);
}

// A world image a scene's draws ask for is listed for the scene; entering the
// scene in a later session decodes its listed images before its first draw
// asks (test.10: "right after loading into an area, textures pop in").
// Images decoded ahead and never asked for go at the next scene change.
TEST_F(Oot3dAzaharTexturePackTest, SceneImagesSeenOnceAreDecodedWhenTheSceneStarts) {
    using namespace Oot3d::Renderer;
    const auto pack = Root / "scenes";
    std::filesystem::create_directories(pack);
    {
        std::ofstream output(pack / "pack.json");
        output << nlohmann::json{{"options", {{"skip_mipmap", true}, {"flip_png_files", true}, {"use_new_hash", true}}}};
    }
    std::array<std::array<uint8_t, 256>, 3> natives{};
    for (size_t image = 0; image < natives.size(); ++image) {
        for (size_t index = 0; index < 256U; ++index) natives[image][index] = static_cast<uint8_t>(index * 7U + image * 31U);
        WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(8U, 8U, AzaharCityHash64(natives[image]), 0U, 0U), 16U,
                              16U, std::vector<uint8_t>(16U * 16U * 4U, static_cast<uint8_t>(40U + image)));
    }
    const auto hash = [&](size_t image) { return AzaharCityHash64(natives[image]); };
    const auto request = [&](size_t image) { return AzaharTextureRequest{8U, 8U, 0U, 0U, natives[image], {}}; };
    const auto list = Root / "texture_pack_scenes";
    const AzaharTexturePackConfiguration configuration{.LoadCustomTextures = true, .UserDirectory = Root,
                                                       .LoadDirectory = pack, .MaxReplacementScale = 2U,
                                                       .GenerateMipmaps = true, .SceneImageList = list};
    const auto readList = [&] {
        std::ifstream file(list, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    };
    {
        // First session: no list. Hyrule Field (0x51) draws images 0 and 1,
        // Kokiri Forest (0x55) image 2 and image 0 again.
        AzaharTexturePackRuntime runtime;
        runtime.Configure(configuration);
        runtime.WaitForPendingLoads();
        runtime.NoteScene(0x51);
        EXPECT_EQ(runtime.ResolveOrQueue(request(0)).State, AzaharTextureResolveState::Pending);
        EXPECT_EQ(runtime.ResolveOrQueue(request(1)).State, AzaharTextureResolveState::Pending);
        runtime.WaitForPendingLoads();
        EXPECT_EQ(runtime.ResolveOrQueue(request(0)).State, AzaharTextureResolveState::Ready);
        EXPECT_EQ(runtime.SceneStatus().OnSight, 2U);
        EXPECT_EQ(runtime.SceneStatus().Added, 2U);
        runtime.NoteScene(0x55);
        (void)runtime.ResolveOrQueue(request(2));
        (void)runtime.ResolveOrQueue(request(0));
        runtime.WaitForPendingLoads();
        EXPECT_EQ(runtime.PreviousSceneStatus().Scene, 0x51);
        EXPECT_EQ(runtime.PreviousSceneStatus().Added, 2U);
        // Outside gameplay nothing is listed.
        runtime.NoteScene(-1);
        (void)runtime.ResolveOrQueue(request(1));
        runtime.WaitForPendingLoads();
        EXPECT_EQ(readList(), FormatSceneImageEntry({0x51U, {hash(0), 8U, 8U}}) +
                                  FormatSceneImageEntry({0x51U, {hash(1), 8U, 8U}}) +
                                  FormatSceneImageEntry({0x55U, {hash(2), 8U, 8U}}) +
                                  FormatSceneImageEntry({0x55U, {hash(0), 8U, 8U}}));
    }
    // A line of another pack.
    std::ofstream(list, std::ios::binary | std::ios::app) << FormatSceneImageEntry({0x51U, {0x0123456789ABCDEFULL, 8U, 8U}});
    {
        // Next session: the scene starts before the index is ready (title
        // screen save); its listed images decode once it is.
        AzaharTexturePackRuntime runtime;
        runtime.Configure(configuration);
        runtime.NoteScene(0x51);
        runtime.WaitForPendingLoads();
        auto status = runtime.SceneStatus();
        EXPECT_EQ(status.Listed, 2U);
        EXPECT_EQ(status.PreloadQueued, 2U);
        EXPECT_EQ(status.PreloadDecoded, 2U);
        EXPECT_GT(status.PreloadBytes, 0U);
        // The first draw finds its image: no native first.
        const auto first = runtime.ResolveOrQueue(request(0));
        ASSERT_EQ(first.State, AzaharTextureResolveState::Ready);
        EXPECT_EQ(first.Replacement->Width, 16U);
        runtime.ReleaseDecoded(hash(0), first.Replacement->Generation); // uploaded
        status = runtime.SceneStatus();
        EXPECT_EQ(status.PreloadUsed, 1U);
        EXPECT_EQ(status.OnSight, 0U);
        EXPECT_EQ(status.Added, 0U);
        // Image 1 is never drawn: it goes at the change, counted unused.
        runtime.NoteScene(0x55);
        EXPECT_EQ(runtime.PreviousSceneStatus().PreloadUnused, 1U);
        EXPECT_EQ(runtime.PollQueued(hash(1)).State, AzaharTextureResolveState::Missing);
        runtime.WaitForPendingLoads();
        // Kokiri Forest's image 0 is still on the GPU (drawn in the field):
        // its copy decoded ahead is released at once.
        runtime.NoteSceneImage(hash(0), 8U, 8U);
        status = runtime.SceneStatus();
        EXPECT_EQ(status.PreloadQueued, 2U);
        EXPECT_EQ(status.PreloadResident, 1U);
        EXPECT_EQ(runtime.PollQueued(hash(0)).State, AzaharTextureResolveState::Missing);
        EXPECT_EQ(runtime.ResolveOrQueue(request(2)).State, AzaharTextureResolveState::Ready);
        // The other pack's line was dropped from the file.
        EXPECT_EQ(readList().find("0123456789ABCDEF"), std::string::npos);
    }
    {
        // config/texture_pack_preload = 0: no list read, nothing listed.
        auto off = configuration;
        off.SceneImageList.clear();
        AzaharTexturePackRuntime runtime;
        runtime.Configure(off);
        runtime.NoteScene(0x51);
        runtime.WaitForPendingLoads();
        EXPECT_EQ(runtime.SceneStatus().PreloadQueued, 0U);
        EXPECT_EQ(runtime.ResolveOrQueue(request(0)).State, AzaharTextureResolveState::Pending);
        runtime.WaitForPendingLoads();
        EXPECT_EQ(runtime.SceneStatus().Added, 0U);
    }
}

// Leaving a scene drops its world decodes still queued (logs18: the pop-ins
// of a new scene waited behind the old one's backlog); an image a draw of the
// new scene still needs is queued again. The renderer's hold moves the
// images on screen to the front and waits for them only.
TEST_F(Oot3dAzaharTexturePackTest, SceneChangesDropTheBacklogAndTheHoldWaitsForImagesOnScreen) {
    using namespace Oot3d::Renderer;
    const auto pack = Root / "backlog";
    std::filesystem::create_directories(pack);
    {
        std::ofstream output(pack / "pack.json");
        output << nlohmann::json{{"options", {{"skip_mipmap", true}, {"flip_png_files", true}, {"use_new_hash", true}}}};
    }
    constexpr size_t kSlowImages = 12U;
    std::vector<uint8_t> noise(1024U * 1024U * 4U);
    uint32_t state = 777U;
    for (auto& value : noise) value = static_cast<uint8_t>((state = state * 1664525U + 1013904223U) >> 24U);
    std::vector<std::array<uint8_t, 256>> slow(kSlowImages);
    for (size_t image = 0; image < kSlowImages; ++image) {
        for (size_t index = 0; index < 256U; ++index) slow[image][index] = static_cast<uint8_t>(index * 5U + image * 11U);
        WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(8U, 8U, AzaharCityHash64(slow[image]), 0U, 0U), 1024U,
                              1024U, noise);
    }
    std::array<uint8_t, 256> onScreen{};
    for (size_t index = 0; index < onScreen.size(); ++index) onScreen[index] = static_cast<uint8_t>(index * 3U + 200U);
    const uint64_t onScreenHash = AzaharCityHash64(onScreen);
    WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(8U, 8U, onScreenHash, 0U, 0U), 16U, 16U,
                          std::vector<uint8_t>(16U * 16U * 4U, 9U));

    AzaharTexturePackRuntime runtime;
    runtime.Configure({.LoadCustomTextures = true, .UserDirectory = Root, .LoadDirectory = pack,
                       .MaxReplacementScale = 0U, .GenerateMipmaps = true});
    runtime.WaitForPendingLoads();
    runtime.NoteScene(0x51);
    for (const auto& native : slow) (void)runtime.ResolveOrQueue({8U, 8U, 0U, 0U, native, {}});
    runtime.NoteScene(0x55);
    const auto left = runtime.PreviousSceneStatus();
    // At most the images the world workers had started are not dropped.
    EXPECT_GE(left.Dropped, kSlowImages - kWorldDecodeWorkers);
    size_t dropped = 0U;
    for (const auto& native : slow)
        dropped += runtime.PollQueued(AzaharCityHash64(native)).State == AzaharTextureResolveState::Missing;
    EXPECT_GE(dropped, kSlowImages - kWorldDecodeWorkers);
    // The new scene draws them again behind a texture on screen.
    for (const auto& native : slow) (void)runtime.ResolveOrQueue({8U, 8U, 0U, 0U, native, {}});
    EXPECT_EQ(runtime.ResolveOrQueue({8U, 8U, 0U, 0U, onScreen, {}}).State, AzaharTextureResolveState::Pending);
    const std::array<uint64_t, 1> hold{onScreenHash};
    EXPECT_EQ(runtime.WaitForImages(hold, std::chrono::seconds(60)), 0U);
    EXPECT_EQ(runtime.PollQueued(onScreenHash).State, AzaharTextureResolveState::Ready);
    size_t stillDecoding = 0U;
    for (const auto& native : slow)
        stillDecoding += runtime.PollQueued(AzaharCityHash64(native)).State == AzaharTextureResolveState::Pending;
    EXPECT_GE(stillDecoding, kSlowImages / 2U);
    // A wait that runs out reports what is left.
    std::vector<uint64_t> all;
    for (const auto& native : slow) all.push_back(AzaharCityHash64(native));
    EXPECT_GT(runtime.WaitForImages(all, std::chrono::microseconds(0)), 0U);
    runtime.WaitForPendingLoads();
    EXPECT_EQ(runtime.WaitForImages(all, std::chrono::microseconds(0)), 0U);
}

#if !defined(_WIN32)
// HD textures turned on (or another size) in the Video tab index the pack
// again; textures drawn meanwhile get no hash. Until 08/11 the renderer polled
// them with that empty hash, the new index answered Missing, and they stayed
// native until evicted (texture_pack_reresolve). Here pack.json is a FIFO: the
// index waits until the test writes it, so "meanwhile" is deterministic.
TEST_F(Oot3dAzaharTexturePackTest, TexturesDrawnWhileThePackIndexesResolveOnceItIsReady) {
    using namespace Oot3d::Renderer;
    std::array<uint8_t, 256> native{};
    for (size_t index = 0; index < native.size(); ++index) native[index] = static_cast<uint8_t>(index * 5U + 1U);
    const uint64_t hash = AzaharCityHash64(native);
    const auto pack = Root / "indexing";
    std::filesystem::create_directories(pack);
    // A 32x32 image (4x) for an 8x8 native texture.
    WriteAzaharTexturePng(pack / MakeAzaharTextureFilename(8U, 8U, hash, 0U, 0U), 32U, 32U,
                          std::vector<uint8_t>(32U * 32U * 4U, 200U));
    const auto packJson = pack / "pack.json";
    ASSERT_EQ(mkfifo(packJson.c_str(), 0600), 0);
    // Writes pack.json once the index worker waits in it (a non-blocking open
    // fails until the FIFO has a reader). Should it never come, a plain file
    // takes its place so that the worker cannot block for good.
    const auto feedPackJson = [&] {
        const std::string json =
            nlohmann::json{{"options", {{"flip_png_files", true}, {"use_new_hash", true}}}}.dump();
        int fifo = -1;
        for (int attempt = 0; attempt < 2000 && fifo < 0; ++attempt) {
            fifo = open(packJson.c_str(), O_WRONLY | O_NONBLOCK);
            if (fifo < 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (fifo < 0) {
            std::filesystem::remove(packJson);
            std::ofstream(packJson) << json;
            return false;
        }
        const bool written = write(fifo, json.data(), json.size()) == static_cast<ssize_t>(json.size());
        close(fifo);
        return written;
    };
    const AzaharTextureRequest request{8U, 8U, 0U, 0U, native, {}};
    constexpr uint32_t kEpoch = 2U; // the renderer's epoch: one change, no other

    AzaharTexturePackRuntime runtime;
    runtime.Configure({.LoadCustomTextures = true, .UserDirectory = Root, .LoadDirectory = pack,
                       .MaxReplacementScale = 2U, .GenerateMipmaps = true});
    // Drawn while the worker waits in pack.json: no hash, drawn native.
    EXPECT_FALSE(runtime.IndexReady());
    const auto drawn = runtime.ResolveOrQueue(request);
    EXPECT_EQ(drawn.State, AzaharTextureResolveState::Pending);
    EXPECT_FALSE(drawn.NativeHashAvailable);
    EXPECT_EQ(drawn.NativeHash, 0U);
    EXPECT_EQ(TexturePackEntryActionFor(false, kEpoch, kEpoch, true, true, runtime.IndexReady()),
              TexturePackEntryAction::Keep);

    ASSERT_TRUE(feedPackJson());
    runtime.WaitForPendingLoads();
    ASSERT_TRUE(runtime.IndexReady());
    // Polled with the empty hash it never finds its image (the path before 08/11)...
    EXPECT_EQ(runtime.PollOrQueue(drawn.NativeHash, 8U, 8U).State, AzaharTextureResolveState::Missing);
    // ...so the renderer makes it again, and that resolve has the hash and the image.
    EXPECT_EQ(TexturePackEntryActionFor(false, kEpoch, kEpoch, true, true, runtime.IndexReady()),
              TexturePackEntryAction::Rebuild);
    const auto again = runtime.ResolveOrQueue(request);
    EXPECT_TRUE(again.NativeHashAvailable);
    EXPECT_EQ(again.NativeHash, hash);
    EXPECT_EQ(again.State, AzaharTextureResolveState::Pending);
    runtime.WaitForPendingLoads();
    const auto capped = runtime.PollOrQueue(hash, 8U, 8U);
    ASSERT_EQ(capped.State, AzaharTextureResolveState::Ready);
    EXPECT_EQ(capped.Replacement->Width, 16U); // the 2x cap

    // "HD texture size" No limit: indexed again, the same in between, and the
    // second resolve decodes the file at its own size.
    runtime.Configure({.LoadCustomTextures = true, .UserDirectory = Root, .LoadDirectory = pack,
                       .MaxReplacementScale = 0U, .GenerateMipmaps = true});
    EXPECT_FALSE(runtime.IndexReady());
    const auto meanwhile = runtime.ResolveOrQueue(request);
    EXPECT_EQ(meanwhile.State, AzaharTextureResolveState::Pending);
    EXPECT_FALSE(meanwhile.NativeHashAvailable);
    ASSERT_TRUE(feedPackJson());
    runtime.WaitForPendingLoads();
    ASSERT_TRUE(runtime.IndexReady());
    EXPECT_EQ(runtime.ResolveOrQueue(request).State, AzaharTextureResolveState::Pending);
    runtime.WaitForPendingLoads();
    const auto full = runtime.PollOrQueue(hash, 8U, 8U);
    ASSERT_EQ(full.State, AzaharTextureResolveState::Ready);
    EXPECT_EQ(full.Replacement->Width, 32U); // the file's own size, never enlarged

    // HD textures off, the index waiting in pack.json again: a HUD texture
    // gets its own pixels at once (test.13: Pending until the index, and the
    // UI polled it meanwhile), the hash for authoring once indexed.
    runtime.Configure({.LoadCustomTextures = false, .UserDirectory = Root, .LoadDirectory = pack});
    EXPECT_FALSE(runtime.IndexReady());
    const AzaharTextureRequest hud{8U, 8U, 0U, 0U, native, {}, true};
    EXPECT_EQ(runtime.ResolveOrQueue(hud).State, AzaharTextureResolveState::Disabled);
    EXPECT_EQ(runtime.ResolveOrQueue(request).State, AzaharTextureResolveState::Disabled);
    ASSERT_TRUE(feedPackJson());
    runtime.WaitForPendingLoads();
    const auto indexed = runtime.ResolveOrQueue(request);
    EXPECT_EQ(indexed.State, AzaharTextureResolveState::Disabled);
    EXPECT_TRUE(indexed.NativeHashAvailable);
    EXPECT_EQ(indexed.NativeHash, hash);
}
#endif

// L3+R3 > Video offers the sizes the pack has: once the index is ready, the
// pack worker measures the largest world/model image from the PNG headers (HUD
// art in UI left out, names without a native size skipped), remembers it with
// the pack's fingerprint, keeps it through a size cap change, and measures
// again when the pack changed.
TEST_F(Oot3dAzaharTexturePackTest, MeasuresThePacksLargestImageScaleAndRemembersIt) {
    using namespace Oot3d::Renderer;
    using namespace std::chrono_literals;
    const auto pack = Root / "sizes";
    const auto record = Root / "config" / kTexturePackSizesName;
    std::filesystem::create_directories(pack / "ENVIRONMENT");
    std::filesystem::create_directories(pack / "UI");
    std::filesystem::create_directories(Root / "config");
    {
        std::ofstream output(pack / "pack.json");
        output << nlohmann::json{{"options", {{"use_new_hash", true}}},
                                 {"textures", {{"00000000000000AB", "odd_name.png"}}}};
    }
    const auto image = [](const std::filesystem::path& path, uint32_t width, uint32_t height) {
        WriteAzaharTexturePng(path, width, height, std::vector<uint8_t>(static_cast<size_t>(width) * height * 4U, 255U));
    };
    const auto world = pack / "ENVIRONMENT";
    image(world / MakeAzaharTextureFilename(8U, 8U, 0x11U, 0U, 0U), 16U, 16U);  // 2x
    image(world / MakeAzaharTextureFilename(8U, 8U, 0x11U, 0U, 1U), 64U, 64U);  // its mip 1: not the indexed file
    image(world / MakeAzaharTextureFilename(8U, 4U, 0x12U, 0U, 0U), 24U, 8U);   // 3x on one axis
    image(world / MakeAzaharTextureFilename(8U, 8U, 0x14U, 0U, 1U), 64U, 64U);  // only a mip 1: no native size
    image(world / "odd_name.png", 128U, 128U);                                  // pack.json's name map
    image(pack / "UI" / MakeAzaharTextureFilename(8U, 8U, 0x13U, 0U, 0U), 64U, 64U); // HUD, 8x
    const auto waitForResult = [](const AzaharTexturePackRuntime& runtime, uint64_t after) {
        for (int tries = 0; tries < 500 && runtime.ImageScaleSerial() <= after; ++tries) std::this_thread::sleep_for(10ms);
        return runtime.ImageScaleSerial() > after;
    };
    const AzaharTexturePackConfiguration configuration{.LoadCustomTextures = true, .UserDirectory = Root,
                                                       .LoadDirectory = pack, .MaxReplacementScale = 2U,
                                                       .ImageScaleRecord = record};
    {
        AzaharTexturePackRuntime runtime;
        EXPECT_EQ(runtime.LargestImageScale(), 0U);
        runtime.Configure(configuration);
        ASSERT_TRUE(waitForResult(runtime, 0U));
        const auto status = runtime.ImageScaleStatus();
        EXPECT_EQ(status.Largest, 3U);
        EXPECT_EQ(runtime.LargestImageScale(), 3U);
        EXPECT_FALSE(status.Remembered);
        EXPECT_EQ(status.Measured, 2U);
        EXPECT_EQ(status.Hud, 1U);
        EXPECT_EQ(status.Unnamed, 2U);
        EXPECT_EQ(status.Unreadable, 0U);
        EXPECT_EQ(status.Files, 7U); // pack.json and the six images
        std::ifstream file(record);
        std::string line;
        std::getline(file, line);
        const auto remembered = ParsePackScaleRecord(line);
        ASSERT_TRUE(remembered.has_value());
        EXPECT_EQ(remembered->Largest, 3U);
        EXPECT_EQ(remembered->Measured, 2U);
        EXPECT_EQ(remembered->Fingerprint.Files, 7U);

        // A size cap change indexes the same pack again: the sizes stay known.
        auto otherCap = configuration;
        otherCap.MaxReplacementScale = 4U;
        runtime.Configure(otherCap);
        EXPECT_EQ(runtime.LargestImageScale(), 3U);
        runtime.WaitForPendingLoads();
        std::this_thread::sleep_for(300ms);
        EXPECT_EQ(runtime.LargestImageScale(), 3U);
        EXPECT_EQ(runtime.ImageScaleSerial(), status.Serial); // same pack: no new result

        // HD textures off: not known.
        auto off = configuration;
        off.LoadCustomTextures = false;
        runtime.Configure(off);
        EXPECT_EQ(runtime.LargestImageScale(), 0U);
    }
    {
        // The next start lists the pack and finds it remembered.
        AzaharTexturePackRuntime runtime;
        runtime.Configure(configuration);
        ASSERT_TRUE(waitForResult(runtime, 0U));
        EXPECT_TRUE(runtime.ImageScaleStatus().Remembered);
        EXPECT_EQ(runtime.LargestImageScale(), 3U);
    }
    // The pack copied again at other sizes (same names): measured again.
    image(world / MakeAzaharTextureFilename(8U, 4U, 0x12U, 0U, 0U), 40U, 20U); // 5x
    {
        AzaharTexturePackRuntime runtime;
        runtime.Configure(configuration);
        ASSERT_TRUE(waitForResult(runtime, 0U));
        EXPECT_FALSE(runtime.ImageScaleStatus().Remembered);
        EXPECT_EQ(runtime.LargestImageScale(), 5U);
    }
}

} // namespace
