// Texture pack size cap, mipmaps, the HD textures setting and the sizes a pack
// has (azahar_texture_scale.h, azahar_texture_pack_settings.h,
// azahar_pack_scale.h).
#include "oot3d/renderer/azahar_pack_scale.h"
#include "oot3d/renderer/azahar_texture_pack_settings.h"
#include "oot3d/renderer/azahar_texture_scale.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace Oot3d::Renderer;

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

std::vector<uint8_t> Solid(TextureSize size, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  std::vector<uint8_t> pixels(static_cast<size_t>(size.Width) * size.Height * 4U);
  for (size_t i = 0; i < pixels.size(); i += 4U) {
    pixels[i] = r;
    pixels[i + 1U] = g;
    pixels[i + 2U] = b;
    pixels[i + 3U] = a;
  }
  return pixels;
}

const uint8_t *Texel(const std::vector<uint8_t> &pixels, TextureSize size, uint32_t x, uint32_t y) {
  return pixels.data() + (static_cast<size_t>(y) * size.Width + x) * 4U;
}

void CapSizes() {
  // The 1080p pack is 4x the native texture: the default cap halves it.
  Require(CappedReplacementSize({128, 128}, {512, 512}, 2) == TextureSize{256, 256}, "4x pack at cap 2 -> 2x");
  Require(CappedReplacementSize({128, 64}, {1024, 512}, 2) == TextureSize{256, 128}, "8x (4K) pack at cap 2 -> 2x");
  Require(CappedReplacementSize({256, 32}, {1024, 128}, 3) == TextureSize{768, 96}, "cap 3: 3x, not a power of two");
  Require(CappedReplacementSize({128, 128}, {512, 512}, 0) == TextureSize{512, 512}, "cap 0 keeps the image");
  Require(CappedReplacementSize({128, 128}, {256, 256}, 2) == TextureSize{256, 256}, "at the cap: unchanged");
  Require(CappedReplacementSize({128, 128}, {192, 192}, 2) == TextureSize{192, 192}, "1.5x below the cap: unchanged");
  Require(CappedReplacementSize({128, 128}, {64, 64}, 2) == TextureSize{64, 64}, "never enlarged");
  Require(CappedReplacementSize({64, 32}, {512, 512}, 2) == TextureSize{128, 64}, "each axis capped on its own");
  Require(CappedReplacementSize({64, 64}, {512, 100}, 2) == TextureSize{128, 100}, "only the axis above the cap");
  // HUD textures: 3x native whatever "HD texture size" says (720 = 3 x 240).
  Require(kUiTextureMaxScale == 3U, "the HUD cap is 3x");
  Require(CappedReplacementSize({512, 256}, {4096, 2048}, kUiTextureMaxScale) == TextureSize{1536, 768},
          "an 8x HUD atlas -> 3x (1536x768)");
  Require(CappedReplacementSize({64, 64}, {128, 128}, kUiTextureMaxScale) == TextureSize{128, 128},
          "a 2x HUD image is kept");
  Require(CappedReplacementSize({0, 0}, {512, 512}, 2) == TextureSize{512, 512}, "unknown native size: no cap");
  Require(CappedReplacementSize({128, 128}, {512, 512}, 1) == TextureSize{128, 128}, "cap 1: native size");
}

