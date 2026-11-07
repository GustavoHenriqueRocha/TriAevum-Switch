#include "oot3d/renderer/azahar_texture_pack.h"
#include "oot3d/renderer/azahar_hud_image_list.h"
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

} // namespace
