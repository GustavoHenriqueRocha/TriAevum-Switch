// Grass on the Switch (SWITCH_PORT.md, "Grass on deko3d"): density thinning
// of the placements, the Switch profile's budgets and config/grass, and the
// per-frame culling and instance stream of the deko3d draw.
#include "fast/oot3d/grass_anchor_codec.h"
#include "fast/oot3d/grass_instance_stream.h"
#include "fast/oot3d/grass_stream_handoff.h"
#include "fast/oot3d/grass_surface_extractor.h"
#include "fast/oot3d/grass_switch_profile.h"
#include "fast/oot3d/grass_texture_source_cache.h"
#include "fast/oot3d/grass_texture_twins.h"
#include "fast/oot3d/grass_visibility.h"

#include <nlohmann/json.hpp>

#include "oot3d_grass_switch_check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {
using namespace Fast::Oot3d;

// A flat square field of `quads` x `quads` quads (`size` world units across),
// two triangles each, in row order: the order a truncation would fill.
struct Field {
    std::vector<GrassSourceVertex> Vertices;
    std::vector<uint32_t> Indices;
};

Field MakeField(uint32_t quads, float size) {
    Field field;
    const float step = size / static_cast<float>(quads);
    for (uint32_t z = 0; z <= quads; ++z)
        for (uint32_t x = 0; x <= quads; ++x)
            field.Vertices.push_back({{x * step, 0.0F, z * step}, {0.0F, 1.0F, 0.0F},
                                      {static_cast<float>(x) / quads, static_cast<float>(z) / quads}, {}});
    for (uint32_t z = 0; z < quads; ++z)
        for (uint32_t x = 0; x < quads; ++x) {
            const uint32_t a = z * (quads + 1U) + x, b = a + 1U, c = a + quads + 1U, d = c + 1U;
            field.Indices.insert(field.Indices.end(), {a, c, b, b, c, d});
        }
    return field;
}

GrassSourceSurface Surface(const Field& field) {
    GrassSourceSurface surface;
    surface.GeometryId = 7U;
    surface.ContentVersion = 3U;
    surface.TransformBakedIntoVertices = true;
    surface.Vertices = field.Vertices;
    surface.Indices = field.Indices;
    return surface;
}

GrassPlacementRule FullRule() {
    GrassPlacementRule rule;
    rule.RuleId = 1U;
    rule.InputBlack = 0.0F;
    rule.InputWhite = 1.0F;
    rule.ResponseExponent = 1.0F;
    rule.OutputBlack = 0.0F;
    rule.OutputWhite = 1.0F;
    rule.MaximumSlopeDegrees = 60.0F;
    return rule;
}

GrassScalarMask Mask(uint8_t level) {
    GrassScalarMask mask;
    mask.Width = 1U;
    mask.Height = 1U;
    mask.Samples = {level};
    return mask;
}

GrassGenerationSettings SwitchGeneration() {
    InteractiveGrassSettings settings;
    ApplyGrassSwitchProfile(SelectGrassSwitchProfile(true, GrassSwitchDistance::Near), settings);
    return settings.Generation;
}

// Anchors per cell of a cells x cells grid over the field.
std::vector<uint32_t> CellCounts(const std::vector<GrassAnchor>& anchors, float size, uint32_t cells) {
    std::vector<uint32_t> counts(size_t{cells} * cells);
    for (const auto& anchor : anchors) {
        const auto x = std::min(cells - 1U, static_cast<uint32_t>(anchor.LocalPosition[0] / size * cells));
        const auto z = std::min(cells - 1U, static_cast<uint32_t>(anchor.LocalPosition[2] / size * cells));
        ++counts[size_t{z} * cells + x];
    }
    return counts;
}

TEST(Oot3dGrassThinning, ABudgetThatBitesThinsEvenlyWithoutBaldPatches) {
    const auto field = MakeField(40U, 2000.0F); // 400 m2, 3200 triangles
    const auto generation = SwitchGeneration(); // 40 tufts per m2: ~16000 candidates
    const auto rule = FullRule();
    const auto mask = Mask(255U);
    const uint32_t budget = 2000U;
    const auto anchors = GrassSurfaceExtractor::Extract(Surface(field), rule, generation, mask, budget);
    ASSERT_EQ(anchors.size(), budget);
    const auto counts = CellCounts(anchors, 2000.0F, 10U); // 4 m2 cells, 20 expected each
    for (size_t cell = 0; cell < counts.size(); ++cell) {
        EXPECT_GE(counts[cell], 6U) << "cell " << cell;
        EXPECT_LE(counts[cell], 40U) << "cell " << cell;
    }
    // The first and the last rows of the index buffer keep the same density.
    const uint32_t front = std::accumulate(counts.begin(), counts.begin() + 50, 0U);
    const uint32_t back = std::accumulate(counts.begin() + 50, counts.end(), 0U);
    EXPECT_NEAR(static_cast<double>(front), static_cast<double>(back), 0.1 * budget);
}

TEST(Oot3dGrassThinning, ThinningIsDeterministic) {
    const auto field = MakeField(30U, 1500.0F);
    const auto generation = SwitchGeneration();
    const auto first = GrassSurfaceExtractor::Extract(Surface(field), FullRule(), generation, Mask(255U), 1500U);
    const auto second = GrassSurfaceExtractor::Extract(Surface(field), FullRule(), generation, Mask(255U), 1500U);
    ASSERT_EQ(first.size(), second.size());
    for (size_t index = 0; index < first.size(); ++index) {
        EXPECT_EQ(first[index].StableId, second[index].StableId);
        EXPECT_EQ(first[index].LocalPosition, second[index].LocalPosition);
    }
}

TEST(Oot3dGrassThinning, AMaskThatRejectsHalfStillFillsTheBudget) {
    const auto field = MakeField(40U, 2000.0F);
    const auto anchors =
        GrassSurfaceExtractor::Extract(Surface(field), FullRule(), SwitchGeneration(), Mask(128U), 2000U);
    // The candidates are sampled with headroom, so the survivors of a 50%
    // mask still reach the cap instead of half of it.
    EXPECT_EQ(anchors.size(), 2000U);
    const auto counts = CellCounts(anchors, 2000.0F, 10U);
    EXPECT_EQ(std::count(counts.begin(), counts.end(), 0U), 0);
}

TEST(Oot3dGrassThinning, BelowTheBudgetNothingChanges) {
    const auto field = MakeField(10U, 500.0F); // 25 m2: ~1000 candidates
    const auto generation = SwitchGeneration();
    const auto free = GrassSurfaceExtractor::Extract(Surface(field), FullRule(), generation, Mask(255U), 1000000U);
    ASSERT_GT(free.size(), 500U);
    const auto exact = GrassSurfaceExtractor::Extract(Surface(field), FullRule(), generation, Mask(255U),
                                                      static_cast<uint32_t>(free.size()));
    ASSERT_EQ(exact.size(), free.size());
    for (size_t index = 0; index < free.size(); ++index) EXPECT_EQ(exact[index].StableId, free[index].StableId);
}

TEST(Oot3dGrassThinning, RankThinningKeepsOrderAndIgnoresPosition) {
    // Anchors sorted by X (as a triangle-order fill would produce): thinning
    // must keep both halves alike and the survivors in their order.
    std::vector<GrassAnchor> anchors(10000);
    for (uint32_t index = 0; index < anchors.size(); ++index) {
        anchors[index].LocalPosition = {static_cast<float>(index), 0.0F, 0.0F};
        anchors[index].StableId = index * 2654435761U + 1U;
    }
    auto thinned = anchors;
    ThinGrassAnchorsToLimit(thinned, 1000U);
    ASSERT_EQ(thinned.size(), 1000U);
    EXPECT_TRUE(std::is_sorted(thinned.begin(), thinned.end(), [](const auto& a, const auto& b) {
        return a.LocalPosition[0] < b.LocalPosition[0];
    }));
    const auto firstHalf = std::count_if(thinned.begin(), thinned.end(),
                                         [](const auto& a) { return a.LocalPosition[0] < 5000.0F; });
    EXPECT_NEAR(static_cast<double>(firstHalf), 500.0, 60.0);
    auto again = anchors;
    ThinGrassAnchorsToLimit(again, 1000U);
    for (size_t index = 0; index < thinned.size(); ++index) EXPECT_EQ(again[index].StableId, thinned[index].StableId);
    auto small = anchors;
    small.resize(10);
    ThinGrassAnchorsToLimit(small, 1000U);
    EXPECT_EQ(small.size(), 10U);
    ThinGrassAnchorsToLimit(small, 0U);
    EXPECT_TRUE(small.empty());
}

// ---- profile and config/grass ----------------------------------------------

TEST(Oot3dGrassSwitchProfile, SixtyFpsBudget) {
    const auto profile = SelectGrassSwitchProfile(true, GrassSwitchDistance::Near);
    EXPECT_EQ(profile.MaxVisibleInstances, 8000U);
    // Full density near the camera, thinning with distance instead of a
    // hard cut at 1200 (the title demo's far camera saw nothing).
    EXPECT_EQ(profile.FullDensityDistance, 1000.0F);
    EXPECT_EQ(profile.DrawDistance, 4000.0F);
    EXPECT_GT(profile.FadeStart, profile.FullDensityDistance);
    EXPECT_LT(profile.FadeStart, profile.DrawDistance);
    EXPECT_EQ(profile.SingleBladeDistance, 400.0F); // 4 m
    EXPECT_EQ(profile.TuftsPerSquareMeter, 40.0F);
    EXPECT_GE(profile.BladesPerTuft, 4U);
    EXPECT_LE(profile.BladesPerTuft, 5U);
}

TEST(Oot3dGrassSwitchProfile, ThirtyFpsBudget) {
    const auto profile = SelectGrassSwitchProfile(false, GrassSwitchDistance::Near);
    EXPECT_GE(profile.MaxVisibleInstances, 20000U);
    EXPECT_LE(profile.MaxVisibleInstances, 25000U);
    EXPECT_EQ(profile.FullDensityDistance, 1600.0F);
    EXPECT_EQ(profile.DrawDistance, 5000.0F);
    EXPECT_LT(profile.FadeStart, profile.DrawDistance);
    // Placement and memory do not depend on the frame rate.
    const auto sixty = SelectGrassSwitchProfile(true, GrassSwitchDistance::Near);
    EXPECT_EQ(profile.TuftsPerSquareMeter, sixty.TuftsPerSquareMeter);
    EXPECT_EQ(profile.RetainedBytes, sixty.RetainedBytes);
}

TEST(Oot3dGrassSwitchProfile, RetainedMemoryHoldsHyruleFieldAtTheProfileDensity) {
    const auto profile = SelectGrassSwitchProfile(true, GrassSwitchDistance::Near);
    // ~4460 m2 of grass sources (the PC's 4.68 M anchors at ~1049 per m2).
    const double tufts = 4460.0 * profile.TuftsPerSquareMeter;
    const double bytes = tufts * static_cast<double>(GrassRetainedBytesPerAnchor());
    EXPECT_EQ(GrassRetainedBytesPerAnchor(), 64U);
    EXPECT_LT(bytes, static_cast<double>(profile.RetainedBytes)); // ~11 MB
    EXPECT_LE(profile.RetainedBytes, 16U * 1024U * 1024U);
    // A cap that bites in one placement still leaves room for two of them.
    EXPECT_LE(2.0 * profile.MaxAnchorsPerPlacement * GrassRetainedBytesPerAnchor(),
              static_cast<double>(profile.RetainedBytes) + 1.0);
}

TEST(Oot3dGrassSwitchProfile, AppliesOnTopOfThePresetRules) {
    InteractiveGrassSettings settings;
    settings.Rules.push_back(FullRule());
    const auto profile = SelectGrassSwitchProfile(true, GrassSwitchDistance::Near);
    ApplyGrassSwitchProfile(profile, settings);
    EXPECT_EQ(settings.Rules.size(), 1U);
    EXPECT_EQ(settings.MaxInstancesPerRoom, profile.MaxAnchorsPerPlacement);
    EXPECT_EQ(settings.Generation.InstancesPerSquareMeter, 40.0F);
    EXPECT_EQ(settings.DrawDistance, 4000.0F);
    EXPECT_FALSE(settings.MidrangeClustersEnabled);
    EXPECT_GT(settings.Appearance.TextureColorInfluence, 0.0F);
    const auto stream = GrassStreamSettingsFromProfile(profile);
    EXPECT_EQ(stream.MaxInstances, profile.MaxVisibleInstances);
    EXPECT_EQ(stream.DrawDistance, profile.DrawDistance);
    EXPECT_EQ(stream.FullDensityDistance, profile.FullDensityDistance);
}

