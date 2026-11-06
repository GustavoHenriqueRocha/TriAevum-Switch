// deko3d lighting variants (gfx_deko3d_lighting.h): the toon and per-pixel
// text mutations, the variant lookup and its fallback, the config switch and
// the surface lighting block's layout and binding.
#include "fast/backends/gfx_deko3d_lighting.h"
#include "fast/backends/gfx_deko3d_shader_pack.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

using Fast::Deko3d::LightingMode;

// A recorded GLSL 4.3 PICA fragment text (shadercache/094682b1669320e0.src,
// TEV stages 2-5 left out).
constexpr std::string_view kNative = R"glsl(#version 430
layout(location=0) in vec4 pica_primary_color;
layout(location=1) in vec2 pica_texcoord0;
layout(location=5) in vec4 pica_normquat;
layout(location=6) in vec3 pica_view;
layout(binding=1) uniform sampler2D pica_texture0;
layout(binding=4, std140) uniform PicaFragmentUniforms {
    vec4 tev_constants[6];
    vec4 combiner_buffer_color;
    int alpha_reference;
    float depth_scale;
    float depth_offset;
    int w_buffering;
    vec4 fog_color;
    vec4 fog_lut[64];
} fragment_uniforms;
layout(location=0) out vec4 pica_color;
vec3 byteround3(vec3 value) { precise vec3 scaled=value*255.0; precise vec3 biased=scaled+0.5; return floor(biased)/255.0; }
float byteround1(float value) { precise float scaled=value*255.0; precise float biased=scaled+0.5; return floor(biased)/255.0; }
void main() {
    precise vec4 rounded_primary_color = vec4(byteround3(pica_primary_color.rgb), byteround1(pica_primary_color.a));
    vec4 primary_fragment_color = vec4(0.0);
    vec4 secondary_fragment_color = vec4(0.0);
    vec4 combiner_buffer = vec4(0.0);
    vec4 next_combiner_buffer = fragment_uniforms.combiner_buffer_color;
    vec4 combiner_output = vec4(0.0);
    // OOT3D_PICA_LIGHTING_EXTENSION_POINT
    precise vec3 color_output_0 = byteround3(clamp(rounded_primary_color.rgb * texture(pica_texture0, pica_texcoord0).rgb, vec3(0.0), vec3(1.0)));
    precise float alpha_output_0 = byteround1(clamp(rounded_primary_color.a * texture(pica_texture0, pica_texcoord0).a, 0.0, 1.0));
    combiner_output = vec4(clamp(color_output_0 * 2.0, vec3(0.0), vec3(1.0)), clamp(alpha_output_0 * 1.0, 0.0, 1.0));
    combiner_buffer = next_combiner_buffer;
    int alpha_byte = int(combiner_output.a * 255.0);
    if (alpha_byte <= fragment_uniforms.alpha_reference) discard;
    float pica_z_over_w = -gl_FragCoord.z;
    float pica_depth = gl_FragCoord.z;
    combiner_output.rgb = mix(fragment_uniforms.fog_color.rgb, combiner_output.rgb, 1.0);
    pica_color = vec4(byteround3(combiner_output.rgb), byteround1(combiner_output.a));
}
)glsl";

std::string Replace(std::string_view text, std::string_view from, std::string_view to) {
  std::string result(text);
  const size_t at = result.find(from);
  Require(at != std::string::npos, "test text has the replaced part");
  result.replace(at, from.size(), to);
  return result;
}

// Removes the first occurrence of `part` (which must be there).
std::string Without(std::string text, std::string_view part) {
  const size_t at = text.find(part);
  Require(at != std::string::npos, "variant contains the inserted part");
  text.erase(at, part.size());
  return text;
}