void DownscaleRatios() {
  // 2x2 -> 1x1 is the plain average for opaque texels.
  {
    const std::vector<uint8_t> source{10, 20, 30, 255, 30, 40, 50, 255, 50, 60, 70, 255, 70, 80, 90, 255};
    const auto out = DownscaleRgba8(source, {2, 2}, {1, 1});
    Require(out.size() == 4U, "2x2 -> 1x1 size");
    Require(out[0] == 40 && out[1] == 50 && out[2] == 60 && out[3] == 255, "2x2 -> 1x1 averages");
  }
  // 3 -> 2 (ratio 1.5): output 0 covers texel 0 and half of texel 1.
  {
    const std::vector<uint8_t> source{0, 0, 0, 255, 255, 255, 255, 255, 0, 0, 0, 255};
    const auto out = DownscaleRgba8(source, {3, 1}, {2, 1});
    Require(out.size() == 8U, "3 -> 2 size");
    Require(out[0] == 85 && out[4] == 85, "3 -> 2 weights the half texel by its area");
  }
  // 4x4 -> 2x2 keeps each quadrant apart.
  {
    std::vector<uint8_t> source = Solid({4, 4}, 0, 0, 0, 255);
    for (uint32_t y = 0; y < 2; ++y)
      for (uint32_t x = 2; x < 4; ++x) source[(y * 4U + x) * 4U] = 200;
    const auto out = DownscaleRgba8(source, {4, 4}, {2, 2});
    Require(Texel(out, {2, 2}, 1, 0)[0] == 200 && Texel(out, {2, 2}, 0, 0)[0] == 0 &&
                Texel(out, {2, 2}, 1, 1)[0] == 0,
            "4x4 -> 2x2 quadrants");
  }
  // A constant image stays constant at any ratio (weights sum to 1).
  {
    const auto source = Solid({7, 5}, 13, 77, 201, 99);
    const auto out = DownscaleRgba8(source, {7, 5}, {3, 2});
    bool same = out.size() == 3U * 2U * 4U;
    for (size_t i = 0; same && i < out.size(); i += 4U)
      same = out[i] == 13 && out[i + 1] == 77 && out[i + 2] == 201 && out[i + 3] == 99;
    Require(same, "7x5 -> 3x2 keeps a constant image");
  }
  // Same size on one axis.
  {
    const std::vector<uint8_t> source{0, 0, 0, 255, 100, 100, 100, 255, 10, 10, 10, 255, 30, 30, 30, 255};
    const auto out = DownscaleRgba8(source, {2, 2}, {2, 1});
    Require(out[0] == 5 && out[4] == 65, "2x2 -> 2x1 averages rows only");
  }
  Require(DownscaleRgba8(std::vector<uint8_t>(16), {2, 2}, {4, 4}).empty(), "never enlarges");
  Require(DownscaleRgba8(std::vector<uint8_t>(12), {2, 2}, {1, 1}).empty(), "wrong byte count rejected");
}

void DownscaleAlpha() {
  // A transparent red texel beside an opaque blue one: the colour is the
  // blue (no red fringe), the coverage is half.
  {
    const std::vector<uint8_t> source{255, 0, 0, 0, 0, 0, 255, 255};
    const auto out = DownscaleRgba8(source, {2, 1}, {1, 1});
    Require(out[0] == 0 && out[1] == 0 && out[2] == 255, "transparent texels do not tint");
    Require(out[3] == 128, "alpha is the plain average");
  }
  // Partial alpha weights the colour by coverage: (200*255 + 0*85) / 340 = 150.
  {
    const std::vector<uint8_t> source{200, 0, 0, 255, 0, 0, 0, 85};
    const auto out = DownscaleRgba8(source, {2, 1}, {1, 1});
    Require(out[0] == 150 && out[3] == 170, "colour weighted by alpha");
  }
  // Fully transparent: keep the plain colour average (a later bilinear
  // fetch next to opaque texels then blends towards a sensible colour).
  {
    const std::vector<uint8_t> source{100, 0, 0, 0, 200, 0, 0, 0};
    const auto out = DownscaleRgba8(source, {2, 1}, {1, 1});
    Require(out[0] == 150 && out[3] == 0, "transparent area keeps its colour");
  }
}