// L3+R3 > Video > FPS while the game runs (Deko3dSetGrassFrameRate): the
// rates differ only in what the cull reads per job (distances, visible cap)
// and the preset fields ApplyGrassSwitchProfile overwrites, so a switch
// leaves the settings of a start at the new rate; the stream memory holds
// either cap.
TEST(Oot3dGrassSwitchProfile, FrameRateSwitchesLiveAsAStartWould) {
    const auto sixty = SelectGrassSwitchProfile(true, GrassSwitchDistance::Near);
    const auto thirty = SelectGrassSwitchProfile(false, GrassSwitchDistance::Near);
    // Placement-time fields (generation, caps, memory) are the same.
    EXPECT_EQ(sixty.TuftsPerSquareMeter, thirty.TuftsPerSquareMeter);
    EXPECT_EQ(sixty.MinimumSpacing, thirty.MinimumSpacing);
    EXPECT_EQ(sixty.TuftHeightMin, thirty.TuftHeightMin);
    EXPECT_EQ(sixty.TuftHeightMax, thirty.TuftHeightMax);
    EXPECT_EQ(sixty.BladeWidthMin, thirty.BladeWidthMin);
    EXPECT_EQ(sixty.BladeWidthMax, thirty.BladeWidthMax);
    EXPECT_EQ(sixty.BladesPerTuft, thirty.BladesPerTuft);
    EXPECT_EQ(sixty.ClusterSize, thirty.ClusterSize);
    EXPECT_EQ(sixty.MaxAnchorsPerPlacement, thirty.MaxAnchorsPerPlacement);
    EXPECT_EQ(sixty.RetainedBytes, thirty.RetainedBytes);
    for (const bool to : {false, true}) {
        InteractiveGrassSettings switched;
        switched.Rules.push_back(FullRule());
        ApplyGrassSwitchProfile(SelectGrassSwitchProfile(!to, GrassSwitchDistance::Near), switched);
        ApplyGrassSwitchProfile(SelectGrassSwitchProfile(to, GrassSwitchDistance::Near), switched);
        InteractiveGrassSettings start;
        start.Rules.push_back(FullRule());
        ApplyGrassSwitchProfile(SelectGrassSwitchProfile(to, GrassSwitchDistance::Near), start);
        EXPECT_EQ(switched.Rules.size(), start.Rules.size());
        EXPECT_EQ(switched.MaxInstancesPerRoom, start.MaxInstancesPerRoom);
        EXPECT_EQ(switched.DrawDistance, start.DrawDistance);
        EXPECT_EQ(switched.LodReferenceDistance, start.LodReferenceDistance);
        EXPECT_EQ(switched.CullingClusterSize, start.CullingClusterSize);
        EXPECT_EQ(switched.FarTuftBladeCount, start.FarTuftBladeCount);
        EXPECT_EQ(switched.Generation.InstancesPerSquareMeter, start.Generation.InstancesPerSquareMeter);
        EXPECT_EQ(switched.Generation.BladeHeightMax, start.Generation.BladeHeightMax);
        EXPECT_EQ(switched.Generation.BladeWidthMax, start.Generation.BladeWidthMax);
        EXPECT_EQ(switched.Appearance.TextureColorInfluence, start.Appearance.TextureColorInfluence);
        const auto stream = GrassStreamSettingsFromProfile(SelectGrassSwitchProfile(to, GrassSwitchDistance::Near));
        EXPECT_EQ(stream.DrawDistance, switched.DrawDistance);
        EXPECT_LE(stream.MaxInstances, GrassSwitchStreamCapacity());
    }
    EXPECT_GE(GrassSwitchStreamCapacity(), std::max(sixty.MaxVisibleInstances, thirty.MaxVisibleInstances));
}

TEST(Oot3dGrassSwitchProfile, ConfigFileMeaning) {
    EXPECT_EQ(ParseGrassSwitchMode(std::nullopt), GrassSwitchMode::Off);
    EXPECT_EQ(ParseGrassSwitchMode(""), GrassSwitchMode::Placements); // the empty file of before
    EXPECT_EQ(ParseGrassSwitchMode("placements"), GrassSwitchMode::Placements);
    EXPECT_EQ(ParseGrassSwitchMode(" draw\r\n"), GrassSwitchMode::Draw);
    EXPECT_EQ(ParseGrassSwitchMode("0"), GrassSwitchMode::Off);
    EXPECT_EQ(ParseGrassSwitchMode("off"), GrassSwitchMode::Off);
    const auto dir = std::filesystem::temp_directory_path() / "oot3d_grass_switch_tests";
    std::filesystem::remove_all(dir);
    EXPECT_EQ(ReadGrassSwitchMode(dir), GrassSwitchMode::Off);
    for (auto mode : {GrassSwitchMode::Draw, GrassSwitchMode::Placements, GrassSwitchMode::Off}) {
        ASSERT_TRUE(SaveGrassSwitchMode(dir, mode));
        EXPECT_EQ(ReadGrassSwitchMode(dir), mode);
    }
    EXPECT_FALSE(std::filesystem::exists(dir / "grass"));
    std::filesystem::remove_all(dir);
}

// L3+R3 > Video > "Grass distance": Near is the profile of before, Far
// doubles the full-density distance, Whole area keeps every tuft. Only
// what each cull job reads changes, so the switch applies at once.
TEST(Oot3dGrassSwitchProfile, GrassDistanceModes) {
    for (const bool fps60 : {true, false}) {
        const auto near = SelectGrassSwitchProfile(fps60, GrassSwitchDistance::Near);
        const auto far = SelectGrassSwitchProfile(fps60, GrassSwitchDistance::Far);
        const auto area = SelectGrassSwitchProfile(fps60, GrassSwitchDistance::Area);
        EXPECT_EQ(near.FullDensityDistance, fps60 ? 1000.0F : 1600.0F);
        EXPECT_EQ(near.MaxVisibleInstances, fps60 ? 8000U : 22000U);
        EXPECT_EQ(far.FullDensityDistance, 2.0F * near.FullDensityDistance);
        EXPECT_GT(far.DrawDistance, near.DrawDistance);
        EXPECT_GT(far.FadeStart, far.FullDensityDistance);
        EXPECT_LT(far.FadeStart, far.DrawDistance);
        EXPECT_GT(far.MaxVisibleInstances, near.MaxVisibleInstances);
        EXPECT_EQ(area.FullDensityDistance, kGrassWholeAreaDistance);
        EXPECT_EQ(area.DrawDistance, kGrassWholeAreaDistance);
        EXPECT_EQ(area.MaxVisibleInstances, GrassSwitchStreamCapacity());
        for (const auto& profile : {near, far, area}) {
            // Placement fields are the same: no placement is rebuilt.
            EXPECT_EQ(profile.TuftsPerSquareMeter, near.TuftsPerSquareMeter);
            EXPECT_EQ(profile.MaxAnchorsPerPlacement, near.MaxAnchorsPerPlacement);
            EXPECT_EQ(profile.RetainedBytes, near.RetainedBytes);
            EXPECT_EQ(profile.ClusterSize, near.ClusterSize);
            EXPECT_LE(profile.MaxVisibleInstances, GrassSwitchStreamCapacity());
            InteractiveGrassSettings settings;
            ApplyGrassSwitchProfile(profile, settings);
            InteractiveGrassSettings reference;
            ApplyGrassSwitchProfile(near, reference);
            EXPECT_EQ(settings.Generation.InstancesPerSquareMeter, reference.Generation.InstancesPerSquareMeter);
            EXPECT_EQ(settings.MaxInstancesPerRoom, reference.MaxInstancesPerRoom);
        }
    }
    // The stream memory: 4 slots of the Whole area cap, 32 bytes each = 8 MiB.
    EXPECT_EQ(GrassSwitchStreamCapacity(), kGrassWholeAreaInstances);
    EXPECT_EQ(GrassStreamSlots::kSlots * GrassSwitchStreamCapacity() * sizeof(GrassStreamInstance),
              8U * 1024U * 1024U);
}

TEST(Oot3dGrassSwitchProfile, GrassDistanceConfigFile) {
    EXPECT_EQ(ParseGrassSwitchDistance(std::nullopt), GrassSwitchDistance::Near);
    EXPECT_EQ(ParseGrassSwitchDistance(""), GrassSwitchDistance::Near);
    EXPECT_EQ(ParseGrassSwitchDistance(" far\r\n"), GrassSwitchDistance::Far);
    EXPECT_EQ(ParseGrassSwitchDistance("area"), GrassSwitchDistance::Area);
    EXPECT_EQ(ParseGrassSwitchDistance("everything"), GrassSwitchDistance::Near);
    EXPECT_EQ(GrassSwitchDistanceWord(GrassSwitchDistance::Near), "near");
    EXPECT_EQ(GrassSwitchDistanceWord(GrassSwitchDistance::Far), "far");
    EXPECT_EQ(GrassSwitchDistanceWord(GrassSwitchDistance::Area), "area");
    const auto dir = std::filesystem::temp_directory_path() / "oot3d_grass_distance_tests";
    std::filesystem::remove_all(dir);
    EXPECT_EQ(ReadGrassSwitchDistance(dir), GrassSwitchDistance::Near);
    for (auto distance : {GrassSwitchDistance::Area, GrassSwitchDistance::Far, GrassSwitchDistance::Near}) {
        ASSERT_TRUE(SaveGrassSwitchDistance(dir, distance));
        EXPECT_EQ(ReadGrassSwitchDistance(dir), distance);
    }
    EXPECT_FALSE(std::filesystem::exists(dir / "grass_distance"));
    std::filesystem::remove_all(dir);
    // The menu's live choice reaches the renderer.
    EXPECT_FALSE(PublishedGrassSwitchDistance().has_value());
    PublishGrassSwitchDistance(GrassSwitchDistance::Area);
    EXPECT_EQ(PublishedGrassSwitchDistance(), GrassSwitchDistance::Area);
    PublishGrassSwitchDistance(GrassSwitchDistance::Near);
    EXPECT_EQ(PublishedGrassSwitchDistance(), GrassSwitchDistance::Near);
}

// ---- culling and the instance stream ----------------------------------------

// Camera at `eye` looking along +Z: clip x = f x, y = f y, w = z (column-major).
std::array<float, 16> LookAlongZ(const std::array<float, 3>& eye) {
    const float f = 1.0F;
    std::array<float, 16> m{};
    m[0] = f;                 // x row
    m[5] = f;                 // y row
    m[10] = 1.0F;             // z row (unused by the culling)
    m[11] = 1.0F;             // w row: w = z
    m[12] = -f * eye[0];
    m[13] = -f * eye[1];
    m[14] = -eye[2];
    m[15] = -eye[2];
    return m;
}

// Clusters of 4 anchors on a grid of `step` world units from -extent to extent.
std::shared_ptr<GrassWorldPlacement> GridPlacement(float extent, float step, uint64_t identity) {
    auto placement = std::make_shared<GrassWorldPlacement>();
    placement->Identity = identity;
    uint32_t id = 1U;
    for (float z = -extent; z <= extent; z += step)
        for (float x = -extent; x <= extent; x += step) {
            GrassWorldCluster cluster;
            cluster.FirstAnchor = static_cast<uint32_t>(placement->Anchors.size());
            cluster.AnchorCount = 4U;
            cluster.Center = {x, 0.0F, z};
            cluster.Radius = step * 0.75F + 30.0F;
            cluster.MaximumBladeHeight = 30.0F;
            for (int corner = 0; corner < 4; ++corner) { // stable visibility ascending, as built
                const float visibility = (static_cast<float>(corner) + 0.5F) / 4.0F;
                GrassWorldAnchor anchor;
                anchor.BaseHeight = {x + (corner & 1 ? 0.25F : -0.25F) * step, 0.0F,
                                     z + (corner & 2 ? 0.25F : -0.25F) * step, 20.0F};
                anchor.HalfWidthPhase = {1.5F, 1.0F};
                anchor.PackedWidthAxis = PackGrassDirection({1.0F, 0.0F});
                anchor.PackedWorldNormal = PackGrassNormal({0.0F, 1.0F, 0.0F});
                anchor.StableId = id++;
                anchor.SurfaceColor = 0xff00ff00U;
                placement->Anchors.push_back(anchor);
                placement->CullingAnchors.push_back(
                    {{anchor.BaseHeight[0], anchor.BaseHeight[1], anchor.BaseHeight[2]}, visibility});
            }
            placement->Clusters.push_back(cluster);
        }
    return placement;
}