void TestConfig() {
  using Fast::Deko3d::ParseLightingMode;
  Require(ParseLightingMode("") == LightingMode::Off, "empty = off");
  Require(ParseLightingMode("off") == LightingMode::Off, "off");
  Require(ParseLightingMode("toon") == LightingMode::Toon, "toon");
  Require(ParseLightingMode(" TOON\r\n") == LightingMode::Toon, "toon, case and spaces");
  Require(ParseLightingMode("perpixel") == LightingMode::PerPixel, "perpixel");
  Require(ParseLightingMode("per-pixel\nsomething") == LightingMode::PerPixel, "first line only");
  Require(ParseLightingMode("modern") == LightingMode::Off, "unknown = off");
  Require(ParseLightingMode("\ntoon") == LightingMode::Off, "empty first line = off");

  const auto dir = std::filesystem::temp_directory_path() / "oot3d_deko3d_lighting_tests";
  std::filesystem::remove_all(dir);
  Require(Fast::Deko3d::LoadLightingMode(dir) == LightingMode::Off, "no file = off");
  Require(Fast::Deko3d::SaveLightingMode(dir, LightingMode::PerPixel).empty(), "save perpixel");
  Require(Fast::Deko3d::LoadLightingMode(dir) == LightingMode::PerPixel, "reload perpixel");
  Require(Fast::Deko3d::SaveLightingMode(dir, LightingMode::Toon).empty(), "save toon");
  Require(Fast::Deko3d::LoadLightingMode(dir) == LightingMode::Toon, "reload toon");
  Require(Fast::Deko3d::SaveLightingMode(dir, LightingMode::Off).empty(), "save off");
  Require(!std::filesystem::exists(dir / Fast::Deko3d::kLightingConfigName), "off removes config/lighting");
  std::filesystem::remove_all(dir);

  auto &live = Fast::Deko3d::LiveLightingSettings();
  const uint64_t revision = live.Revision.load();
  Require(live.Current() == LightingMode::Off, "live default off");
  live.Publish(LightingMode::Toon);
  Require(live.Current() == LightingMode::Toon && live.Revision.load() == revision + 1U, "publish");
  live.Publish(LightingMode::Off);
}

void TestOffIsNative() {
  Require(!Fast::Deko3d::LightingFragmentVariant(LightingMode::Off, kNative).has_value(), "off: no variant");
  // The native key is the key of the native text alone.
  const uint64_t native = Fast::Deko3d::ShaderTextKey(Fast::Deko3d::ShaderStage::Fragment, kNative);
  const auto toon = Fast::Deko3d::ToonFragmentVariant(kNative);
  const auto perPixel = Fast::Deko3d::PerPixelFragmentVariant(kNative);
  Require(toon && perPixel, "both variants apply to a recorded text");
  Require(Fast::Deko3d::ShaderTextKey(Fast::Deko3d::ShaderStage::Fragment, *toon) != native, "toon: its own key");
  Require(Fast::Deko3d::ShaderTextKey(Fast::Deko3d::ShaderStage::Fragment, *perPixel) != native,
          "perpixel: its own key");
  Require(Fast::Deko3d::ShaderTextKey(Fast::Deko3d::ShaderStage::Fragment, kNative) == native,
          "making variants leaves the native key");
}

void TestToon() {
  const auto variant = Fast::Deko3d::ToonFragmentVariant(kNative);
  Require(variant.has_value(), "toon applies");
  Require(*Fast::Deko3d::ToonFragmentVariant(kNative) == *variant, "toon: stable");
  Require(*Fast::Deko3d::ToonFragmentVariant(*variant) == *variant, "toon: idempotent");
  Require(variant->find(Fast::Deko3d::kToonVariantMarker) != std::string::npos, "toon marker");
  Require(variant->find("binding=15") == std::string::npos && variant->find("set=0") == std::string::npos,
          "toon: constants baked in, no extra block");
  // Only the declarations before main() and the banding line before the
  // depth are new; the rest is the native text byte for byte.
  const size_t main = variant->find("void main()");
  const size_t nativeMain = kNative.find("void main()");
  std::string stripped = variant->substr(0, nativeMain) + variant->substr(main);
  stripped = Without(stripped, "    combiner_output.rgb = oot3d_apply_toon(combiner_output.rgb);\n");
  Require(stripped == kNative, "toon: only the intended code changes");
  Require(variant->substr(0, nativeMain) == kNative.substr(0, nativeMain), "toon: prefix kept");
  // No depth: no place for the banding.
  const std::string noDepth = Replace(kNative, "    float pica_z_over_w = -gl_FragCoord.z;\n", "");
  Require(!Fast::Deko3d::ToonFragmentVariant(noDepth).has_value(), "toon: unsupported without the depth line");
  Require(!Fast::Deko3d::ToonFragmentVariant(*Fast::Deko3d::PerPixelFragmentVariant(kNative)).has_value(),
          "toon of a per-pixel variant: unsupported");
}