void MipChain() {
  Require(FullMipLevelCount({256, 256}) == 9U, "256x256 has 9 levels");
  Require(FullMipLevelCount({8, 2}) == 4U, "8x2 has 4 levels");
  Require(FullMipLevelCount({0, 4}) == 0U, "empty image has no levels");
  const auto level0 = Solid({8, 2}, 40, 80, 120, 255);
  const auto chain = BuildRgba8MipChain(level0, {8, 2}, 16);
  Require(chain.size() == 3U, "8x2 -> 4x1, 2x1, 1x1");
  Require(chain[0].size() == 4U * 1U * 4U && chain[1].size() == 2U * 4U && chain[2].size() == 4U,
          "levels are max(1, previous / 2)");
  Require(chain[2][0] == 40 && chain[2][1] == 80 && chain[2][2] == 120 && chain[2][3] == 255,
          "constant image through the chain");
  Require(BuildRgba8MipChain(level0, {8, 2}, 2).size() == 1U, "levels counts level 0");
  Require(BuildRgba8MipChain(level0, {8, 2}, 1).empty(), "one level: no chain");
  // Odd size: 3x3 -> 1x1 averages all nine texels.
  std::vector<uint8_t> odd = Solid({3, 3}, 0, 0, 0, 255);
  odd[4 * 4] = 90; // centre texel
  const auto oddChain = BuildRgba8MipChain(odd, {3, 3}, 2);
  Require(oddChain.size() == 1U && oddChain[0].size() == 4U && oddChain[0][0] == 10, "3x3 -> 1x1 area average");
}

