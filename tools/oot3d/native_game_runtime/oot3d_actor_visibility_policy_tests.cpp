#include "oot3d_actor_visibility_policy.h"
#include "oot3d_draw_distance_config.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void Expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Oot3dNativeGame::ActorCullingSample Actor(float x, float y, float z, float w) {
    Oot3dNativeGame::ActorCullingSample sample;
    sample.X = x;
    sample.Y = y;
    sample.Z = z;
    sample.W = w;
    sample.Forward = 1000.0F;
    sample.Scale = 50.0F;
    sample.Downward = 100.0F;
    return sample;
}

} // namespace

int main() {
    using namespace Oot3dNativeGame;
    try {
        const ActorCullingVolume native;
        // Depth: strict inequalities at -scale and forward + scale.
        Expect(ActorInsideNativeCullingVolume(Actor(0, 0, 500, 500)), "a centred actor is visible");
        Expect(!ActorInsideNativeCullingVolume(Actor(0, 0, 1050, 1050)), "forward + scale is outside");
        Expect(ActorInsideNativeCullingVolume(Actor(0, 0, 1049, 1049)), "just inside forward + scale");
        Expect(!ActorInsideNativeCullingVolume(Actor(0, 0, -50, 1)), "-scale is outside");
        // W under 1 divides by 1.
        Expect(ActorInsideNativeCullingVolume(Actor(50.5F, 0, 10, 0.25F)), "W below 1 does not magnify");
        Expect(!ActorInsideNativeCullingVolume(Actor(51.0F, 0, 10, 0.25F)), "lateral bound with W below 1");
        // Lateral and vertical bounds grow by the scale.
        Expect(ActorInsideNativeCullingVolume(Actor(549, 0, 500, 500)), "inside the grown lateral bound");
        Expect(!ActorInsideNativeCullingVolume(Actor(550, 0, 500, 500)), "outside the lateral bound");
        Expect(!ActorInsideNativeCullingVolume(Actor(0, -600, 500, 500)), "below the vertical bound");
        Expect(!ActorInsideNativeCullingVolume(Actor(0, 550, 500, 500)), "above the vertical bound");

        // The extended volume without extensions is the native one.
        for (const auto& sample : {Actor(0, 0, 1049, 1049), Actor(0, 0, 1050, 1050), Actor(549, 0, 500, 500),
                                   Actor(550, 0, 500, 500), Actor(0, -600, 500, 500), Actor(0, 0, -50, 1)}) {
            Expect(ActorInsideExtendedCullingVolume(sample, native) == ActorInsideNativeCullingVolume(sample),
                   "an identity volume must match the native test");
        }

        // Presentation factors reveal what a wider view shows, nothing else.
        ActorCullingVolume wide;
        wide.Horizontal = 1.2F;
        wide.Vertical = 1.1F;
        Expect(ActorInsideExtendedCullingVolume(Actor(640, 0, 500, 500), wide), "16:9 margin is visible");
        Expect(!ActorInsideExtendedCullingVolume(Actor(660, 0, 500, 500), wide), "beyond the presented width");
        Expect(ActorInsideExtendedCullingVolume(Actor(0, 590, 500, 500), wide), "taller view reveals more");
        Expect(!ActorInsideExtendedCullingVolume(Actor(0, 0, 1050, 1050), wide), "aspect does not extend depth");

        // Distance: forward + scale times 5, with SoH's +1000 under 500.
        ActorCullingVolume far;
        far.DistanceMultiplier = kExtendedActorDistanceMultiplier;
        Expect(ActorInsideExtendedCullingVolume(Actor(0, 0, 5049, 5049), far), "5x forward");
        Expect(!ActorInsideExtendedCullingVolume(Actor(0, 0, 5050, 5050), far), "beyond 5x forward");
        auto shortRange = Actor(0, 0, 7544, 7544);
        shortRange.Forward = 500.0F - 1.0F;
        Expect(ActorInsideExtendedCullingVolume(shortRange, far), "short forward gains 1000 first");
        shortRange.Z = 7545.0F;
        Expect(!ActorInsideExtendedCullingVolume(shortRange, far), "beyond the short-forward extension");
        Expect(!ActorInsideExtendedCullingVolume(Actor(0, 0, -50, 1), far), "behind the camera stays culled");

        // Invalid factors never shrink the volume; non-finite samples are culled.
        ActorCullingVolume invalid;
        invalid.DistanceMultiplier = 0.5F;
        invalid.Horizontal = std::numeric_limits<float>::quiet_NaN();
        invalid.Vertical = -2.0F;
        Expect(ActorInsideExtendedCullingVolume(Actor(549, 0, 1049, 1049), invalid), "factors below 1 clamp");
        Expect(!ActorInsideExtendedCullingVolume(Actor(std::numeric_limits<float>::infinity(), 0, 10, 10), far),
               "a non-finite position is culled");

        // Failures keep the title's states: far first, then behind or aside.
        Expect(ClassifyNativeActorCulling(Actor(0, 0, 2000, 2000)) == ActorCullingClass::Far, "native far");
        Expect(ClassifyNativeActorCulling(Actor(5000, 0, 2000, 2000)) == ActorCullingClass::Far,
               "far wins over aside, as in the title");
        Expect(ClassifyNativeActorCulling(Actor(0, 0, -60, 1)) == ActorCullingClass::Outside, "behind");
        // Within the extended depth but aside: no longer far, so no far fade.
        Expect(ClassifyExtendedActorCulling(Actor(5000, 0, 2000, 2000), far) == ActorCullingClass::Outside,
               "an extended depth reclassifies a far actor aside");
        Expect(ClassifyExtendedActorCulling(Actor(0, 0, 6000, 6000), far) == ActorCullingClass::Far,
               "beyond the extended depth stays far");

        // A burst can consume every remaining reservation this epoch. No
        // previous-frame average appears in the admission calculation.
        for (uint32_t used = 0; used <= 150; ++used)
            for (uint32_t remaining = 0; remaining <= 300; ++remaining)
                Expect(ActorLightBudgetAllows(used, remaining) ==
                           (used + remaining + kActorLightAuxiliaryReserve <= kActorLightBudget),
                       "exact reservation boundary");
        Expect(!ActorLightBudgetAllows(UINT32_MAX, 1), "used overflow fails closed");
        Expect(!ActorLightBudgetAllows(1, UINT32_MAX), "reservation overflow fails closed");
        Expect(NativeCulledActorFade(ActorCullingClass::Far, 10) == 9, "native fade decrements");
        Expect(NativeCulledActorFade(ActorCullingClass::Far, 1) == 0, "last native fade expires");
        Expect(NativeCulledActorFade(ActorCullingClass::Outside, 10) == 0, "native aside cuts");

        std::array<ActorExtensionCandidate, 40> candidates{};
        for (size_t i = 0; i < candidates.size(); ++i)
            candidates[i] = {i + 1, static_cast<float>(i + 1), false, false};
        candidates.back().Retained = true;
        SelectActorExtensions(candidates);
        Expect(candidates.back().Selected && candidates[30].Selected && !candidates[31].Selected,
               "incumbents then nearer newcomers");
        std::reverse(candidates.begin(), candidates.end());
        SelectActorExtensions(candidates);
        for (const auto& c : candidates)
            Expect(c.Selected == (c.Identity <= 31 || c.Identity == 40), "list order does not change rank");
        candidates[0].DistanceSquared = std::numeric_limits<float>::quiet_NaN();
        SelectActorExtensions(candidates);
        Expect(!candidates[0].Selected, "NaN incumbent cannot monopolize the budget");

        // The Draw distance choice's limit: Farther and Maximum select more.
        Expect(ActorExtensionLimit(ActorDrawDistance::Original) == kActorExtensionLimit &&
                   ActorExtensionLimit(ActorDrawDistance::Far) == kActorExtensionLimit &&
                   ActorExtensionLimit(ActorDrawDistance::Farther) == 48U &&
                   ActorExtensionLimit(ActorDrawDistance::Maximum) == 64U,
               "draw distance limits");
        candidates[0].DistanceSquared = 1.0F;
        SelectActorExtensions(candidates, ActorExtensionLimit(ActorDrawDistance::Maximum));
        Expect(std::all_of(candidates.begin(), candidates.end(), [](const auto& c) { return c.Selected; }),
               "Maximum's limit selects all 40");

        // Obj_Mure2's radial scalar.
        Expect(ObjMure2DistanceScalar(1.0F, 300.0F, 400.0F, 1.0F, false) == 1.0F, "native scalar kept");
        Expect(ObjMure2DistanceScalar(4.0F, 0.0F, 0.0F, 1.0F, true) == 25.0F, "extended is at least 5x squared");
        Expect(ObjMure2DistanceScalar(100.0F, 0.0F, 0.0F, 1.0F, true) == 100.0F, "a larger native scalar stays");
        Expect(ObjMure2DistanceScalar(1.0F, 0.0F, 400.0F, 1.2F, false) == 1.0F, "straight ahead is unchanged");
        // X = 600, Z = 0, H = 1.2: the presented X is 500, so 600^2 / 500^2.
        const float aside = ObjMure2DistanceScalar(1.0F, 600.0F, 0.0F, 1.2F, false);
        Expect(aside > 1.4399F && aside < 1.4401F, "aside the test sees the presented width");
        Expect(ObjMure2DistanceScalar(1.0F, 0.0F, 0.0F, 1.2F, false) == 1.0F, "a zero position is guarded");

        // L3+R3 > Video > Draw distance: choices, words and the config file.
        Expect(ActorDrawDistanceMultiplier(ActorDrawDistance::Original) == 1.0F &&
               ActorDrawDistanceMultiplier(ActorDrawDistance::Far) == 2.0F &&
               ActorDrawDistanceMultiplier(ActorDrawDistance::Farther) == 3.0F &&
               ActorDrawDistanceMultiplier(ActorDrawDistance::Maximum) == kExtendedActorDistanceMultiplier,
               "draw distance multipliers");
        for (const auto choice : {ActorDrawDistance::Original, ActorDrawDistance::Far, ActorDrawDistance::Farther,
                                  ActorDrawDistance::Maximum})
            Expect(ParseActorDrawDistance(ActorDrawDistanceWord(choice)) == choice, "draw distance word round trip");
        Expect(ParseActorDrawDistance(nullptr) == ActorDrawDistance::Original &&
               ParseActorDrawDistance("5") == ActorDrawDistance::Original &&
               ParseActorDrawDistance("") == ActorDrawDistance::Original, "unknown words are Original");
        {
            const auto dir = std::filesystem::temp_directory_path() / "oot3d_draw_distance_config_test";
            std::filesystem::remove_all(dir);
            Expect(ReadActorDrawDistance(dir) == kDefaultActorDrawDistance, "no file is the default");
            for (const auto choice : {ActorDrawDistance::Far, ActorDrawDistance::Farther, ActorDrawDistance::Maximum,
                                      ActorDrawDistance::Original}) {
                Expect(SaveActorDrawDistance(dir, choice), "draw distance saved");
                Expect(ReadActorDrawDistance(dir) == choice, "draw distance reloaded");
            }
            Expect(!std::filesystem::exists(dir / kDrawDistanceConfigName), "the default removes the file");
            std::filesystem::remove_all(dir);
        }
        // 2x and 3x: (forward 1000) * multiplier + scale 50.
        ActorCullingVolume twice;
        twice.DistanceMultiplier = ActorDrawDistanceMultiplier(ActorDrawDistance::Far);
        ActorCullingVolume thrice;
        thrice.DistanceMultiplier = ActorDrawDistanceMultiplier(ActorDrawDistance::Farther);
        Expect(ActorInsideExtendedCullingVolume(Actor(0, 0, 2049, 2049), twice) &&
               !ActorInsideExtendedCullingVolume(Actor(0, 0, 2050, 2050), twice), "2x forward + scale");
        Expect(ActorInsideExtendedCullingVolume(Actor(0, 0, 3049, 3049), thrice) &&
               !ActorInsideExtendedCullingVolume(Actor(0, 0, 3050, 3050), thrice), "3x forward + scale");
        // (200 + 1000) * 2 + 50 = 2450.
        auto shortTwice = Actor(0, 0, 2449, 2449);
        shortTwice.Forward = 200.0F;
        Expect(ActorInsideExtendedCullingVolume(shortTwice, twice), "SoH's +1000 under 500 at 2x");
        shortTwice.Z = shortTwice.W = 2450.0F;
        Expect(!ActorInsideExtendedCullingVolume(shortTwice, twice), "beyond the short range at 2x");

        // Exclusions: En_Viewer, Gold Skulltulas (En_Sw types 1 and 2).
        Expect(ActorExtensionExcluded(0x002A, 0) && ActorExtensionExcluded(0x0095, 0x2000) &&
               ActorExtensionExcluded(0x0095, 0x4000) && ActorExtensionExcluded(0x0095, 0x5FFF),
               "excluded actors");
        Expect(!ActorExtensionExcluded(0x0095, 0x0000) && !ActorExtensionExcluded(0x0095, 0x6000) &&
               !ActorExtensionExcluded(0x0141, 0x2000), "a Skullwalltula and others extend");

        // Handoff: the extension's fade continues until the title's catches up.
        Expect(ActorHandoffFade(10, 1, 0) == 10, "a fully shown actor stays shown");
        Expect(ActorHandoffFade(4, 1, 0) == 5, "a fading-in extension keeps ramping");
        Expect(ActorHandoffFade(10, 10, 0) == 10 && ActorHandoffFade(3, 9, 0) == 9, "the title's fade wins once ahead");
        Expect(ActorHandoffFade(10, 1, 20) == 11 && ActorHandoffFade(30, 1, 20) == 20, "custom maximum");
        Expect(ActorHandoffFade(0, 1, 0) == 1, "no extension history keeps the title's");
    } catch (const std::exception& error) {
        std::cerr << "oot3d_actor_visibility_policy_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "oot3d_actor_visibility_policy_tests: ok\n";
    return 0;
}