void TestPerPixel() {
  const auto variant = Fast::Deko3d::PerPixelFragmentVariant(kNative);
  Require(variant.has_value(), "perpixel applies");
  Require(*Fast::Deko3d::PerPixelFragmentVariant(kNative) == *variant, "perpixel: stable");
  Require(*Fast::Deko3d::PerPixelFragmentVariant(*variant) == *variant, "perpixel: idempotent");
  std::string stripped = Without(*variant, Fast::Deko3d::PerPixelFragmentDeclarations());
  stripped = Without(stripped, Fast::Deko3d::PerPixelFragmentBody());
  Require(stripped == kNative, "perpixel: only the intended code changes");
  Require(variant->find("layout(binding=5, std140) uniform PicaSurfaceLighting") != std::string::npos,
          "perpixel: block at binding 5");
  Require(variant->find("binding=16") == std::string::npos, "perpixel: no binding 16");
  Require(variant->find("placed_light(") == std::string::npos && variant->find("water(") == std::string::npos &&
              variant->find("pica_surface_shadow(") == std::string::npos &&
              variant->find("pica_point_shadow(") == std::string::npos,
          "perpixel: no placed lights, water or shadows");
  Require(variant->find("min(pica_surface.control.y, 4u)") != std::string::npos, "perpixel: at most 4 points");
  // The body follows the lighting extension point, after the primary colour.
  const size_t body = variant->find(Fast::Deko3d::PerPixelFragmentBody());
  Require(body > variant->find("// OOT3D_PICA_LIGHTING_EXTENSION_POINT") &&
              body > variant->find("precise vec4 rounded_primary_color"),
          "perpixel: body after the primary colour and the extension point");

  // Alpha that reads the primary RGB would see relit colour: no variant.
  const std::string alphaRgb = Replace(kNative, "byteround1(clamp(rounded_primary_color.a *",
                                       "byteround1(clamp(rounded_primary_color.g *");
  Require(!Fast::Deko3d::PerPixelFragmentVariant(alphaRgb).has_value(), "perpixel: alpha reading RGB");
  const std::string noPoint = Replace(kNative, "    // OOT3D_PICA_LIGHTING_EXTENSION_POINT\n", "");
  Require(!Fast::Deko3d::PerPixelFragmentVariant(noPoint).has_value(), "perpixel: no extension point");
  const std::string taken = Replace(kNative, "layout(location=6) in vec3 pica_view;",
                                    "layout(location=6) in vec3 pica_view;\nlayout(location=9) in vec3 other;");
  Require(!Fast::Deko3d::PerPixelFragmentVariant(taken).has_value(), "perpixel: location 9 taken");
  Require(!Fast::Deko3d::PerPixelFragmentVariant(*Fast::Deko3d::ToonFragmentVariant(kNative)).has_value(),
          "perpixel of a toon variant: unsupported");
}

// Members of a `uniform PicaSurfaceLighting { ... }` block, and its layout().
struct Block {
  std::string Layout;
  std::vector<std::string> Members;
};

Block ParseBlock(std::string_view text) {
  Block block;
  const size_t name = text.find("uniform PicaSurfaceLighting {");
  Require(name != std::string_view::npos, "block present");
  const size_t layout = text.rfind("layout(", name);
  block.Layout = std::string(text.substr(layout, name - layout));
  size_t at = text.find('{', name) + 1U;
  const size_t end = text.find('}', at);
  while (at < end) {
    const size_t semi = text.find(';', at);
    if (semi == std::string_view::npos || semi > end) break;
    std::string member(text.substr(at, semi - at));
    member.erase(0, member.find_first_not_of(" \n"));
    block.Members.push_back(member);
    at = semi + 1U;
  }
  return block;
}

