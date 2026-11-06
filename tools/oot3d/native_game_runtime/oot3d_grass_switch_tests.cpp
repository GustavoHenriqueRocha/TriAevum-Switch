// Grass on the Switch (SWITCH_PORT.md, "Grass on deko3d"): density thinning
// of the placements, the Switch profile's budgets and config/grass, and the
// per-frame culling and instance stream of the deko3d draw.
#include "fast/oot3d/grass_anchor_codec.h"
#include "fast/oot3d/grass_instance_stream.h"
#include "fast/oot3d/grass_stream_handoff.h"
#include "fast/oot3d/grass_surface_extractor.h"
#include "fast/oot3d/grass_switch_profile.h"
#include "fast/oot3d/grass_texture_source_cache.h"
#include "fast/oot3d/grass_visibility.h"

#include "oot3d_grass_switch_check.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <set>
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
            if (distance <= settings.FadeStart) EXPECT_EQ(fade, 255U);
            else EXPECT_LT(fade, 255U);
        }
        next += range.Count;
    }
    EXPECT_EQ(next, stats.Tufts + stats.Blades);
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

TEST(Oot3dGrassForeignCull, ACullOfAnotherAreaOrPlacementSetIsNeverDrawn) {
    // The title demo's last cull must not be drawn in the temple entrance,
    // even when a surface there is drawn as a grass source.
    const std::vector<uint64_t> cull{11U, 12U};
    const std::set<uint64_t> title{11U, 12U, 13U}, temple{90U};
    const auto has = [](const std::set<uint64_t>& set) { return [&set](uint64_t id) { return set.contains(id); }; };
    const uint32_t field = GrassArea(0x5E, 0), entrance = GrassArea(0x03, 0);
    EXPECT_EQ(GrassCullFitsFrame(field, field, cull, has(title)), GrassCullFit::Draw);
    EXPECT_EQ(GrassCullFitsFrame(field, entrance, cull, has(title)), GrassCullFit::OtherArea);
    EXPECT_EQ(GrassCullFitsFrame(entrance, entrance, cull, has(temple)), GrassCullFit::NoPlacement);
    EXPECT_EQ(GrassCullFitsFrame(field, GrassArea(0x5E, 1), cull, has(title)), GrassCullFit::OtherArea);
    EXPECT_EQ(GrassArea(-1, 3), 0xffffffffU);
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

// 60 fps (logs14g, Sacred Forest Meadow): the cull made in one frame was
// dropped as foreign in the next one, so only 257 of 600 frames drew. A
// cull of the same area and placement is drawn in every frame, whatever
// target the frame draws into, with that frame's camera; another area or
// placement set is still dropped.
TEST(Oot3dGrassInterpolatedFrames, AnInBetweenFrameIntoAnotherTargetKeepsTheGrass) {
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
    EXPECT_EQ(GrassCullFitsFrame(field, meadow, cull, has), GrassCullFit::OtherArea);
    EXPECT_EQ(GrassCullFitsFrame(meadow, meadow, std::vector<uint64_t>{90U}, has), GrassCullFit::NoPlacement);
    EXPECT_EQ(frame.Find(90U, kReal, kRealColor), nullptr);

    // 600 frames, the in-between ones drawing into another target than the
    // one their source is keyed by, a cull finishing every frame (made with
    // the previous frame's placements): every frame after the first cull
    // draws, each with its own camera. (Keyed by target alone, as in round
    // 3, every in-between frame dropped the cull.)
    GrassStreamSlots slots(2U);
    uint64_t drawn = 0U, dropped = 0U, ownCamera = 0U;
    std::optional<uint32_t> writing;
    for (uint64_t index = 1U; index <= 600U; ++index) {
        const bool between = index % 2U == 0U;
        frame.Clear();
        frame.Add(5U, kReal, kRealColor, {static_cast<int>(index)});
        if (writing) slots.FinishWrite(*writing, index - 1U);
        if (slots.Current() && GrassCullFitsFrame(meadow, meadow, cull, has) != GrassCullFit::Draw) {
            ++dropped;
            slots.Reset();
        }
        if (slots.Draw(index)) {
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
    EXPECT_EQ(GrassCullFitsFrame(meadow, GrassArea(0x5B, 0), cull, has), GrassCullFit::OtherArea);
}

} // namespace