float Distance(const GrassStreamInstance& instance, const std::array<float, 3>& eye) {
    const float dx = instance.PositionHeight[0] - eye[0], dy = instance.PositionHeight[1] - eye[1],
                dz = instance.PositionHeight[2] - eye[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

TEST(Oot3dGrassInstanceStream, CullsByFrustumAndDistanceAndSplitsNearBlades) {
    const std::array<float, 3> eye{0.0F, 100.0F, 0.0F};
    GrassStreamPlacement placement{GridPlacement(3000.0F, 100.0F, 9U), LookAlongZ(eye), eye};
    GrassStreamSettings settings = GrassStreamSettingsFromProfile(SelectGrassSwitchProfile(true, GrassSwitchDistance::Near));
    settings.MaxInstances = 1000000U;
    settings.FillDistance = 0.0F; // the placement's own tufts (fill: Oot3dGrassNearDensity)
    std::vector<GrassStreamInstance> output(settings.MaxInstances);
    std::vector<GrassStreamRange> ranges;
    GrassInstanceStreamBuilder builder;
    const auto stats = builder.Build({&placement, 1}, settings, output, ranges);
    ASSERT_GT(stats.Tufts, 0U);
    ASSERT_GT(stats.Blades, 0U);
    EXPECT_LT(stats.ClustersVisible, stats.ClustersTested / 2U); // behind the camera and far away: skipped
    // Ranges: grouped by kind, contiguous from 0.
    uint32_t next = 0U;
    for (const auto& range : ranges) {
        EXPECT_EQ(range.First, next);
        EXPECT_EQ(range.Placement, 0U);
        for (uint32_t index = range.First; index < range.First + range.Count; ++index) {
            const float distance = Distance(output[index], eye);
            EXPECT_LE(distance, settings.DrawDistance + 1.0e-3F);
            if (range.Kind == GrassStreamKind::Blades) EXPECT_LT(distance, settings.SingleBladeDistance);
            else EXPECT_GE(distance, settings.SingleBladeDistance);
            // Nothing well behind the camera (the margin keeps a little).
            EXPECT_GT(output[index].PositionHeight[2], -settings.CullMargin - 200.0F);
            const uint32_t fade = output[index].WidthPhaseFade >> 24U;
            // Full height until the thinning starts (a kept tuft grows over
            // the last kGrassThinGrowth of its distance), shrinking past FadeStart.
            if (distance <= settings.FullDensityDistance / (1.0F + kGrassThinGrowth)) EXPECT_EQ(fade, 255U);
            if (distance > settings.FadeStart) EXPECT_LT(fade, 255U);
        }
        next += range.Count;
    }
    EXPECT_EQ(next, stats.Tufts + stats.Blades);
    EXPECT_EQ(stats.Fill, 0U);
    EXPECT_EQ(stats.ClustersOverBudget, 0U);
}

TEST(Oot3dGrassInstanceStream, TheBudgetDropsTheFarthestClusters) {
    const std::array<float, 3> eye{0.0F, 100.0F, 0.0F};
    GrassStreamPlacement placement{GridPlacement(3000.0F, 100.0F, 9U), LookAlongZ(eye), eye};
    auto settings = GrassStreamSettingsFromProfile(SelectGrassSwitchProfile(true, GrassSwitchDistance::Near));
    std::vector<GrassStreamInstance> unlimited(1000000U);
    std::vector<GrassStreamRange> ranges;
    GrassInstanceStreamBuilder builder;
    settings.MaxInstances = 1000000U;
    const auto all = builder.Build({&placement, 1}, settings, unlimited, ranges);
    const uint32_t total = all.Tufts + all.Blades;
    settings.MaxInstances = total / 3U;
    std::vector<GrassStreamInstance> output(settings.MaxInstances);
    const auto capped = builder.Build({&placement, 1}, settings, output, ranges);
    EXPECT_EQ(capped.Tufts + capped.Blades, settings.MaxInstances);
    EXPECT_GT(capped.ClustersOverBudget, 0U);
    // What is kept is the near grass: the farthest kept tuft is nearer than
    // the farthest one without the cap.
    float keptFarthest = 0.0F, allFarthest = 0.0F;
    for (uint32_t index = 0; index < settings.MaxInstances; ++index)
        keptFarthest = std::max(keptFarthest, Distance(output[index], eye));
    for (uint32_t index = 0; index < total; ++index)
        allFarthest = std::max(allFarthest, Distance(unlimited[index], eye));
    EXPECT_LT(keptFarthest, allFarthest * 0.9F);
    // Every near blade survives the cap.
    EXPECT_EQ(capped.Blades, all.Blades);
}

TEST(Oot3dGrassInstanceStream, RangesPerPlacementAndDeterministic) {
    const std::array<float, 3> eye{0.0F, 100.0F, 0.0F};
    std::array<GrassStreamPlacement, 2> placements{
        GrassStreamPlacement{GridPlacement(1000.0F, 100.0F, 1U), LookAlongZ(eye), eye},
        GrassStreamPlacement{GridPlacement(1000.0F, 150.0F, 2U), LookAlongZ(eye), eye}};
    auto settings = GrassStreamSettingsFromProfile(SelectGrassSwitchProfile(false, GrassSwitchDistance::Near));
    std::vector<GrassStreamInstance> first(settings.MaxInstances), second(settings.MaxInstances);
    std::vector<GrassStreamRange> firstRanges, secondRanges;
    GrassInstanceStreamBuilder builder;
    const auto a = builder.Build(placements, settings, first, firstRanges);
    const auto b = builder.Build(placements, settings, second, secondRanges);
    std::set<uint32_t> seen;
    for (const auto& range : firstRanges) seen.insert(range.Placement);
    EXPECT_EQ(seen, (std::set<uint32_t>{0U, 1U}));
    ASSERT_EQ(firstRanges.size(), secondRanges.size());
    for (size_t index = 0; index < firstRanges.size(); ++index) {
        EXPECT_EQ(firstRanges[index].First, secondRanges[index].First);
        EXPECT_EQ(firstRanges[index].Count, secondRanges[index].Count);
    }
    const uint32_t total = a.Tufts + a.Blades;
    EXPECT_EQ(total, b.Tufts + b.Blades);
    EXPECT_EQ(std::memcmp(first.data(), second.data(), total * sizeof(GrassStreamInstance)), 0);
    // The output span bounds the stream too.
    std::vector<GrassStreamInstance> tiny(10);
    const auto bounded = builder.Build(placements, settings, tiny, firstRanges);
    EXPECT_EQ(bounded.Tufts + bounded.Blades, 10U);
}

TEST(Oot3dGrassInstanceStream, InstancePacking) {
    for (float value : {0.9F, 1.5F, 2.25F, 3.0F, 17.0F}) {
        EXPECT_NEAR(UnpackGrassHalf(PackGrassHalf(value)), value, value * 1.0e-3F);
    }
    EXPECT_EQ(UnpackGrassHalf(PackGrassHalf(0.0F)), 0.0F);
    GrassWorldAnchor anchor;
    anchor.BaseHeight = {1.0F, 2.0F, 3.0F, 25.0F};
    anchor.HalfWidthPhase = {1.25F, 3.14159265F};
    anchor.PackedWidthAxis = 0x12345678U;
    anchor.PackedWorldNormal = 0x9abcdef0U;
    anchor.SurfaceColor = 0x80402010U;
    const auto instance = PackGrassStreamInstance(anchor, 0.5F);
    EXPECT_EQ(instance.PositionHeight, anchor.BaseHeight);
    EXPECT_EQ(instance.PackedAxis, anchor.PackedWidthAxis);
    EXPECT_EQ(instance.PackedNormal, anchor.PackedWorldNormal);
    EXPECT_EQ(instance.SurfaceColor, anchor.SurfaceColor);
    EXPECT_NEAR(UnpackGrassHalf(static_cast<uint16_t>(instance.WidthPhaseFade & 0xffffU)), 1.25F, 1.0e-3F);
    EXPECT_NEAR(((instance.WidthPhaseFade >> 16U) & 0xffU) / 255.0F, 0.5F, 0.01F);
    EXPECT_EQ(instance.WidthPhaseFade >> 24U, 128U);
}

TEST(Oot3dGrassInstanceStream, NothingWithoutBudgetOrPlacements) {
    GrassInstanceStreamBuilder builder;
    std::vector<GrassStreamInstance> output(100);
    std::vector<GrassStreamRange> ranges{{}};
    GrassStreamSettings settings;
    const auto none = builder.Build({}, settings, output, ranges);
    EXPECT_EQ(none.Tufts + none.Blades, 0U);
    EXPECT_TRUE(ranges.empty());
    const std::array<float, 3> eye{0.0F, 100.0F, 0.0F};
    GrassStreamPlacement placement{GridPlacement(500.0F, 100.0F, 1U), LookAlongZ(eye), eye};
    settings.MaxInstances = 0U;
    EXPECT_EQ(builder.Build({&placement, 1}, settings, output, ranges).Tufts, 0U);
}

// ---- Hyrule Field at Near: the grass around Link (07/11 videos, log16) -----

// A camera like the field camera behind Link: `height` above the ground at
// eye.z, pitched down by `pitchDegrees`, 16:9. Clip w is the view depth.
std::array<float, 16> FieldCamera(const std::array<float, 3>& eye, float pitchDegrees) {
    const float pitch = pitchDegrees * 3.14159265F / 180.0F;
    const float sy = 1.0F / std::tan(22.5F * 3.14159265F / 180.0F); // 45 degrees vertical
    const float sx = sy / (16.0F / 9.0F);
    const std::array<float, 3> right{1.0F, 0.0F, 0.0F}, up{0.0F, std::cos(pitch), std::sin(pitch)},
        forward{0.0F, -std::sin(pitch), std::cos(pitch)};
    const auto dot = [&](const std::array<float, 3>& a) { return a[0] * eye[0] + a[1] * eye[1] + a[2] * eye[2]; };
    std::array<float, 16> m{};
    for (int axis = 0; axis < 3; ++axis) {
        m[axis * 4 + 0] = sx * right[axis];
        m[axis * 4 + 1] = sy * up[axis];
        m[axis * 4 + 2] = forward[axis];
        m[axis * 4 + 3] = forward[axis];
    }
    m[12] = -sx * dot(right);
    m[13] = -sy * dot(up);
    m[14] = -dot(forward);
    m[15] = -dot(forward);
    return m;
}

std::array<float, 4> Clip(const std::array<float, 16>& m, const std::array<float, 3>& p) {
    std::array<float, 4> clip{};
    for (int row = 0; row < 4; ++row)
        clip[row] = m[row] * p[0] + m[4 + row] * p[1] + m[8 + row] * p[2] + m[12 + row];
    return clip;
}

bool OnScreen(const std::array<float, 16>& m, const std::array<float, 3>& p, float edge = 1.0F) {
    const auto clip = Clip(m, p);
    return clip[3] > 1.0F && std::abs(clip[0]) < edge * clip[3] && std::abs(clip[1]) < edge * clip[3];
}

uint32_t Mix(uint32_t value) {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return value;
}

float Unit(uint32_t value) { return static_cast<float>(Mix(value) >> 8U) / static_cast<float>(1U << 24U); }

// A flat field like Hyrule Field's ground placement after its cap: one tuft
// per `spacing` x `spacing` (jittered), the profile's tuft sizes, clusters of
// ClusterSize cells sorted by stable visibility as BuildGrassPlacementSet does.
std::shared_ptr<GrassWorldPlacement> FieldPlacement(float spacing) {
    const auto profile = SelectGrassSwitchProfile(true, GrassSwitchDistance::Near);
    struct Cell {
        std::vector<GrassWorldAnchor> Anchors;
    };
    const float minX = -3000.0F, maxX = 3000.0F, minZ = -1000.0F, maxZ = 6000.0F;
    const auto columns = static_cast<uint32_t>((maxX - minX) / profile.ClusterSize);
    const auto rows = static_cast<uint32_t>((maxZ - minZ) / profile.ClusterSize);
    std::vector<Cell> cells(size_t{columns} * rows);
    uint32_t id = 1U;
    for (float z = minZ + spacing * 0.5F; z < maxZ; z += spacing)
        for (float x = minX + spacing * 0.5F; x < maxX; x += spacing) {
            GrassWorldAnchor anchor;
            const float px = x + (Unit(id * 3U) - 0.5F) * spacing, pz = z + (Unit(id * 3U + 1U) - 0.5F) * spacing;
            const float height = profile.TuftHeightMin + (profile.TuftHeightMax - profile.TuftHeightMin) * Unit(id * 5U);
            const float width = profile.BladeWidthMin + (profile.BladeWidthMax - profile.BladeWidthMin) * Unit(id * 7U);
            const float angle = Unit(id * 11U) * 6.2831853F;
            anchor.BaseHeight = {px, 0.0F, pz, height};
            anchor.HalfWidthPhase = {width * 0.5F, Unit(id * 13U) * 6.2831853F};
            anchor.PackedWidthAxis = PackGrassDirection({std::cos(angle), std::sin(angle)});
            anchor.PackedWorldNormal = PackGrassNormal({0.0F, 1.0F, 0.0F});
            anchor.StableId = Mix(id++) | 1U;
            anchor.SurfaceColor = 0xff20a040U;
            const auto column = std::min(columns - 1U, static_cast<uint32_t>((px - minX) / profile.ClusterSize));
            const auto row = std::min(rows - 1U, static_cast<uint32_t>((pz - minZ) / profile.ClusterSize));
            cells[size_t{row} * columns + column].Anchors.push_back(anchor);
        }
    auto placement = std::make_shared<GrassWorldPlacement>();
    placement->Identity = 77U;
    for (auto& cell : cells) {
        if (cell.Anchors.empty()) continue;
        std::sort(cell.Anchors.begin(), cell.Anchors.end(), [](const auto& a, const auto& b) {
            return GrassStableVisibilityValue(a.StableId) < GrassStableVisibilityValue(b.StableId);
        });
        GrassWorldCluster cluster;
        cluster.FirstAnchor = static_cast<uint32_t>(placement->Anchors.size());
        cluster.AnchorCount = static_cast<uint32_t>(cell.Anchors.size());
        std::array<float, 3> low{1.0e9F, 1.0e9F, 1.0e9F}, high{-1.0e9F, -1.0e9F, -1.0e9F};
        for (const auto& anchor : cell.Anchors)
            for (int axis = 0; axis < 3; ++axis) {
                low[axis] = std::min(low[axis], anchor.BaseHeight[axis]);
                high[axis] = std::max(high[axis], anchor.BaseHeight[axis]);
            }
        for (int axis = 0; axis < 3; ++axis) cluster.Center[axis] = (low[axis] + high[axis]) * 0.5F;
        for (const auto& anchor : cell.Anchors) {
            const float dx = anchor.BaseHeight[0] - cluster.Center[0], dz = anchor.BaseHeight[2] - cluster.Center[2];
            cluster.Radius = std::max(cluster.Radius, std::sqrt(dx * dx + dz * dz));
            cluster.MaximumBladeHeight = std::max(cluster.MaximumBladeHeight, anchor.BaseHeight[3]);
        }
        cluster.Radius += cluster.MaximumBladeHeight;
        cluster.MinimumStableVisibility = GrassStableVisibilityValue(cell.Anchors.front().StableId);
        placement->Clusters.push_back(cluster);
        for (const auto& anchor : cell.Anchors) {
            placement->Anchors.push_back(anchor);
            placement->CullingAnchors.push_back({{anchor.BaseHeight[0], anchor.BaseHeight[1], anchor.BaseHeight[2]},
                                                 GrassStableVisibilityValue(anchor.StableId)});
        }
    }
    return placement;
}

float Smooth01(float x) {
    x = std::clamp(x, 0.0F, 1.0F);
    return x * x * (3.0F - 2.0F * x);
}

// What a drawn instance covers on screen, per steradian: its card (the
// shader's BladesPerTuft blades of the packed half width side by side) times
// its drawn height (grass_vsh.glsl: height * smoothstep(fade)) over the
// squared distance.
float DrawnSize(const GrassStreamInstance& instance) {
    const float halfWidth = UnpackGrassHalf(static_cast<uint16_t>(instance.WidthPhaseFade & 0xffffU));
    const float fade = static_cast<float>(instance.WidthPhaseFade >> 24U) / 255.0F;
    return 2.0F * halfWidth * 5.0F * instance.PositionHeight[3] * Smooth01(fade);
}

// How much of the ground's screen area the grass covers between `near` and
// `far` units from the eye: the drawn sizes over the ground's solid angle
// (a horizontal patch dA at distance d covers dA * height / d^3).
float ScreenCoverage(std::span<const GrassStreamInstance> drawn, const std::array<float, 16>& camera,
                     const std::array<float, 3>& eye, float near, float far) {
    double grass = 0.0;
    for (const auto& instance : drawn) {
        const std::array<float, 3> base{instance.PositionHeight[0], instance.PositionHeight[1],
                                        instance.PositionHeight[2]};
        const float d = Distance(instance, eye);
        if (d < near || d >= far || !OnScreen(camera, base)) continue;
        grass += DrawnSize(instance) / (static_cast<double>(d) * d);
    }
    double ground = 0.0;
    const float step = 5.0F;
    for (float z = eye[2]; z < eye[2] + far; z += step)
        for (float x = -far; x < far; x += step) {
            const std::array<float, 3> point{x + step * 0.5F, 0.0F, z + step * 0.5F};
            const float dx = point[0] - eye[0], dy = point[1] - eye[1], dz = point[2] - eye[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < near || d >= far || !OnScreen(camera, point)) continue;
            ground += step * step * eye[1] / (static_cast<double>(d) * d * d);
        }
    return ground > 0.0 ? static_cast<float>(grass / ground) : 0.0F;
}

// The user (07/11, Hyrule Field at Near): "the amount of grass reduces near
// the character". The placement keeps one density everywhere (its cap thins
// it evenly); past FullDensityDistance the thinning keeps the coverage per
// screen area (fewer, wider tufts), but nearer than that the same tufts per m2
// cover less of the screen the nearer they are (coverage ~ density x
// distance): around Link, ~250 units from the camera, a quarter of the band
// further out. Near the camera the stream adds fill tufts so the coverage
// stays at least that of the full-density band.
TEST(Oot3dGrassNearDensity, AroundLinkTheGrassCoversAsMuchAsFurtherOut) {
    const auto placementWorld = FieldPlacement(30.0F); // ~11 tufts per m2
    const std::array<float, 3> eye{0.0F, 100.0F, 0.0F};
    const auto camera = FieldCamera(eye, 12.0F);
    GrassStreamPlacement placement{placementWorld, camera, eye};
    for (const bool fps60 : {true, false}) {
        auto settings = GrassStreamSettingsFromProfile(SelectGrassSwitchProfile(fps60, GrassSwitchDistance::Near));
        std::vector<GrassStreamInstance> output(GrassSwitchStreamCapacity());
        std::vector<GrassStreamRange> ranges;
        GrassInstanceStreamBuilder builder;
        const auto stats = builder.Build({&placement, 1}, settings, output, ranges);
        EXPECT_EQ(stats.ClustersOverBudget, 0U);
        const std::span<const GrassStreamInstance> drawn(output.data(), stats.Tufts + stats.Blades);
        const float full = settings.FullDensityDistance;
        const float link = ScreenCoverage(drawn, camera, eye, 180.0F, 320.0F);
        const float fewMetres = ScreenCoverage(drawn, camera, eye, 0.6F * full, full);
        const float thinned = ScreenCoverage(drawn, camera, eye, full, 1.6F * full);
        // Cost proxies: instances (vertex work) and drawn screen area (pixel
        // work, ScreenCoverage's units over the whole reach) with and without fill.
        const float area = ScreenCoverage(drawn, camera, eye, 0.0F, settings.DrawDistance);
        auto unfilled = settings;
        unfilled.FillDistance = 0.0F;
        std::vector<GrassStreamInstance> plain(GrassSwitchStreamCapacity());
        const auto plainStats = builder.Build({&placement, 1}, unfilled, plain, ranges);
        const float plainArea = ScreenCoverage(std::span<const GrassStreamInstance>(plain.data(), plainStats.Tufts + plainStats.Blades),
                                               camera, eye, 0.0F, settings.DrawDistance);
        std::cout << "  coverage fps60=" << fps60 << ": around Link " << link << ", " << 0.6F * full << "-" << full
                  << " " << fewMetres << ", " << full << "-" << 1.6F * full << " " << thinned << "; instances "
                  << drawn.size() << " (fill " << stats.Fill << ", left out off screen " << stats.OffScreen
                  << "), screen area " << area << " (without fill " << plainArea << ")\n";
        EXPECT_GT(fewMetres, 0.0F);
        EXPECT_GE(link, 0.9F * fewMetres);
        EXPECT_GE(link, 0.9F * thinned);
    }
}

// Walking through the field: no tuft may appear, vanish or change its drawn
// size at once (popping) as the camera moves, inside the screen.
TEST(Oot3dGrassNearDensity, WalkingChangesNoTuftAtOnce) {
    const auto placementWorld = FieldPlacement(30.0F);
    auto settings = GrassStreamSettingsFromProfile(SelectGrassSwitchProfile(true, GrassSwitchDistance::Near));
    settings.MaxInstances = GrassSwitchStreamCapacity(); // the visible cap is another matter
    struct Seen {
        float Size = 0.0F;
        float Full = 0.0F;
    };
    using Key = std::pair<int64_t, int64_t>;
    const auto key = [](const GrassStreamInstance& instance) {
        return Key{std::llround(instance.PositionHeight[0] * 64.0), std::llround(instance.PositionHeight[2] * 64.0)};
    };
    std::vector<GrassStreamInstance> output(GrassSwitchStreamCapacity());
    std::vector<GrassStreamRange> ranges;
    GrassInstanceStreamBuilder builder;
    std::map<Key, Seen> before;
    std::array<float, 16> previousCamera{};
    float worst = 0.0F;
    uint32_t pops = 0U;
    for (int step = 0; step <= 100; ++step) {
        const std::array<float, 3> eye{0.0F, 100.0F, 4.0F * static_cast<float>(step)};
        const auto camera = FieldCamera(eye, 12.0F);
        GrassStreamPlacement placement{placementWorld, camera, eye};
        const auto stats = builder.Build({&placement, 1}, settings, output, ranges);
        ASSERT_EQ(stats.ClustersOverBudget, 0U);
        std::map<Key, Seen> now;
        for (uint32_t index = 0; index < stats.Tufts + stats.Blades; ++index) {
            const auto& instance = output[index];
            const float height = instance.PositionHeight[3];
            const float halfWidth = UnpackGrassHalf(static_cast<uint16_t>(instance.WidthPhaseFade & 0xffffU));
            now[key(instance)] = {DrawnSize(instance), 2.0F * halfWidth * 5.0F * height};
        }
        if (step != 0) {
            // Inside both screens, away from the edges: no frustum change.
            const auto inside = [&](const Key& k) {
                const std::array<float, 3> p{static_cast<float>(k.first) / 64.0F, 0.0F,
                                             static_cast<float>(k.second) / 64.0F};
                return OnScreen(camera, p, 0.8F) && OnScreen(previousCamera, p, 0.8F);
            };
            const auto compare = [&](const Key& k, const Seen* a, const Seen* b) {
                if (!inside(k)) return;
                const float full = std::max(a ? a->Full : 0.0F, b ? b->Full : 0.0F);
                const float change = std::abs((a ? a->Size : 0.0F) - (b ? b->Size : 0.0F)) / full;
                worst = std::max(worst, change);
                if (change > 0.25F) {
                    ++pops;
                    std::cout << "  POP x=" << k.first / 64.0 << " z=" << k.second / 64.0 << " eyez=" << eye[2]
                              << " a=" << (a ? a->Size / a->Full : -1.0F) << " b=" << (b ? b->Size / b->Full : -1.0F)
                              << "\n";
                }
            };
            for (const auto& [k, seen] : now) {
                const auto found = before.find(k);
                compare(k, found == before.end() ? nullptr : &found->second, &seen);
            }
            for (const auto& [k, seen] : before)
                if (!now.contains(k)) compare(k, &seen, nullptr);
        }
        before = std::move(now);
        previousCamera = camera;
    }
    std::cout << "  walking 400 units in steps of 4: " << pops << " tufts changed by more than a quarter at once, "
              << "largest change " << worst << " of a tuft\n";
    EXPECT_EQ(pops, 0U);
    EXPECT_LE(worst, 0.25F);
}

TEST(Oot3dGrassNearDensity, FillMultiplierFollowsTheDistanceToItsCap) {
    const auto settings = GrassStreamSettingsFromProfile(SelectGrassSwitchProfile(true, GrassSwitchDistance::Near));
    EXPECT_EQ(settings.FillDistance, settings.FullDensityDistance);
    EXPECT_EQ(GrassStreamFillMultiplier(settings, settings.FillDistance), 1.0F);
    EXPECT_EQ(GrassStreamFillMultiplier(settings, 2.0F * settings.FillDistance), 1.0F);
    EXPECT_NEAR(GrassStreamFillMultiplier(settings, 250.0F), 1000.0F / (1.125F * 250.0F), 1.0e-4F);
    EXPECT_EQ(GrassStreamFillMultiplier(settings, 10.0F), 1.0F + static_cast<float>(kGrassMaxFillPerTuft));
    for (float d = 50.0F; d < 1000.0F; d += 50.0F)
        EXPECT_GE(GrassStreamFillMultiplier(settings, d), GrassStreamFillMultiplier(settings, d + 50.0F));
    GrassStreamSettings none = settings;
    none.FillDistance = 0.0F;
    EXPECT_EQ(GrassStreamFillMultiplier(none, 100.0F), 1.0F);
    // Fill never reaches past full density, where the thinning could drop
    // the tuft and its fill tufts with it.
    GrassStreamSettings beyond = settings;
    beyond.FillDistance = 4.0F * settings.FullDensityDistance;
    EXPECT_EQ(GrassStreamFillMultiplier(beyond, settings.FullDensityDistance), 1.0F);
    // Every mode fills to its full-density band; Whole area as Near does.
    for (const bool fps60 : {true, false}) {
        const auto near = SelectGrassSwitchProfile(fps60, GrassSwitchDistance::Near);
        const auto far = SelectGrassSwitchProfile(fps60, GrassSwitchDistance::Far);
        const auto area = SelectGrassSwitchProfile(fps60, GrassSwitchDistance::Area);
        EXPECT_EQ(near.FillDistance, near.FullDensityDistance);
        EXPECT_EQ(far.FillDistance, far.FullDensityDistance);
        EXPECT_EQ(area.FillDistance, near.FillDistance);
    }
}

TEST(Oot3dGrassNearDensity, AFillTuftStaysOnItsTuftsGroundAndNeverMoves) {
    GrassWorldAnchor tuft;
    tuft.BaseHeight = {100.0F, 50.0F, -40.0F, 20.0F};
    tuft.HalfWidthPhase = {1.25F, 2.0F};
    tuft.PackedWidthAxis = PackGrassDirection({1.0F, 0.0F});
    const std::array<float, 3> slope{0.3F, 0.9F, -0.2F};
    tuft.PackedWorldNormal = PackGrassNormal(slope);
    tuft.StableId = 0x1234567U;
    tuft.SurfaceColor = 0xff336699U;
    const auto normal = UnpackGrassNormal(tuft.PackedWorldNormal);
    std::set<std::pair<float, float>> spots;
    for (uint32_t child = 1U; child <= kGrassMaxFillPerTuft; ++child) {
        const auto fill = PackGrassFillInstance(tuft, child, 1.0F, 18.0F);
        const auto again = PackGrassFillInstance(tuft, child, 1.0F, 18.0F);
        EXPECT_EQ(std::memcmp(&fill, &again, sizeof(fill)), 0);
        const float dx = fill.PositionHeight[0] - tuft.BaseHeight[0], dy = fill.PositionHeight[1] - tuft.BaseHeight[1],
                    dz = fill.PositionHeight[2] - tuft.BaseHeight[2];
        EXPECT_LE(std::sqrt(dx * dx + dz * dz), 18.0F + 1.0e-3F);
        EXPECT_NEAR(normal[0] * dx + normal[1] * dy + normal[2] * dz, 0.0F, 1.0e-3F); // on the ground plane
        EXPECT_EQ(fill.SurfaceColor, tuft.SurfaceColor);
        EXPECT_EQ(fill.PackedNormal, tuft.PackedWorldNormal);
        EXPECT_GE(fill.PositionHeight[3], 0.75F * tuft.BaseHeight[3] - 1.0e-3F);
        EXPECT_LE(fill.PositionHeight[3], 1.25F * tuft.BaseHeight[3] + 1.0e-3F);
        EXPECT_EQ(fill.WidthPhaseFade >> 24U, 255U);
        spots.insert({fill.PositionHeight[0], fill.PositionHeight[2]});
        EXPECT_GE(GrassFillStableValue(tuft.StableId, child), 0.0F);
        EXPECT_LT(GrassFillStableValue(tuft.StableId, child), 1.0F);
    }
    EXPECT_EQ(spots.size(), size_t{kGrassMaxFillPerTuft}); // each its own spot
}

TEST(Oot3dGrassNearDensity, FillComesOnlyNearTheCameraAndOnScreen) {
    const auto placementWorld = FieldPlacement(30.0F);
    const std::array<float, 3> eye{0.0F, 100.0F, 0.0F};
    const auto camera = FieldCamera(eye, 12.0F);
    GrassStreamPlacement placement{placementWorld, camera, eye};
    const auto nearSettings = GrassStreamSettingsFromProfile(SelectGrassSwitchProfile(true, GrassSwitchDistance::Near));
    auto settings = nearSettings;
    std::vector<GrassStreamInstance> output(GrassSwitchStreamCapacity());
    std::vector<GrassStreamRange> ranges;
    GrassInstanceStreamBuilder builder;
    const auto with = builder.Build({&placement, 1}, settings, output, ranges);
    const std::vector<GrassStreamInstance> filled(output.begin(), output.begin() + with.Tufts + with.Blades);
    settings.FillDistance = 0.0F;
    const auto without = builder.Build({&placement, 1}, settings, output, ranges);
    EXPECT_EQ(without.Fill, 0U);
    EXPECT_GT(with.Fill, 0U);
    EXPECT_EQ(with.Tufts + with.Blades, without.Tufts + without.Blades + with.Fill);
    EXPECT_EQ(with.OffScreen, without.OffScreen);
    EXPECT_GT(with.OffScreen, 0U); // a visible cluster's tufts off the screen are left out
    // Per band, the tufts drawn per placement tuft follow the multiplier.
    for (const auto& [near, far] : {std::pair{180.0F, 320.0F}, std::pair{400.0F, 600.0F}, std::pair{1100.0F, 2000.0F}}) {
        uint32_t count = 0U, own = 0U;
        for (const auto& instance : filled) {
            const float d = Distance(instance, eye);
            if (d >= near && d < far &&
                OnScreen(camera, {instance.PositionHeight[0], 0.0F, instance.PositionHeight[2]}, 0.9F))
                ++count;
        }
        for (uint32_t index = 0; index < without.Tufts + without.Blades; ++index) {
            const float d = Distance(output[index], eye);
            if (d >= near && d < far &&
                OnScreen(camera, {output[index].PositionHeight[0], 0.0F, output[index].PositionHeight[2]}, 0.9F))
                ++own;
        }
        ASSERT_GT(own, 0U);
        const float expected = GrassStreamFillMultiplier(nearSettings, 0.5F * (near + far));
        std::cout << "  band " << near << "-" << far << ": " << count << " tufts for " << own
                  << " of the placement, multiplier at its middle " << expected << "\n";
        EXPECT_NEAR(static_cast<float>(count) / static_cast<float>(own), expected, 0.25F * expected);
    }
}

} // namespace

namespace {
using namespace Fast::Oot3d;

TEST(Oot3dGrassDistanceThinning, FullNearThinningFarNothingPastTheEnd) {
    GrassStreamSettings settings;
    settings.FullDensityDistance = 700.0F;
    settings.DrawDistance = 3000.0F;
    EXPECT_EQ(GrassStreamRetention(settings, 0.0F), 1.0F);
    EXPECT_EQ(GrassStreamRetention(settings, 700.0F), 1.0F);
    EXPECT_NEAR(GrassStreamRetention(settings, 1400.0F), 0.25F, 1.0e-6F);
    EXPECT_GT(GrassStreamRetention(settings, 2999.0F), 0.0F);
    EXPECT_EQ(GrassStreamRetention(settings, 3001.0F), 0.0F);
    for (float d = 700.0F; d < 3000.0F; d += 100.0F)
        EXPECT_GE(GrassStreamRetention(settings, d), GrassStreamRetention(settings, d + 100.0F));
}

TEST(Oot3dGrassDistanceThinning, AFarCameraStillSeesGrassWithinTheBudget) {
    // The title demo: the grass starts ~1500 units away. A hard 1200 cut
    // drew nothing; thinning keeps some, fewer per area the farther it is.
    const std::array<float, 3> eye{0.0F, 300.0F, -1500.0F};
    GrassStreamPlacement placement{GridPlacement(1500.0F, 100.0F, 3U), LookAlongZ(eye), eye};
    const auto settings = GrassStreamSettingsFromProfile(SelectGrassSwitchProfile(true, GrassSwitchDistance::Near));
    std::vector<GrassStreamInstance> output(settings.MaxInstances);
    std::vector<GrassStreamRange> ranges;
    GrassInstanceStreamBuilder builder;
    const auto stats = builder.Build({&placement, 1}, settings, output, ranges);
    const uint32_t total = stats.Tufts + stats.Blades;
    EXPECT_GT(total, 100U);
    EXPECT_LE(total, settings.MaxInstances);
    EXPECT_GT(stats.AnchorsThinned, 0U);
    // Near band (< 1600) keeps a larger share of its anchors than a far one.
    uint32_t nearCount = 0U, farCount = 0U;
    for (uint32_t index = 0; index < total; ++index) {
        const float d = Distance(output[index], eye);
        EXPECT_LE(d, settings.DrawDistance + 1.0F);
        if (d < 1600.0F) ++nearCount;
        else if (d > 2400.0F && d < 2800.0F) ++farCount;
    }
    EXPECT_GT(nearCount, 0U);
    EXPECT_LT(farCount, nearCount * 4U);
}

TEST(Oot3dGrassHandoff, AFrameAlwaysDrawsTheNewestFinishedCull) {
    GrassStreamSlots slots(2U);
    EXPECT_FALSE(slots.Draw(1).has_value()); // nothing culled yet
    auto first = slots.BeginWrite(1);
    ASSERT_TRUE(first.has_value());
    EXPECT_FALSE(slots.BeginWrite(1).has_value()); // one cull at a time
    slots.FinishWrite(*first, 1);
    // A late worker: frames 2..10 draw the same result, never nothing.
    for (uint64_t frame = 2; frame <= 10; ++frame) {
        const auto drawn = slots.Draw(frame);
        ASSERT_TRUE(drawn.has_value());
        EXPECT_EQ(*drawn, *first);
    }
    EXPECT_EQ(slots.FreshFrames, 1U);
    EXPECT_EQ(slots.StaleFrames, 8U);
    auto second = slots.BeginWrite(11);
    ASSERT_TRUE(second.has_value());
    EXPECT_NE(*second, *first); // never the drawn result
    EXPECT_EQ(*slots.Draw(11), *first); // still the old one while the new one is written
    slots.FinishWrite(*second, 11);
    EXPECT_EQ(*slots.Draw(12), *second);
    EXPECT_EQ(slots.CurrentFrame(), 11U);
}

TEST(Oot3dGrassHandoff, NeverWritesASlotAFrameInFlightReads) {
    GrassStreamSlots slots(2U);
    std::vector<std::pair<uint32_t, uint64_t>> reads; // slot, frame
    for (uint64_t frame = 1; frame < 200; ++frame) {
        // The worker finishes every other frame, sometimes late.
        if (const auto writing = slots.Writing(); writing && frame % 3 != 0) slots.FinishWrite(*writing, frame - 1);
        if (const auto slot = slots.BeginWrite(frame)) {
            for (const auto& [read, readFrame] : reads)
                if (read == *slot) EXPECT_LE(readFrame + 2U, frame) << "slot " << *slot << " frame " << frame;
            EXPECT_FALSE(slots.Current() && *slots.Current() == *slot);
        }
        if (const auto drawn = slots.Draw(frame)) reads.emplace_back(*drawn, frame);
        if (frame > 3) EXPECT_TRUE(slots.Current().has_value());
    }
}

TEST(Oot3dGrassHandoff, APlacementIsKeyedPerRenderTarget) {
    // The same placement drawn into two targets (a native replay and the
    // presented frame) keeps both cameras apart.
    EXPECT_NE(GrassFramePlacementKey(5U, 1U, 0x182447c0U), GrassFramePlacementKey(5U, 2U, 0x182447c0U));
    EXPECT_NE(GrassFramePlacementKey(5U, 2U, 0x182447c0U), GrassFramePlacementKey(5U, 2U, 0x18244800U));
    EXPECT_EQ(GrassFramePlacementKey(5U, 2U, 0x182447c0U), GrassFramePlacementKey(5U, 2U, 0x182447c0U));
}

} // namespace

namespace {
using namespace Fast::Oot3d;

TEST(Oot3dGrassForeignCull, ACullOfAnotherSceneOrPlacementSetIsNeverDrawn) {
    // The title demo's last cull must not be drawn in the temple entrance,
    // even when a surface there is drawn as a grass source.
    const std::vector<uint64_t> cull{11U, 12U};
    const std::set<uint64_t> title{11U, 12U, 13U}, temple{90U};
    const auto has = [](const std::set<uint64_t>& set) { return [&set](uint64_t id) { return set.contains(id); }; };
    const uint32_t field = GrassArea(0x5E, 0), entrance = GrassArea(0x03, 0);
    EXPECT_EQ(GrassCullFitsFrame(field, field, cull, has(title)), GrassCullFit::Draw);
    EXPECT_EQ(GrassCullFitsFrame(field, entrance, cull, has(title)), GrassCullFit::OtherScene);
    EXPECT_EQ(GrassCullFitsFrame(entrance, entrance, cull, has(temple)), GrassCullFit::NoPlacement);
    // Another room of the scene: its placements that this frame draws are.
    EXPECT_EQ(GrassCullFitsFrame(field, GrassArea(0x5E, 1), cull, has(title)), GrassCullFit::Draw);
    EXPECT_EQ(GrassCullFitsFrame(field, GrassArea(0x5E, 1), cull, has(temple)), GrassCullFit::NoPlacement);
    EXPECT_EQ(GrassCullFitsFrame(field, GrassArea(-1, 0), cull, has(title)), GrassCullFit::OtherScene);
    EXPECT_EQ(GrassArea(-1, 3), 0xffffffffU);
    EXPECT_TRUE(ClassifyGrassAreaChange(field, GrassArea(0x5E, 1)) == GrassAreaChange::Room);
    EXPECT_TRUE(ClassifyGrassAreaChange(field, entrance) == GrassAreaChange::Scene);
    EXPECT_TRUE(ClassifyGrassAreaChange(field, GrassArea(-1, -1)) == GrassAreaChange::Scene);
    EXPECT_TRUE(ClassifyGrassAreaChange(entrance, entrance) == GrassAreaChange::None);
    // After a reset nothing is drawn until a new cull finishes.
    GrassStreamSlots slots(2U);
    const auto slot = slots.BeginWrite(1);
    ASSERT_TRUE(slot.has_value());
    slots.FinishWrite(*slot, 1);
    EXPECT_TRUE(slots.Draw(2).has_value());
    slots.Reset();
    EXPECT_FALSE(slots.Draw(3).has_value());
    // The slot read by frame 2 is still protected from the next cull.
    const auto next = slots.BeginWrite(3);
    ASSERT_TRUE(next.has_value());
    EXPECT_NE(*next, *slot);
}

TEST(Oot3dGrassInterpolatedFrames, EveryFrameGetsItsOwnModelToClip) {
    // Packed vertex uniforms: 16 bytes of booleans + 4 integer vec4s, then
    // float vec4s (projection 0-3, view 4-6, model 20-22).
    std::vector<uint8_t> uniforms(80U + 24U * 16U);
    const auto put = [&](size_t vec4, std::array<float, 4> v) {
        std::memcpy(uniforms.data() + 80U + vec4 * 16U, v.data(), 16U);
    };
    put(0, {1, 0, 0, 0}); put(1, {0, 1, 0, 0}); put(2, {0, 0, -1, -1}); put(3, {0, 0, -1, 0});
    put(20, {1, 0, 0, 0}); put(21, {0, 1, 0, 0}); put(22, {0, 0, 1, 0});
    // Two in-between frames: the view moves by 10 then 15 units.
    std::array<float, 16> previous{};
    for (float x : {10.0F, 15.0F}) {
        put(4, {1, 0, 0, -x}); put(5, {0, 1, 0, 0}); put(6, {0, 0, 1, 0});
        const auto clip = GrassRigidModelToClip(uniforms, false);
        ASSERT_TRUE(clip.has_value());
        EXPECT_FLOAT_EQ((*clip)[12], -x); // column 3, row 0: the view's translation
        EXPECT_NE(*clip, previous);
        previous = *clip;
    }
    const auto flipped = GrassRigidModelToClip(uniforms, true);
    ASSERT_TRUE(flipped.has_value());
    EXPECT_EQ((*flipped)[5], -previous[5]);
    EXPECT_FALSE(GrassRigidModelToClip(std::span<const uint8_t>(uniforms).first(100), false).has_value());
}

TEST(Oot3dGrassDistanceThinning, ThinnedTuftsAreWider) {
    EXPECT_EQ(GrassStreamWidthScale(1.0F), 1.0F);
    EXPECT_NEAR(GrassStreamWidthScale(0.25F), 2.0F, 1e-6F);
    EXPECT_EQ(GrassStreamWidthScale(0.01F), kGrassMaxWidthScale);
    GrassWorldAnchor anchor;
    anchor.HalfWidthPhase = {1.5F, 0.0F};
    EXPECT_NEAR(UnpackGrassHalf(static_cast<uint16_t>(PackGrassStreamInstance(anchor, 1.0F, 2.0F).WidthPhaseFade & 0xffffU)),
                3.0F, 1e-2F);
}

TEST(Oot3dGrassDistanceThinning, WholeAreaDrawsEveryTuftAndTheCapKeepsTheNearest) {
    // A field 5000-11000 units ahead, all inside the frustum: Near reaches
    // none of it, Whole area every tuft at full height and width.
    const std::array<float, 3> eye{0.0F, 100.0F, -8000.0F};
    GrassStreamPlacement placement{GridPlacement(3000.0F, 100.0F, 4U), LookAlongZ(eye), eye};
    const size_t anchors = placement.World->Anchors.size();
    std::vector<GrassStreamInstance> output(GrassSwitchStreamCapacity());
    std::vector<GrassStreamRange> ranges;
    GrassInstanceStreamBuilder builder;
    const auto near = builder.Build(
        {&placement, 1}, GrassStreamSettingsFromProfile(SelectGrassSwitchProfile(false, GrassSwitchDistance::Near)),
        output, ranges);
    EXPECT_EQ(near.Tufts + near.Blades, 0U);
    auto settings = GrassStreamSettingsFromProfile(SelectGrassSwitchProfile(true, GrassSwitchDistance::Area));
    const auto area = builder.Build({&placement, 1}, settings, output, ranges);
    ASSERT_LT(anchors, settings.MaxInstances);
    EXPECT_EQ(area.Tufts + area.Blades, anchors);
    EXPECT_EQ(area.AnchorsThinned, 0U);
    EXPECT_EQ(area.ClustersOverBudget, 0U);
    for (size_t index = 0; index < anchors; ++index) {
        EXPECT_EQ(output[index].WidthPhaseFade >> 24U, 255U); // full height
        EXPECT_NEAR(UnpackGrassHalf(static_cast<uint16_t>(output[index].WidthPhaseFade & 0xffffU)), 1.5F, 1e-2F);
    }
    float allFarthest = 0.0F;
    for (size_t index = 0; index < anchors; ++index) allFarthest = std::max(allFarthest, Distance(output[index], eye));
    // A cap that bites keeps the nearest clusters.
    settings.MaxInstances = 2000U;
    const auto capped = builder.Build({&placement, 1}, settings, output, ranges);
    EXPECT_EQ(capped.Tufts + capped.Blades, 2000U);
    EXPECT_GT(capped.ClustersOverBudget, 0U);
    float keptFarthest = 0.0F;
    for (size_t index = 0; index < 2000U; ++index) keptFarthest = std::max(keptFarthest, Distance(output[index], eye));
    EXPECT_LT(keptFarthest, allFarthest * 0.7F);
}

// ---- config/grass_lod (SWITCH_PORT.md "Grass on deko3d: cost per tuft") ----

TEST(Oot3dGrassLod, ConfigFileMeaning) {
    EXPECT_TRUE(ParseGrassSwitchLod(std::nullopt)); // absent: on
    EXPECT_TRUE(ParseGrassSwitchLod(""));
    EXPECT_TRUE(ParseGrassSwitchLod("1\n"));
    EXPECT_TRUE(ParseGrassSwitchLod("on"));
    EXPECT_FALSE(ParseGrassSwitchLod("0\n"));
    EXPECT_FALSE(ParseGrassSwitchLod(" off"));
    EXPECT_FALSE(ParseGrassSwitchLod("false"));
    const auto dir = std::filesystem::temp_directory_path() / "oot3d_grass_lod_config";
    std::filesystem::remove_all(dir);
    EXPECT_TRUE(ReadGrassSwitchLod(dir));
    ASSERT_TRUE(SaveGrassSwitchLod(dir, false));
    EXPECT_FALSE(ReadGrassSwitchLod(dir));
    ASSERT_TRUE(SaveGrassSwitchLod(dir, true));
    EXPECT_FALSE(std::filesystem::exists(dir / "grass_lod"));
    EXPECT_TRUE(ReadGrassSwitchLod(dir));
    std::filesystem::remove_all(dir);
    EXPECT_FALSE(PublishedGrassSwitchLod().has_value());
    PublishGrassSwitchLod(false);
    EXPECT_EQ(PublishedGrassSwitchLod(), false);
    PublishGrassSwitchLod(true);
    EXPECT_EQ(PublishedGrassSwitchLod(), true);
}

// The same samples as tests/switch/test_grass_lod.py, which checks the GLSL
// (grass_lod_coordinate) against them.
TEST(Oot3dGrassLod, LevelCoordinateSamples) {
    struct Sample {
        float Focal, Minimum, Distance, HalfWidth, Coordinate;
    };
    for (const auto& s : {Sample{600.0F, 800.0F, 700.0F, 1.4F, 0.0F}, Sample{600.0F, 800.0F, 900.0F, 1.4F, 0.5F},
                          Sample{600.0F, 800.0F, 1150.0F, 1.75F, 0.9565215706825256F},
                          Sample{600.0F, 800.0F, 1300.0F, 1.4F, 1.0F},
                          Sample{600.0F, 800.0F, 1500.0F, 1.4F, 1.2666666507720947F},
                          Sample{600.0F, 800.0F, 6000.0F, 1.0F, 2.0F}, Sample{900.0F, 800.0F, 1500.0F, 2.5F, 0.0F}}) {
        GrassLodParameters lod;
        lod.Focal = s.Focal;
        lod.MinimumDistance = s.Minimum;
        lod.RampInverse = 2.0F / s.Minimum;
        EXPECT_NEAR(GrassLodCoordinate(lod, s.Distance, s.HalfWidth), s.Coordinate, 1.0e-6F) << s.Distance;
    }
    EXPECT_EQ(GrassLodKind(0.99F), GrassStreamKind::Tufts);
    EXPECT_EQ(GrassLodKind(1.0F), GrassStreamKind::Tufts3);
    EXPECT_EQ(GrassLodKind(2.0F), GrassStreamKind::Tufts1);
    EXPECT_EQ(GrassLodBlades(GrassStreamKind::Blades), 5U);
    EXPECT_EQ(GrassLodTargetBlades(GrassStreamKind::Blades), 5U);
    EXPECT_EQ(GrassLodBlades(GrassStreamKind::Tufts), 5U);
    EXPECT_EQ(GrassLodTargetBlades(GrassStreamKind::Tufts), 3U);
    EXPECT_EQ(GrassLodBlades(GrassStreamKind::Tufts3), 3U);
    EXPECT_EQ(GrassLodTargetBlades(GrassStreamKind::Tufts3), 1U);
    EXPECT_EQ(GrassLodBlades(GrassStreamKind::Tufts1), 1U);
    // Single blades turn into tufts by 600; the levels start past that and
    // twice the cull margin there.
    const auto settings = GrassStreamSettingsFromProfile(SelectGrassSwitchProfile(true, GrassSwitchDistance::Near));
    EXPECT_EQ(GrassLodMinimumDistance(settings), 800.0F);
}

TEST(Oot3dGrassLod, FocalLengthIsAPixelWidthOfA1200By720Target) {
    EXPECT_NEAR(GrassLodFocal(LookAlongZ({0.0F, 100.0F, 0.0F})), 600.0F, 1.0e-3F);
    // 45 degrees vertical, 16:9: 600 x the horizontal scale, whatever the pitch.
    const float sx = 1.0F / std::tan(22.5F * 3.14159265F / 180.0F) / (16.0F / 9.0F);
    EXPECT_NEAR(GrassLodFocal(FieldCamera({0.0F, 100.0F, 0.0F}, 12.0F)), 600.0F * sx, 1.0e-2F);
    EXPECT_EQ(GrassLodFocal(std::array<float, 16>{}), 0.0F);
}

// The levels only regroup the instances: the same instances (bytes) as
// without them, each in the draw its distance (less the cull margin) asks
// for; near Link only single blades and 5-blade tufts.
TEST(Oot3dGrassLod, LevelsRegroupTheSameInstancesByDistance) {
    const auto placementWorld = FieldPlacement(30.0F);
    const std::array<float, 3> eye{0.0F, 100.0F, 0.0F};
    const auto camera = FieldCamera(eye, 12.0F);
    GrassStreamPlacement placement{placementWorld, camera, eye};
    for (const auto distance : {GrassSwitchDistance::Near, GrassSwitchDistance::Far, GrassSwitchDistance::Area}) {
        auto settings = GrassStreamSettingsFromProfile(SelectGrassSwitchProfile(true, distance));
        std::vector<GrassStreamInstance> off(GrassSwitchStreamCapacity()), on(GrassSwitchStreamCapacity());
        std::vector<GrassStreamRange> offRanges, onRanges;
        GrassInstanceStreamBuilder builder;
        const auto offStats = builder.Build({&placement, 1}, settings, off, offRanges);
        settings.Lod = true;
        const auto onStats = builder.Build({&placement, 1}, settings, on, onRanges);
        ASSERT_EQ(offStats.Instances(), onStats.Instances());
        EXPECT_EQ(offStats.Tufts3 + offStats.Tufts1, 0U);
        EXPECT_EQ(offStats.Fill, onStats.Fill);
        const uint32_t count = onStats.Instances();
        const auto bytes = [](const GrassStreamInstance& i) {
            std::array<uint8_t, sizeof(GrassStreamInstance)> b{};
            std::memcpy(b.data(), &i, sizeof(i));
            return b;
        };
        std::multiset<std::array<uint8_t, sizeof(GrassStreamInstance)>> offSet, onSet;
        for (uint32_t i = 0; i < count; ++i) {
            offSet.insert(bytes(off[i]));
            onSet.insert(bytes(on[i]));
        }
        EXPECT_TRUE(offSet == onSet);
        const auto lod = GrassLodParametersFor(settings, camera);
        const float nearEnd = 1.5F * settings.SingleBladeDistance;
        std::array<uint32_t, kGrassStreamKinds> perKind{};
        uint64_t verticesOff = 0U, verticesOn = 0U;
        uint32_t next = 0U;
        for (const auto& range : onRanges) {
            EXPECT_EQ(range.First, next);
            next += range.Count;
            perKind[static_cast<size_t>(range.Kind)] += range.Count;
            verticesOn += uint64_t{3U * GrassLodBlades(range.Kind)} * range.Count;
            for (uint32_t i = range.First; i < range.First + range.Count; ++i) {
                const float d = Distance(on[i], eye);
                const float margin = settings.CullMargin + settings.CullMarginPerDistance * d;
                const float halfWidth = UnpackGrassHalf(static_cast<uint16_t>(on[i].WidthPhaseFade & 0xffffU));
                if (range.Kind == GrassStreamKind::Blades) {
                    EXPECT_LT(d, nearEnd + margin);
                    continue;
                }
                ASSERT_TRUE(d >= nearEnd + margin);
                // The cull's level holds for a camera up to the margin nearer.
                const float coordinate = GrassLodCoordinate(lod, d - margin, halfWidth);
                EXPECT_EQ(range.Kind, GrassLodKind(coordinate));
                const float base = range.Kind == GrassStreamKind::Tufts3 ? 1.0F
                                   : range.Kind == GrassStreamKind::Tufts1 ? 2.0F
                                                                           : 0.0F;
                for (const float nearer : {0.0F, 0.5F * margin, margin})
                    EXPECT_GE(GrassLodCoordinate(lod, d - nearer, halfWidth), base);
                // Around Link (within 700 units) every tuft keeps its 5 blades.
                if (d < 700.0F) EXPECT_EQ(range.Kind, GrassStreamKind::Tufts);
            }
        }
        EXPECT_EQ(next, count);
        EXPECT_EQ(perKind[static_cast<size_t>(GrassStreamKind::Blades)] + perKind[0] + perKind[2] + perKind[3], count);
        for (uint32_t i = 0; i < offStats.Instances(); ++i) verticesOff += 15U;
        std::cout << "  lod " << GrassSwitchDistanceWord(distance) << ": " << count << " instances, near "
                  << perKind[static_cast<size_t>(GrassStreamKind::Blades)] << " l5 " << perKind[0] << " l3 "
                  << perKind[2] << " l1 " << perKind[3] << ", vertices " << verticesOn << " (test.9 " << verticesOff
                  << ")\n";
        // Whole area: most tufts are far away and have 1 blade.
        if (distance == GrassSwitchDistance::Area) EXPECT_GT(perKind[3], count / 2U);
    }
}

// A cull of the same area and placement is drawn in every frame, whatever
// target the frame draws into (a native replay), with that target's camera;
// another area or placement set is still dropped.
TEST(Oot3dGrassHandoff, ACullIsDrawnIntoATargetWithoutItsOwnCopyOfThePlacement) {
    struct Camera {
        int Frame = 0;
    };
    const uint32_t meadow = GrassArea(0x56, 0), field = GrassArea(0x51, 0);
    const std::vector<uint64_t> cull{5U};
    constexpr uint64_t kReal = 1U, kBetween = 2U;
    constexpr uint32_t kRealColor = 0x18000000U, kBetweenColor = 0x18100000U;
    GrassFramePlacements<Camera> frame;
    const auto has = [&](uint64_t identity) { return frame.Has(identity); };
    // The real frame: placement 5 drawn into its target.
    frame.Add(5U, kReal, kRealColor, {1});
    EXPECT_EQ(GrassCullFitsFrame(meadow, meadow, cull, has), GrassCullFit::Draw);
    bool other = true;
    ASSERT_NE(frame.Find(5U, kReal, kRealColor, &other), nullptr);
    EXPECT_FALSE(other);
    // The in-between frame draws the same placement into another target.
    frame.Clear();
    frame.Add(5U, kBetween, kBetweenColor, {2});
    EXPECT_EQ(GrassCullFitsFrame(meadow, meadow, cull, has), GrassCullFit::Draw);
    const auto* own = frame.Find(5U, kBetween, kBetweenColor, &other);
    ASSERT_NE(own, nullptr);
    EXPECT_EQ(own->Frame, 2);
    EXPECT_FALSE(other);
    // Its source keyed by the other target than the one drawn into: still
    // drawn, with this frame's camera.
    const auto* moved = frame.Find(5U, kReal, kRealColor, &other);
    ASSERT_NE(moved, nullptr);
    EXPECT_EQ(moved->Frame, 2);
    EXPECT_TRUE(other);
    // A target with its own copy keeps its own camera.
    frame.Add(5U, kReal, kRealColor, {3});
    EXPECT_EQ(frame.Find(5U, kReal, kRealColor, &other)->Frame, 3);
    EXPECT_FALSE(other);
    EXPECT_EQ(frame.Find(5U, kBetween, kBetweenColor)->Frame, 2);
    // Real foreign culls are still dropped.
    EXPECT_EQ(GrassCullFitsFrame(field, meadow, cull, has), GrassCullFit::OtherScene);
    EXPECT_EQ(GrassCullFitsFrame(meadow, meadow, std::vector<uint64_t>{90U}, has), GrassCullFit::NoPlacement);
    EXPECT_EQ(frame.Find(90U, kReal, kRealColor), nullptr);

    // 600 frames, every other one drawing into another target than the one
    // its source is keyed by, a cull finishing every frame (made with the
    // previous frame's placements): every frame after the first cull draws,
    // each with its own camera.
    GrassStreamSlots slots(2U);
    uint64_t drawn = 0U, dropped = 0U, ownCamera = 0U;
    std::optional<uint32_t> writing;
    for (uint64_t index = 1U; index <= 600U; ++index) {
        const bool between = index % 2U == 0U;
        frame.Clear();
        frame.Add(5U, kReal, kRealColor, {static_cast<int>(index)});
        if (writing) slots.FinishWrite(*writing, index - 1U);
        const auto pick = GrassCullForDrawPoint(slots, index, meadow, meadow, cull, has);
        dropped += pick.Fit && *pick.Fit != GrassCullFit::Draw ? 1U : 0U;
        if (pick.Slot) {
            const auto* placement =
                frame.Find(5U, between ? kBetween : kReal, between ? kBetweenColor : kRealColor);
            if (placement != nullptr) {
                ++drawn;
                ownCamera += placement->Frame == static_cast<int>(index) ? 1U : 0U;
            }
        }
        writing = slots.BeginWrite(index);
    }
    EXPECT_EQ(dropped, 0U);
    EXPECT_EQ(drawn, 599U);
    EXPECT_EQ(ownCamera, drawn);
    // A scene change in the middle drops it.
    EXPECT_EQ(GrassCullFitsFrame(meadow, GrassArea(0x5B, 0), cull, has), GrassCullFit::OtherScene);
}

// fps60 (logs14g, Sacred Forest Meadow, 257/600 frames drawn, 330 culls
// dropped): the in-between frames made grass sources of their own from their
// interpolated draws, whose placement identities the real frame's cull did
// not share, nor the next real frame the in-between's cull: two drops per
// in-between frame. An in-between frame now draws the last real frame's
// placements, each with its own camera, taken from the real frame's source
// draw as drawn in the in-between frame (GrassInBetweenCameras).
TEST(Oot3dGrassInterpolatedFrames, AnInBetweenFrameDrawsTheRealFramesPlacementsWithItsCamera) {
    std::vector<uint8_t> uniforms(80U + 24U * 16U);
    const auto put = [&](size_t vec4, std::array<float, 4> v) {
        std::memcpy(uniforms.data() + 80U + vec4 * 16U, v.data(), 16U);
    };
    // A perspective projection, the view (a turn about y and a translation)
    // and the ground's model transform.
    put(0, {1.2F, 0, 0, 0}); put(1, {0, 2.1F, 0, 0}); put(2, {0, 0, -1.01F, -20.0F}); put(3, {0, 0, -1, 0});
    const auto view = [&](float x, float turn) {
        const float c = std::cos(turn), s = std::sin(turn);
        put(4, {c, 0, s, -x}); put(5, {0, 1, 0, -50.0F}); put(6, {-s, 0, c, -300.0F});
    };
    const auto model = [&](float x, float z) {
        put(20, {1, 0, 0, x}); put(21, {0, 1, 0, 0}); put(22, {0, 0, 1, z});
    };
    // The world-to-clip of a view: its model-to-clip with the identity model.
    const auto worldToClip = [&](float x, float turn) {
        view(x, turn);
        model(0.0F, 0.0F);
        return *GrassRigidModelToClip(uniforms, true);
    };
    const auto groundClip = [&](float x, float turn) {
        view(x, turn);
        model(150.0F, -40.0F);
        return *GrassRigidModelToClip(uniforms, true);
    };
    const auto apply = [](const std::array<float, 16>& m, std::array<float, 4> p) {
        std::array<float, 4> r{};
        for (size_t row = 0; row < 4U; ++row)
            for (size_t k = 0; k < 4U; ++k) r[row] += m[k * 4U + row] * p[k];
        return r;
    };

    struct Placement {
        uint64_t Namespace = 0;
        uint32_t ColorAddress = 0;
        std::array<float, 16> PositionToClip{};
    };
    constexpr uint64_t kVisual = 2U;
    constexpr uint32_t kColor = 0x18000000U;
    const uint64_t groundDraw = GrassFramePlacementKey(0xA5C3D2E1F0ULL, kVisual, kColor);
    const uint64_t otherDraw = GrassFramePlacementKey(0x1234ULL, kVisual, kColor);
    const uint64_t target = GrassFramePlacementKey(0U, kVisual, kColor);
    GrassInBetweenCameras cameras;
    GrassFramePlacements<Placement> real, frame;
    const auto moveTo = [&](uint64_t index) {
        return [&, index](const Placement& placement) -> std::optional<Placement> {
            const auto* change = cameras.Change(index, GrassFramePlacementKey(0U, placement.Namespace,
                                                                              placement.ColorAddress));
            if (change == nullptr) return std::nullopt;
            Placement moved = placement;
            moved.PositionToClip = GrassMultiplyClip(*change, placement.PositionToClip);
            return moved;
        };
    };

    // Real frame 1: placement 5, the ground drawn with the view at x 10.
    real.Add(5U, kVisual, kColor, {kVisual, kColor, worldToClip(10.0F, 0.10F)});
    cameras.RealSource(groundDraw, groundClip(10.0F, 0.10F));
    cameras.EndRealFrame(1U);
    // In-between frame 2: the view half way to the next step. Its own source
    // would have been another placement (identity 9); a draw that was no
    // source of the real frame gives no camera.
    EXPECT_FALSE(cameras.InBetweenDraw(2U, otherDraw, target, uniforms, true));
    EXPECT_EQ(cameras.Change(2U, target), nullptr);
    model(150.0F, -40.0F);
    view(12.5F, 0.13F);
    ASSERT_TRUE(cameras.InBetweenDraw(2U, groundDraw, target, uniforms, true));
    frame.AssignMoved(real, moveTo(2U));
    ASSERT_FALSE(frame.Empty());
    EXPECT_FALSE(frame.Has(9U));
    const auto* moved = frame.Find(5U, kVisual, kColor);
    ASSERT_NE(moved, nullptr);
    // Its camera is the in-between frame's: a point of the ground lands
    // where the in-between frame's own world-to-clip puts it.
    const auto expected = worldToClip(12.5F, 0.13F);
    for (const auto point : {std::array<float, 4>{150.0F, 0.0F, -40.0F, 1.0F}, {900.0F, 30.0F, 2500.0F, 1.0F}}) {
        const auto got = apply(moved->PositionToClip, point), want = apply(expected, point);
        for (size_t i = 0; i < 4U; ++i) EXPECT_NEAR(got[i], want[i], 1e-3F * (1.0F + std::abs(want[i]))) << i;
    }
    // The real frame's own placement is untouched.
    EXPECT_EQ(real.Find(5U, kVisual, kColor)->PositionToClip, worldToClip(10.0F, 0.10F));

    // The cull of the real frame fits the in-between frame; another scene or
    // placement set is still left out.
    const uint32_t meadow = GrassArea(0x56, 0);
    const auto has = [&](uint64_t identity) { return frame.Has(identity); };
    EXPECT_EQ(GrassCullFitsFrame(meadow, meadow, std::vector<uint64_t>{5U}, has), GrassCullFit::Draw);
    EXPECT_EQ(GrassCullFitsFrame(meadow, GrassArea(0x5B, 0), std::vector<uint64_t>{5U}, has),
              GrassCullFit::OtherScene);
    EXPECT_EQ(GrassCullFitsFrame(meadow, meadow, std::vector<uint64_t>{90U}, has), GrassCullFit::NoPlacement);
    // The reference is the real frame before: an in-between frame long after
    // it, or after an area change, has none.
    cameras.Clear();
    EXPECT_EQ(cameras.ReferenceFrame(), ~0ULL);
    EXPECT_FALSE(cameras.InBetweenDraw(3U, groundDraw, target, uniforms, true));

    // 600 frames, real and in-between alternating, a cull finishing every
    // frame with the previous frame's placements. With the in-between
    // frames' own sources (identity 9) the logs' pattern comes back: each
    // in-between frame and the real frame after it drop the cull. With the
    // real frame's placements every frame after the first cull draws.
    for (const bool ownSources : {true, false}) {
        GrassStreamSlots slots(2U);
        std::optional<uint32_t> writing;
        std::vector<uint64_t> cull, posted;
        uint64_t drawn = 0U, dropped = 0U;
        cameras.Clear();
        for (uint64_t index = 1U; index <= 600U; ++index) {
            const bool between = index % 2U == 0U;
            const float x = 10.0F + static_cast<float>(index), turn = 0.002F * static_cast<float>(index);
            if (!between) {
                real.Clear();
                real.Add(5U, kVisual, kColor, {kVisual, kColor, worldToClip(x, turn)});
                cameras.RealSource(groundDraw, groundClip(x, turn));
                cameras.EndRealFrame(index);
                frame = real;
            } else if (ownSources) {
                frame.Clear();
                frame.Add(9U, kVisual, kColor, {kVisual, kColor, worldToClip(x, turn)});
            } else {
                model(150.0F, -40.0F);
                view(x, turn);
                ASSERT_TRUE(cameras.InBetweenDraw(index, groundDraw, target, uniforms, true));
                frame.AssignMoved(real, moveTo(index));
            }
            if (writing) {
                slots.FinishWrite(*writing, index - 1U);
                cull = posted;
            }
            const auto pick = GrassCullForDrawPoint(slots, index, meadow, meadow, cull, has);
            dropped += pick.Fit && *pick.Fit != GrassCullFit::Draw ? 1U : 0U;
            if (pick.Slot) {
                bool any = false;
                for (const auto identity : cull) any |= frame.Find(identity, kVisual, kColor) != nullptr;
                drawn += any ? 1U : 0U;
                if (!ownSources && any) {
                    // With this frame's camera.
                    const auto got = apply(frame.Find(5U, kVisual, kColor)->PositionToClip, {150.0F, 0.0F, -40.0F, 1.0F});
                    const auto want = apply(worldToClip(x, turn), {150.0F, 0.0F, -40.0F, 1.0F});
                    EXPECT_NEAR(got[0], want[0], 1e-3F * (1.0F + std::abs(want[0])));
                    EXPECT_NEAR(got[3], want[3], 1e-3F * (1.0F + std::abs(want[3])));
                }
            }
            posted.clear();
            for (const uint64_t identity : {5U, 9U})
                if (frame.Has(identity)) posted.push_back(identity);
            writing = slots.BeginWrite(index);
        }
        if (ownSources) {
            EXPECT_EQ(dropped, 599U) << "the logs' pattern";
            EXPECT_EQ(drawn, 0U);
        } else {
            EXPECT_EQ(dropped, 0U);
            EXPECT_EQ(drawn, 599U);
        }
    }
}

// log16 (0.1.2-test.8): the grass flickered once on entering the Forest
// Temple. The window of the entry: `area drawn scene 3 room 1, changes 5,
// foreign culls dropped 4 (other area 2, no shared placement 2)`, `no result
// 15`, worker max 5.51 ms; hitches.txt: room 1 at frame 27573. A room change
// dropped the drawn cull although the entrance's ground (room 0) stays drawn
// from room 1, and a cull still in flight from room 0 was dropped on arrival
// as another area's: the frames up to the next cull drew no grass between
// frames with grass. The render thread's order per frame, as in
// gfx_deko3d_grass.cpp: CheckGrassArea, the draw point (TakeCull, then
// GrassCullForDrawPoint), then PostCull.
struct GrassEntrySimulation {
    struct Frame {
        uint32_t Area = 0xffffffffU;
        std::set<uint64_t> Placements; // this frame's (empty: being built)
        bool Sources = true;           // a draw point (grass sources drawn)
        uint32_t CullFrames = 1U;      // the cull posted after it finishes this many frames later
    };
    // test.8: a room change dropped the cull like a scene change, and so did
    // a cull of another room or without a shared placement.
    bool Test8 = false;
    GrassStreamSlots Slots{2U};
    uint32_t Area = 0xffffffffU;
    struct Cull {
        uint32_t Area = 0;
        std::vector<uint64_t> Identities;
    };
    std::optional<Cull> Current;
    struct Job {
        uint32_t Slot = 0;
        uint64_t Frame = 0, Ready = 0;
        Cull Result;
    };
    std::optional<Job> Worker;
    GrassGapCounter Gaps;
    uint64_t NoResult = 0, NoResultAfterGrass = 0, Drawn = 0, ForeignDrawn = 0;
    bool DrewInScene = false;

    void Run(uint64_t index, const Frame& frame) {
        const auto change = ClassifyGrassAreaChange(Area, frame.Area);
        if (change != GrassAreaChange::None) {
            Area = frame.Area;
            if (change == GrassAreaChange::Scene) DrewInScene = false;
            if (change == GrassAreaChange::Scene || Test8) {
                Slots.Reset();
                Current.reset();
            }
        }
        const auto has = [&](uint64_t identity) { return frame.Placements.contains(identity); };
        if (frame.Sources) {
            if (Worker && Worker->Ready <= index) { // TakeCull
                Slots.FinishWrite(Worker->Slot, Worker->Frame);
                Current = Worker->Result;
                Worker.reset();
            }
            if (Test8 && Current &&
                (Current->Area != Area ||
                 GrassCullFitsFrame(Current->Area, Area, Current->Identities, has) != GrassCullFit::Draw)) {
                Slots.Reset();
                Current.reset();
            }
            const std::vector<uint64_t> none;
            const auto pick = GrassCullForDrawPoint(Slots, index, Current ? Current->Area : 0U, Area,
                                                    Current ? Current->Identities : none, has);
            if (pick.Fit == GrassCullFit::OtherScene) Current.reset();
            bool drew = false;
            if (pick.Slot) {
                for (const auto identity : Current->Identities) drew |= has(identity);
                if (!GrassSameScene(Current->Area, Area)) ++ForeignDrawn;
            }
            if (!pick.Fit || pick.Fit == GrassCullFit::OtherScene) {
                ++NoResult;
                NoResultAfterGrass += DrewInScene ? 1U : 0U;
            }
            Gaps.DrawPoint(Area, pick.Slot.has_value(), drew);
            Drawn += drew ? 1U : 0U;
            DrewInScene |= drew;
        }
        // PostCull: the worker takes one job at a time.
        if (!Worker && !frame.Placements.empty()) {
            if (const auto slot = Slots.BeginWrite(index))
                Worker = Job{*slot, index, index + frame.CullFrames,
                             {Area, std::vector<uint64_t>(frame.Placements.begin(), frame.Placements.end())}};
        }
    }
};

TEST(Oot3dGrassAreaChange, EnteringTheForestTempleDrawsItsGrassOnceAndKeepsIt) {
    constexpr uint64_t kMeadow = 0x5600U, kEntrance = 0x0300U; // placement identities
    const uint32_t meadow = GrassArea(0x56, 0), outside = GrassArea(-1, -1);
    const uint32_t room0 = GrassArea(0x03, 0), room1 = GrassArea(0x03, 1);
    const auto frameAt = [&](uint64_t index) {
        GrassEntrySimulation::Frame frame;
        if (index < 100U) {
            frame = {meadow, {kMeadow}};
        } else if (index < 102U) {
            frame = {outside, {kMeadow}}; // the fade-out still draws the meadow
        } else if (index < 105U) {
            frame = {outside, {}, false}; // the load: nothing drawn
        } else if (index < 108U) {
            frame = {room0, {}}; // the entrance's placement being built
        } else if (index < 160U) {
            frame = {room0, {kEntrance}};
        } else if (index < 220U) {
            // Into the corridor (room 1), back to the doorway and in again:
            // the entrance's ground stays drawn from room 1.
            const bool back = index >= 170U && index < 175U;
            frame = {back ? room0 : room1, {kEntrance}};
        } else {
            frame = {room1, {}, false}; // deeper in: no grass source
        }
        // The worker is late around each change (a first cull of up to 5.51 ms).
        if (index == 105U || index == 108U || index == 159U || index == 169U || index == 174U) frame.CullFrames = 2U;
        return frame;
    };
    for (const bool test8 : {true, false}) {
        GrassEntrySimulation simulation;
        simulation.Test8 = test8;
        for (uint64_t index = 1U; index <= 260U; ++index) simulation.Run(index, frameAt(index));
        EXPECT_EQ(simulation.ForeignDrawn, 0U) << (test8 ? "test.8" : "now");
        EXPECT_GT(simulation.NoResult, 0U); // the start of each scene, before its first cull
        if (test8) {
            // The log's flicker: frames without a cull after the entrance's
            // grass appeared, between frames with grass.
            EXPECT_GT(simulation.NoResultAfterGrass, 0U);
            EXPECT_GT(simulation.Gaps.Gaps, 0U);
        } else {
            EXPECT_EQ(simulation.NoResultAfterGrass, 0U);
            EXPECT_EQ(simulation.Gaps.Gaps, 0U);
            // Every draw point with a placement draws from the scene's first
            // cull on: 2-99 in the meadow, 101 (its fade-out outside
            // gameplay: a scene change, a new first cull), 110-219 in the
            // temple.
            EXPECT_EQ(simulation.Drawn, 98U + 1U + 110U);
        }
    }
}

// A cull of another scene is never drawn, even when its placement identity
// is among the frame's; a cull without a shared placement is kept for the
// next frame that has one.
TEST(Oot3dGrassAreaChange, ACullOfAnotherSceneIsNeverDrawnEvenWithASharedPlacement) {
    GrassStreamSlots slots(2U);
    const std::vector<uint64_t> cull{7U};
    const auto has = [](uint64_t identity) { return identity == 7U; };
    const auto slot = slots.BeginWrite(1U);
    ASSERT_TRUE(slot.has_value());
    slots.FinishWrite(*slot, 1U);
    const auto title = GrassArea(0x5E, 0), entrance = GrassArea(0x03, 0);
    auto pick = GrassCullForDrawPoint(slots, 2U, title, entrance, cull, has);
    EXPECT_FALSE(pick.Slot.has_value());
    EXPECT_TRUE(pick.Fit == GrassCullFit::OtherScene);
    EXPECT_FALSE(slots.Current().has_value()); // forgotten, not kept for later frames
    pick = GrassCullForDrawPoint(slots, 3U, title, title, cull, has);
    EXPECT_FALSE(pick.Fit.has_value());
    const auto next = slots.BeginWrite(3U);
    ASSERT_TRUE(next.has_value());
    slots.FinishWrite(*next, 3U);
    pick = GrassCullForDrawPoint(slots, 4U, entrance, entrance, cull, [](uint64_t) { return false; });
    EXPECT_FALSE(pick.Slot.has_value());
    EXPECT_TRUE(pick.Fit == GrassCullFit::NoPlacement);
    pick = GrassCullForDrawPoint(slots, 5U, entrance, GrassArea(0x03, 1), cull, has);
    ASSERT_TRUE(pick.Slot.has_value());
    EXPECT_EQ(*pick.Slot, *next);
}

// The preset's rules as Deko3dConfigureGrass reads them (target, channel,
// slots: what decides which texture has grass).
std::vector<GrassPlacementRule> PresetGrassRules() {
    static constexpr std::string_view kPreset =
#include "triaevum_product_graphics.inc"
        ;
    std::vector<GrassPlacementRule> rules;
    const auto document = nlohmann::json::parse(kPreset);
    for (const auto& source : document.at("Grass").at("Sources")) {
        GrassPlacementRule rule;
        rule.RuleId = source.at("RuleId").get<uint32_t>();
        const auto& target = source.at("Target");
        rule.Target.Rgba8Hash = std::stoull(target.at("Rgba8Hash").get<std::string>(), nullptr, 16);
        rule.Target.Width = target.at("Width").get<uint16_t>();
        rule.Target.Height = target.at("Height").get<uint16_t>();
        rule.Target.MapperSlotMask = target.at("MapperSlotMask").get<uint8_t>();
        const std::array<std::string, 5> channels{"Red", "Green", "Blue", "Alpha", "Luminance"};
        rule.Channel = static_cast<GrassSampleChannel>(
            std::find(channels.begin(), channels.end(), source.at("Channel").get<std::string>()) - channels.begin());
        rules.push_back(rule);
    }
    return rules;
}

// A fresh process: no signature store, nothing drawn before.
void FreshGrassSession() {
    auto& cache = GrassTextureSourceCache::Instance();
    (void)cache.SetSignatureStore({});
    cache.Clear();
    GrassTwinRules::Instance().SetSource(nullptr);
}

// The Forest Temple entrance's ground (bmori1 room 0): its rule texture is
// drawn only in Sacred Forest Meadow (rule 10, spot05).
constexpr uint64_t kForestTempleGround = 0xd885db352c997c7bULL;

// test.7: a save that starts in the Forest Temple drew no grass at its
// entrance until Sacred Forest Meadow had been visited (in that session, or
// in an earlier one that kept the rule texture's signature).
TEST(Oot3dGrassTwinsAtLoad, TheFirstFrameOfAFreshSessionGivesTheForestTempleEntranceItsRule) {
    const auto rules = PresetGrassRules();
    ASSERT_EQ(rules.size(), 25U);
    for (const bool groundDecoded : {true, false}) {
        FreshGrassSession();
        auto& twins = GrassTwinRules::Instance();
        // deko3d decodes the drawn ground before asking (pica_grass_source.cpp);
        // its pixels do not matter while the rule texture's are unknown.
        if (groundDecoded) {
            std::vector<uint8_t> pixels(256U * 256U * 4U);
            for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>((i * 2654435761U) >> 24U);
            GrassTextureSourceCache::Instance().ObserveDecoded(kForestTempleGround, 256U, 256U, pixels);
        }
        EXPECT_TRUE(twins.Configured(rules, kForestTempleGround, 256U, 256U, 0U)) << groundDecoded;
        const auto inherited = twins.Inherited(rules, kForestTempleGround, 256U, 256U);
        ASSERT_TRUE(inherited.has_value());
        EXPECT_EQ(inherited->InheritedFrom, 10U) << "Sacred Forest Meadow's ground, the best twin";
        EXPECT_EQ(inherited->Target.Rgba8Hash, kForestTempleGround);
        EXPECT_EQ(inherited->Channel, GrassSampleChannel::Luminance);
        // The twin's range mapped onto the rule texture's (its own pixels are sampled).
        EXPECT_NEAR(inherited->SampleScale, 1.25F, 1.0e-4F);
        EXPECT_NEAR(inherited->SampleOffset, -0.0402F, 1.0e-3F);
        // The placements build it (PicaGrassPlacements::Update).
        const auto all = twins.AllInherited(rules);
        EXPECT_TRUE(std::any_of(all.begin(), all.end(), [](const GrassPlacementRule& rule) {
            return rule.Target.Rgba8Hash == kForestTempleGround && rule.InheritedFrom == 10U;
        }));
        // A ground that is no twin stays bare.
        EXPECT_FALSE(twins.Configured(rules, 0x0123456789abcdefULL, 256U, 256U, 0U));
    }
    FreshGrassSession();
}

// Every built-in twin is of a rule the preset has, with the channel its
// sample mapping was made for (a preset change without rerunning
// make_twin_table.py leaves twins bare), and grows grass on the first frame.
TEST(Oot3dGrassTwinsAtLoad, EveryBuiltInTwinBelongsToAPresetRule) {
    const auto rules = PresetGrassRules();
    FreshGrassSession();
    auto& twins = GrassTwinRules::Instance();
    const auto known = GrassKnownTwins();
    ASSERT_GT(known.size(), 0U);
    EXPECT_TRUE(std::is_sorted(known.begin(), known.end(), [](const GrassKnownTwin& a, const GrassKnownTwin& b) {
        return std::tie(a.TwinHash, a.TwinWidth, a.TwinHeight) < std::tie(b.TwinHash, b.TwinWidth, b.TwinHeight);
    }));
    for (const auto& twin : known) {
        const auto rule = std::find_if(rules.begin(), rules.end(), [&](const GrassPlacementRule& r) {
            return r.Target.Rgba8Hash == twin.RuleHash && r.Target.Width == twin.RuleWidth &&
                   r.Target.Height == twin.RuleHeight;
        });
        ASSERT_TRUE(rule != rules.end());
        EXPECT_EQ(rule->Channel, twin.Channel) << std::hex << twin.TwinHash;
        EXPECT_GE(twin.Score, kGrassTextureTwinThreshold);
        // The best scoring row is the rule it inherits.
        float best = 0.0F;
        uint64_t bestRule = 0U;
        for (const auto& row : known)
            if (row.TwinHash == twin.TwinHash && row.Score > best) {
                best = row.Score;
                bestRule = row.RuleHash;
            }
        const auto inherited = twins.Inherited(rules, twin.TwinHash, twin.TwinWidth, twin.TwinHeight);
        ASSERT_TRUE(inherited.has_value());
        const auto source = std::find_if(rules.begin(), rules.end(), [&](const GrassPlacementRule& r) {
            return r.RuleId == inherited->InheritedFrom;
        });
        ASSERT_TRUE(source != rules.end());
        EXPECT_EQ(source->Target.Rgba8Hash, bestRule) << std::hex << twin.TwinHash;
    }
    FreshGrassSession();
}

} // namespace