// std140 bytes of the members (vec4, uvec4, the three light structs; arrays of them).
size_t Std140Bytes(const std::vector<std::string> &members, std::map<std::string, size_t> *offsets) {
  const std::map<std::string, size_t> sizes = {
      {"vec4", 16}, {"uvec4", 16}, {"PicaSurfaceLight", 48}, {"PicaScenePointLight", 32}, {"PicaPlacedLight", 64}};
  size_t offset = 0;
  for (const auto &member : members) {
    const size_t space = member.find(' ');
    const auto type = sizes.find(member.substr(0, space));
    Require(type != sizes.end(), "block member of a known type");
    std::string name = member.substr(space + 1U);
    size_t count = 1;
    if (const size_t bracket = name.find('['); bracket != std::string::npos) {
      count = std::stoul(name.substr(bracket + 1U));
      name.erase(bracket);
    }
    (*offsets)[name] = offset;
    offset += type->second * count;
  }
  return offset;
}

void TestBlockLayoutAndBinding() {
  using Fast::Renderer3ds::PicaSurfaceLightingUniforms;
  const Block upstream = ParseBlock(Fast::Renderer3ds::PicaSurfaceLightingDeclarations());
  const Block ours = ParseBlock(Fast::Deko3d::PerPixelFragmentDeclarations());
  Require(upstream.Members == ours.Members, "the block is upstream's, member for member");
  Require(upstream.Layout.find("binding=16") != std::string::npos, "upstream binds 16");
  Require(ours.Layout == "layout(binding=5, std140) ", "deko3d binds 5, std140");
  static_assert(Fast::Deko3d::kSurfaceLightingBinding < 16U, "a deko3d uniform slot");
  static_assert(Fast::Deko3d::kSurfaceLightingBinding != 4U, "not the PICA fragment uniforms");
  static_assert(Fast::Deko3d::kUpstreamSurfaceLightingBinding == 16U);
  std::map<std::string, size_t> offsets;
  Require(Std140Bytes(ours.Members, &offsets) == sizeof(PicaSurfaceLightingUniforms), "2992 bytes in std140");
  Require(offsets["control"] == offsetof(PicaSurfaceLightingUniforms, Control), "control offset");
  Require(offsets["lights"] == offsetof(PicaSurfaceLightingUniforms, Lights), "lights offset");
  Require(offsets["environment_directions"] == offsetof(PicaSurfaceLightingUniforms, EnvironmentDirections),
          "environment directions offset");
  Require(offsets["scene_directions"] == offsetof(PicaSurfaceLightingUniforms, SceneDirections),
          "scene directions offset");
  Require(offsets["scene_points"] == offsetof(PicaSurfaceLightingUniforms, ScenePoints), "scene points offset");
  Require(offsets["baked_reference_up"] == offsetof(PicaSurfaceLightingUniforms, BakedReferenceUp),
          "baked reference offset");
  Require(offsets["baked_reference_key"] == offsetof(PicaSurfaceLightingUniforms, BakedReferenceKey),
          "baked key offset");

  PicaSurfaceLightingUniforms uniforms;
  uniforms.Control = {1U, 9U, 2U, 0x0501U};
  for (auto &point : uniforms.ScenePoints) point.Color = {1.0F, 1.0F, 1.0F, 0.0F};
  uniforms.Rim = {1.0F, 2.0F, 1.0F, 0.0F};
  Fast::Deko3d::TrimSurfaceLighting(uniforms);
  Require(uniforms.Control[1] == Fast::Deko3d::kMaximumSurfacePointLights, "trim: 4 points");
  Require(uniforms.ScenePoints[3].Color[0] == 1.0F && uniforms.ScenePoints[4].Color[0] == 0.0F,
          "trim: points past 4 cleared");
  Require(uniforms.Control[3] == 0x0001U, "trim: no placed lights, other control bits kept");
  Require(uniforms.Rim[0] == 0.0F, "trim: no rim");
}