void SettingsFiles() {
  Require(ParseTexturePackMaxScale("2", 9) == 2U, "parse 2");
  Require(ParseTexturePackMaxScale(" 0 \r\n", 9) == 0U, "parse 0 with spaces and line end");
  Require(ParseTexturePackMaxScale("8", 9) == 8U, "parse the largest cap");
  Require(ParseTexturePackMaxScale("9", 2) == 2U, "above 8 falls back");
  Require(ParseTexturePackMaxScale("2x", 2) == 2U && ParseTexturePackMaxScale("", 3) == 3U &&
              ParseTexturePackMaxScale("-1", 2) == 2U,
          "garbage falls back");

  const auto root = std::filesystem::temp_directory_path() /
                    ("oot3d_texture_pack_settings_" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto config = root / "config";
  // No files: off, 2x, the default folder (an existing setup without the pack).
  const auto defaults = TexturePackSettings::Load(config);
  Require(!defaults.Enabled && defaults.MaxScale == 2U && defaults.Folder == kTexturePackDefaultFolder,
          "defaults without files");
  // The existing convention: an empty config/texture_pack turns the pack on.
  std::filesystem::create_directories(config);
  { std::ofstream file(config / "texture_pack"); }
  const auto existing = TexturePackSettings::Load(config);
  Require(existing.Enabled && existing.Folder == kTexturePackDefaultFolder, "empty texture_pack = on, default folder");
  { std::ofstream file(config / "texture_pack"); file << "sdmc:/packs/oot3d \r\n"; }
  auto custom = TexturePackSettings::Load(config);
  Require(custom.Enabled && custom.Folder == "sdmc:/packs/oot3d", "first line is the folder, trimmed");

  // Off removes the file; on writes it back with the custom folder.
  custom.Enabled = false;
  Require(custom.Save(config).empty() && !std::filesystem::exists(config / "texture_pack"), "off removes the file");
  custom.Enabled = true;
  custom.MaxScale = 0U;
  Require(custom.Save(config).empty(), "save on, no cap");
  Require(TexturePackSettings::Load(config) == custom, "custom folder and cap 0 round trip");
  // The default cap leaves no scale file.
  custom.MaxScale = 2U;
  custom.Folder = kTexturePackDefaultFolder;
  Require(custom.Save(config).empty() && !std::filesystem::exists(config / "texture_pack_scale"),
          "default cap: no file");
  std::string line = "x";
  { std::ifstream file(config / "texture_pack"); std::getline(file, line); }
  Require(line.empty(), "default folder: empty texture_pack");
  Require(TexturePackSettings::Load(config) == custom, "default round trip");
  std::filesystem::remove_all(root);

  Require(TexturePackDisplayFolder(kTexturePackDefaultFolder) == "switch/oot3dre/textures", "display folder");
  const auto configuration = MakeTexturePackConfiguration(custom, "sdmc:/switch/oot3dre/config");
  Require(configuration.LoadCustomTextures && configuration.MaxReplacementScale == 2U &&
              configuration.GenerateMipmaps && configuration.LoadDirectory == kTexturePackDefaultFolder,
          "runtime configuration");
  // The HUD list sits in config/, outside the pack folder (a pack swap keeps it).
  Require(configuration.HudImageList == std::filesystem::path("sdmc:/switch/oot3dre/config") / "texture_pack_hud",
          "HUD list in config/");
  Require(MakeTexturePackConfiguration(custom, {}).HudImageList.empty(), "no config directory: no HUD list");
  // The scene lists sit beside it; config/texture_pack_preload = 0 leaves them out.
  Require(configuration.SceneImageList ==
              std::filesystem::path("sdmc:/switch/oot3dre/config") / "texture_pack_scenes",
          "scene lists in config/");
  auto noPreload = custom;
  noPreload.ScenePreload = false;
  Require(MakeTexturePackConfiguration(noPreload, "sdmc:/switch/oot3dre/config").SceneImageList.empty(),
          "preload off: no scene lists");
  custom.Enabled = false;
  Require(!MakeTexturePackConfiguration(custom, {}).LoadCustomTextures, "off configures no loading");
}

// The HUD list of one session is what the next start decodes ahead: a line
// written must read back, a damaged line must not stop the others, and a
// hash once only (a duplicated line would queue nothing more but count twice).
void HudImageList() {
  const HudImageEntry icon{0x1FAF9B5B2ADF6038ULL, 32U, 32U};
  const HudImageEntry atlas{0x00000000000000ABULL, 512U, 256U};
  std::string text = FormatHudImageEntry(icon) + FormatHudImageEntry(atlas);
  Require(text == "1FAF9B5B2ADF6038 32 32\n00000000000000AB 512 256\n", "list line format");
  Require(ParseHudImageList(text) == std::vector<HudImageEntry>{icon, atlas}, "list round trip");
  // A write cut short, CRLF, a hand edit, a size out of range, a repeat.
  text = "1FAF9B5B2ADF6038 32 32\r\n"
         "garbage\n"
         "00000000000000CD 0 16\n"
         "00000000000000CE 16 4096\n"
         "00000000000000CF 16 16 extra\n"
         "1FAF9B5B2ADF6038 64 64\n"
         "\n"
         "00000000000000AB 512 256\n"
         "00000000000000E0 16";
  Require(ParseHudImageList(text) == std::vector<HudImageEntry>{icon, atlas}, "damaged lines skipped");
  Require(ParseHudImageList("").empty(), "empty list");
  // Lines of another pack go; order kept.
  const auto kept = PruneHudImageList(std::vector<HudImageEntry>{icon, atlas},
                                      [](uint64_t hash) { return hash == 0xABU; });
  Require(kept == std::vector<HudImageEntry>{atlas}, "prune keeps indexed entries");
  // The list never grows past its limit.
  std::string many;
  for (uint64_t hash = 1; hash <= kHudImageListLimit + 10U; ++hash) many += FormatHudImageEntry({hash, 8U, 8U});
  Require(ParseHudImageList(many).size() == kHudImageListLimit, "list limit");
}

// The scene lists: one file, a scene per line, the scene's order kept; a
// damaged line must not stop the others and a (scene, hash) counts once,
// while the same image may be listed for two scenes.
void SceneImageList() {
  const SceneImageEntry field{0x51U, {0x00C0FFEE00C0FFEEULL, 128U, 64U}};
  const SceneImageEntry fieldGround{0x51U, {0x0000000000000AB1ULL, 256U, 256U}};
  const SceneImageEntry forest{0x55U, {0x00C0FFEE00C0FFEEULL, 128U, 64U}};
  std::string text = FormatSceneImageEntry(field) + FormatSceneImageEntry(fieldGround) + FormatSceneImageEntry(forest);
  Require(text == "51 00C0FFEE00C0FFEE 128 64\n51 0000000000000AB1 256 256\n55 00C0FFEE00C0FFEE 128 64\n",
          "scene line format");
  Require(ParseSceneImageList(text) == std::vector<SceneImageEntry>{field, fieldGround, forest}, "scene round trip");
  text = "51 00C0FFEE00C0FFEE 128 64\r\n"
         "zz 00C0FFEE00C0FFEE 128 64\n"
         "100 0000000000000AB2 16 16\n"
         "51 0000000000000AB3 0 16\n"
         "51 0000000000000AB4 16 2048\n"
         "51 00C0FFEE00C0FFEE 64 64\n"
         "51 0000000000000AB5 16 16 x\n"
         "\n"
         "51 0000000000000AB1 256 256\n"
         "55 00C0FFEE00C0FFEE 128 64\n"
         "55 0000000000000AB6 1";
  Require(ParseSceneImageList(text) == std::vector<SceneImageEntry>{field, fieldGround, forest},
          "damaged scene lines skipped");
  Require(ParseSceneImageList("").empty(), "empty scene list");
  const auto kept = PruneSceneImageList(std::vector<SceneImageEntry>{field, fieldGround, forest},
                                        [](uint64_t hash) { return hash == 0xAB1U; });
  Require(kept == std::vector<SceneImageEntry>{fieldGround}, "prune keeps the pack's images");
  // A scene's lines stop at its limit, the others still read.
  std::string many;
  for (uint64_t hash = 1; hash <= kSceneImageListPerScene + 10U; ++hash)
    many += FormatSceneImageEntry({0x03U, {hash, 8U, 8U}});
  many += FormatSceneImageEntry(forest);
  const auto limited = ParseSceneImageList(many);
  Require(limited.size() == kSceneImageListPerScene + 1U && limited.back() == forest, "per-scene limit");
}

// config/texture_pack_preload and config/texture_pack_hold (read at start).
void PreloadAndHoldSettings() {
  Require(ParseTexturePackHoldMs("") == kTexturePackHoldDefaultMs, "empty hold: default");
  Require(ParseTexturePackHoldMs("0") == 0U && ParseTexturePackHoldMs("off\r\n") == 0U, "hold off");
  Require(ParseTexturePackHoldMs(" 600 \n") == 600U, "hold ms");
  Require(ParseTexturePackHoldMs("99999") == kTexturePackHoldLimitMs, "hold capped below the hang watchdog");
  Require(ParseTexturePackHoldMs("soon") == kTexturePackHoldDefaultMs, "unreadable hold: default");
  const auto root = std::filesystem::temp_directory_path() /
                    ("oot3d_pack_preload_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  auto settings = TexturePackSettings::Load(root);
  Require(settings.ScenePreload && settings.HoldMs == kTexturePackHoldDefaultMs && settings.Reresolve,
          "absent files: preload, hold and the second resolve on");
  std::ofstream(root / "texture_pack_preload") << "0\n";
  std::ofstream(root / "texture_pack_hold") << "250\n";
  std::ofstream(root / "texture_pack_reresolve") << "0\n";
  settings = TexturePackSettings::Load(root);
  Require(!settings.ScenePreload && settings.HoldMs == 250U && !settings.Reresolve, "files read");
  std::ofstream(root / "texture_pack_reresolve") << "1\n";
  Require(TexturePackSettings::Load(root).Reresolve, "texture_pack_reresolve: only a leading 0 turns it off");
  // The Video tab's Save leaves them alone.
  settings.Enabled = true;
  Require(settings.Save(root).empty() && std::filesystem::exists(root / "texture_pack_preload") &&
              std::filesystem::exists(root / "texture_pack_hold") &&
              std::filesystem::exists(root / "texture_pack_reresolve"),
          "save keeps the start-only files");
  std::filesystem::remove_all(root);
}

void ToggleCache() {
  using Action = TexturePackEntryAction;
  // Same epoch: nothing changed.
  Require(TexturePackEntryActionFor(false, 3, 3, true, false, true) == Action::Keep, "current native kept");
  Require(TexturePackEntryActionFor(true, 3, 3, true, false, true) == Action::Keep, "current pack image kept");
  // Turned off: pack images go, native textures stay.
  Require(TexturePackEntryActionFor(true, 3, 4, false, false, false) == Action::Release, "off releases pack images");
  Require(TexturePackEntryActionFor(false, 3, 4, false, false, false) == Action::Keep, "off keeps native textures");
  Require(TexturePackEntryActionFor(false, 3, 4, false, true, true) == Action::Keep,
          "off keeps a native made while the index was built");
  // Turned on (or another cap): native textures resolve through the pack again.
  Require(TexturePackEntryActionFor(false, 4, 5, true, false, false) == Action::Rebuild, "on rebuilds native textures");
  Require(TexturePackEntryActionFor(true, 4, 5, true, false, false) == Action::Release,
          "new cap releases old pack images");
  // The change's first frames draw while the pack indexes again: those natives
  // have no hash, so they wait for the index and are made again once it is
  // ready (08/11: they stayed native until evicted, pack images=0 in logs6).
  Require(TexturePackEntryActionFor(false, 5, 5, true, true, false) == Action::Keep,
          "made while indexing: drawn native until the index is ready");
  Require(TexturePackEntryActionFor(false, 5, 5, true, true, true) == Action::Rebuild,
          "made while indexing: resolved again once the index is ready");
  Require(TexturePackEntryActionFor(false, 5, 5, true, false, true) == Action::Keep,
          "a native that had its hash is kept (its pack image is polled)");
  Require(TexturePackEntryActionFor(true, 5, 5, true, true, true) == Action::Keep, "a current pack image is kept");

  // When a change has taken effect (the `texture pack change took effect` log line).
  Require(!TexturePackChangeTookEffect(false, false, 0U, true), "off: earlier pack images still cached");
  Require(TexturePackChangeTookEffect(false, false, 3U, false), "off: done once they are gone, natives drawn");
  Require(!TexturePackChangeTookEffect(true, false, 0U, false), "on: not before the index is ready");
  Require(!TexturePackChangeTookEffect(true, true, 2U, false), "on: textures on screen still wait for their image");
  Require(!TexturePackChangeTookEffect(true, true, 0U, true), "new cap: images of the earlier cap still cached");
  Require(TexturePackChangeTookEffect(true, true, 0U, false), "on: done");

  // The Video tab publishes; the renderer sees a new revision.
  auto &live = LiveTexturePackSettings();
  const uint64_t before = live.Revision.load();
  TexturePackSettings settings;
  settings.Enabled = true;
  settings.MaxScale = 4U;
  live.Publish(settings);
  Require(live.Revision.load() == before + 1U && live.Enabled.load() && live.MaxScale.load() == 4U,
          "publish bumps the revision");
}

// The PNG header, the native size in an Azahar name, the HUD folders, the
// scale of one image and the files a pack's index gives to measure.
void PackImageScales() {
  // Signature, IHDR length 13, "IHDR", width 300, height 0x10000.
  std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R',
                           0, 0, 0x01, 0x2C, 0, 1, 0, 0, 8, 6, 0, 0, 0};
  Require(ReadPngSize(png) == TextureSize{300, 0x10000}, "IHDR width and height, big-endian");
  Require(!ReadPngSize(std::span<const uint8_t>(png.data(), 23)), "short header");
  auto bad = png;
  bad[1] = 'J';
  Require(!ReadPngSize(bad), "not a PNG");
  bad = png;
  bad[12] = 'X';
  Require(!ReadPngSize(bad), "first chunk not IHDR");
  bad = png;
  bad[18] = bad[19] = 0;
  Require(!ReadPngSize(bad), "zero width");
  const auto root = std::filesystem::temp_directory_path() /
                    ("oot3d_pack_scale_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(root);
  { std::ofstream(root / "a.png", std::ios::binary).write(reinterpret_cast<const char *>(png.data()), png.size()); }
  { std::ofstream(root / "short.png", std::ios::binary) << "\x89PNG"; }
  Require(ReadPngFileSize(root / "a.png") == TextureSize{300, 0x10000}, "header read from a file");
  Require(!ReadPngFileSize(root / "short.png") && !ReadPngFileSize(root / "missing.png"), "short or missing file");
  std::filesystem::remove_all(root);

  Require(AzaharNativeSizeFromName("tex1_64x32_0123456789ABCDEF_5_mip0.png") == TextureSize{64, 32}, "Azahar name");
  Require(AzaharNativeSizeFromName("tex1_8x8_ab_0_mip0.PNG") == TextureSize{8, 8}, "short lowercase hash, any extension");
  Require(AzaharNativeSizeFromName("tex1_16x16_0123456789ABCDEF_2.png") == TextureSize{16, 16}, "no mip part");
  Require(!AzaharNativeSizeFromName("tex1_64x32_0123456789ABCDEF_5_mip1.png"), "mip 1: not the native size");
  Require(!AzaharNativeSizeFromName("grass.png") && !AzaharNativeSizeFromName("TEX1_64x32_01_5_mip0.png") &&
              !AzaharNativeSizeFromName("tex1_64_32_01_5_mip0.png") &&
              !AzaharNativeSizeFromName("tex1_0x32_01_5_mip0.png") &&
              !AzaharNativeSizeFromName("tex1_64x32_zz_5_mip0.png") &&
              !AzaharNativeSizeFromName("tex1_64x32_01_5_mip0_n.png") &&
              !AzaharNativeSizeFromName("tex1_64x32__5_mip0.png"),
          "odd names: no native size");

  Require(IsHudPackPath("UI/tex.png") && IsHudPackPath("maps/a/tex.png") && IsHudPackPath("Ui/x.png"),
          "UI and MAPS at the top, any case");
  Require(!IsHudPackPath("ENVIRONMENT/UI/tex.png") && !IsHudPackPath("UI.png") && !IsHudPackPath("LINK/tex.png"),
          "other folders, or a file named UI");

  // The smallest cap that keeps the image whole (CappedReplacementSize).
  for (const auto &[native, image, scale] :
       {std::tuple{TextureSize{8, 8}, TextureSize{16, 16}, 2U}, std::tuple{TextureSize{8, 8}, TextureSize{17, 16}, 3U},
        std::tuple{TextureSize{8, 4}, TextureSize{24, 8}, 3U}, std::tuple{TextureSize{64, 64}, TextureSize{32, 32}, 1U},
        std::tuple{TextureSize{8, 8}, TextureSize{256, 256}, 32U}, std::tuple{TextureSize{8, 8}, TextureSize{8, 8}, 1U}}) {
    Require(ImageScaleOf(native, image) == scale, "image scale");
    Require(CappedReplacementSize(native, image, scale) == image, "the scale keeps the image");
    Require(scale == 1U || CappedReplacementSize(native, image, scale - 1U) != image, "one less reduces it");
  }

  // The index's files: each once, HUD art and names without a native size apart.
  const std::filesystem::path pack = "sdmc:/switch/oot3dre/textures";
  const auto plan = PlanPackScaleScan(
      pack, {pack / "LINK/tex1_8x8_11_0_mip0.png", pack / "UI/tex1_8x8_12_0_mip0.png",
             pack / "ENVIRONMENT/grass.png", pack / "ENVIRONMENT/tex1_16x8_13_0_mip0.png",
             pack / "LINK/tex1_8x8_11_0_mip0.png" /* a file pack.json maps to a second hash */,
             pack / "MAPS/tex1_8x8_14_0_mip0.png", pack / "EFFECTS/tex1_8x8_15_0_mip1.png"});
  Require(plan.Files.size() == 2U && plan.Hud == 2U && plan.Unnamed == 2U, "plan: 2 measured, 2 HUD, 2 unnamed");
  Require(plan.Files[0].Path == pack / "ENVIRONMENT/tex1_16x8_13_0_mip0.png" &&
              plan.Files[0].Native == TextureSize{16, 8} && plan.Files[1].Native == TextureSize{8, 8},
          "plan: native sizes from the names");

  // The fingerprint: any order, and a file of another size is another pack.
  PackFingerprint first, second, resized, renamed;
  first.Add("ENVIRONMENT/a.png", 100);
  first.Add("pack.json", 20);
  second.Add("pack.json", 20);
  second.Add("ENVIRONMENT\\a.png", 100);
  resized.Add("ENVIRONMENT/a.png", 101);
  resized.Add("pack.json", 20);
  renamed.Add("ENVIRONMENT/b.png", 100);
  renamed.Add("pack.json", 20);
  Require(first == second && first.Files == 2U && first.Bytes == 120U, "fingerprint: order and separators");
  Require(!(first == resized) && !(first == renamed), "fingerprint: a size or a name changed");

  const PackScaleRecord record{first, 3U, 1234U};
  Require(ParsePackScaleRecord(FormatPackScaleRecord(record)) == record, "record round trip");
  Require(!ParsePackScaleRecord("") && !ParsePackScaleRecord("zz 1 2 3 4") && !ParsePackScaleRecord("1 2 3 4") &&
              !ParsePackScaleRecord("1 2 3 4 5 6"),
          "damaged record");
}

// "HD texture size": only the sizes the pack has.
void ScaleMenu() {
  const auto scales = [](const TexturePackScaleMenu &menu) {
    std::vector<uint32_t> result;
    for (const auto &choice : menu.Choices) result.push_back(choice.Scale);
    return result;
  };
  const auto selected = [](const TexturePackScaleMenu &menu) {
    return menu.Selected < 0 ? 99U : menu.Choices[static_cast<size_t>(menu.Selected)].Scale;
  };
  // Not known (HD textures off, not measured yet): as before.
  auto menu = MakeTexturePackScaleMenu(2U, 0U);
  Require(scales(menu) == std::vector<uint32_t>{1, 2, 3, 4, 0} && selected(menu) == 2U &&
              menu.Choices[1].Label == "2x (default)" && menu.Choices[3].Label == "4x (full 1080p pack)",
          "unknown: every size");
  Require(MakeTexturePackScaleMenu(6U, 0U).Selected == -1, "unknown: a cap the menu does not name is Custom");
  // The 2x copy shrink_texture_pack.py writes: 1x and 2x.
  menu = MakeTexturePackScaleMenu(2U, 2U);
  Require(scales(menu) == std::vector<uint32_t>{1, 2} && selected(menu) == 2U &&
              menu.Choices[1].Label == "2x (default, the largest in this pack)",
          "2x pack: 1x and 2x");
  // A saved cap above the pack's largest shows the largest (the file is kept).
  for (const uint32_t cap : {3U, 4U, 8U, 0U}) {
    Require(selected(MakeTexturePackScaleMenu(cap, 2U)) == 2U, "cap above a 2x pack: 2x shown");
  }
  Require(selected(MakeTexturePackScaleMenu(1U, 2U)) == 1U, "cap below the largest: as saved");
  // With models at 3x (--models 3).
  menu = MakeTexturePackScaleMenu(4U, 3U);
  Require(scales(menu) == std::vector<uint32_t>{1, 2, 3} && selected(menu) == 3U &&
              menu.Choices[2].Label == "3x (the largest in this pack)",
          "3x pack: up to 3x");
  Require(scales(MakeTexturePackScaleMenu(2U, 1U)) == std::vector<uint32_t>{1} &&
              MakeTexturePackScaleMenu(2U, 1U).Choices[0].Label == "1x (original size, the largest in this pack)",
          "1x pack");
  Require(scales(MakeTexturePackScaleMenu(2U, 4U)) == std::vector<uint32_t>{1, 2, 3, 4}, "4x pack: no No limit");
  // The full 1080p pack (world images up to 32x): every size.
  menu = MakeTexturePackScaleMenu(0U, 32U);
  Require(scales(menu) == std::vector<uint32_t>{1, 2, 3, 4, 0} && selected(menu) == 0U &&
              menu.Choices[4].Label == "No limit (up to 32x in this pack)",
          "1080p pack: every size");
  Require(selected(MakeTexturePackScaleMenu(3U, 32U)) == 3U, "1080p pack, cap 3");
  Require(MakeTexturePackScaleMenu(6U, 32U).Selected == -1, "1080p pack, cap 6: Custom");
  Require(selected(MakeTexturePackScaleMenu(6U, 5U)) == 0U, "cap 6 above a 5x pack: No limit");
}

} // namespace

int main() {
  CapSizes();
  DownscaleRatios();
  DownscaleAlpha();
  MipChain();
  SettingsFiles();
  ToggleCache();
  HudImageList();
  SceneImageList();
  PreloadAndHoldSettings();
  PackImageScales();
  ScaleMenu();
  std::puts("oot3d_texture_pack_scale_tests: ok");
  return 0;
}