void TestLookupAndFallback() {
  using Fast::Deko3d::ShaderStage;
  const auto toon = Fast::Deko3d::ToonFragmentVariant(kNative);
  const uint64_t toonKey = Fast::Deko3d::ShaderTextKey(ShaderStage::Fragment, *toon);
  // A "pack" with the toon variant only; misses are recorded with their text.
  const int nativeVertex = 1, nativeFragment = 2, toonFragment = 3;
  std::vector<std::pair<uint64_t, std::string>> recorded;
  const auto lookup = [&](uint64_t key, std::string_view text) -> const int * {
    if (key == toonKey) return &toonFragment;
    recorded.emplace_back(key, std::string(text));
    return nullptr;
  };
  Fast::Deko3d::LightingCounters counters;
  const int *found = Fast::Deko3d::ResolveLightingVariant<int>(ShaderStage::Fragment, toon, lookup, counters);
  Require(found == &toonFragment && counters.VariantsFound == 1U, "toon variant found by its key");
  auto choice = Fast::Deko3d::ChooseLightingProgram<int>(&nativeVertex, &nativeFragment, &nativeVertex, found,
                                                         counters);
  Require(choice.Lit && choice.Vertex == &nativeVertex && choice.Fragment == &toonFragment && counters.DrawsLit == 1U,
          "toon: native vertex, variant fragment");

  const auto perPixel = Fast::Deko3d::PerPixelFragmentVariant(kNative);
  const int *missing = Fast::Deko3d::ResolveLightingVariant<int>(ShaderStage::Fragment, perPixel, lookup, counters);
  Require(missing == nullptr && counters.VariantsMissing == 1U, "missing variant");
  Require(recorded.size() == 1U && recorded[0].second == *perPixel &&
              recorded[0].first == Fast::Deko3d::ShaderTextKey(ShaderStage::Fragment, *perPixel),
          "a missing variant is recorded with its own text and key");
  const int perPixelVertex = 4;
  choice = Fast::Deko3d::ChooseLightingProgram<int>(&nativeVertex, &nativeFragment, &perPixelVertex, missing,
                                                    counters);
  Require(!choice.Lit && choice.Vertex == &nativeVertex && choice.Fragment == &nativeFragment &&
              counters.DrawsNative == 1U,
          "missing fragment variant: both native shaders, never a mixed pair");
  choice = Fast::Deko3d::ChooseLightingProgram<int>(&nativeVertex, &nativeFragment, nullptr, &toonFragment,
                                                    counters);
  Require(!choice.Lit && choice.Vertex == &nativeVertex && choice.Fragment == &nativeFragment,
          "missing vertex variant: both native shaders");

  const std::optional<std::string> none;
  Require(Fast::Deko3d::ResolveLightingVariant<int>(ShaderStage::Fragment, none, lookup, counters) == nullptr &&
              counters.VariantsUnsupported == 1U && recorded.size() == 1U,
          "unsupported text: not looked up, not recorded");

  // Through the real pack index: a pack without the variant finds nothing.
  std::vector<uint8_t> pack(16 + 24 + 8, 0);
  const uint32_t header[4] = {Fast::Deko3d::kShaderPackMagic, Fast::Deko3d::kShaderPackVersion, 1U, 0U};
  std::memcpy(pack.data(), header, sizeof(header));
  const Fast::Deko3d::ShaderPackEntry entry{Fast::Deko3d::ShaderTextKey(ShaderStage::Fragment, kNative), 40U, 8U, 1U,
                                            0U};
  std::memcpy(pack.data() + 16, &entry, sizeof(entry));
  std::memcpy(pack.data() + 40, &Fast::Deko3d::kDkshMagic, sizeof(uint32_t));
  Fast::Deko3d::ShaderPackIndex index;
  std::string error;
  Require(index.Parse(pack, &error), "pack parses");
  Require(!index.Find(entry.Key).empty(), "native key in the pack");
  Require(index.Find(toonKey).empty(), "variant key not in a pack without variants");
}

void TestVertexVariantNeedsHooks() {
  Fast::Renderer3ds::PicaVertexShaderHookLayout hooks;
  Require(!Fast::Deko3d::PerPixelVertexVariant("void main() {}", hooks).has_value(),
          "vertex variant: none without valid hooks");
}

} // namespace

int main() {
  TestConfig();
  TestOffIsNative();
  TestToon();
  TestPerPixel();
  TestBlockLayoutAndBinding();
  TestLookupAndFallback();
  TestVertexVariantNeedsHooks();
  std::puts("oot3d_deko3d_lighting_tests: ok");
  return 0;
}
