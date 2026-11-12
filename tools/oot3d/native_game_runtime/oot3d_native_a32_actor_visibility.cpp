#include "oot3d_native_a32_actor_visibility.h"

#include "fast/oot3d/perspective_fov_policy.h"
#include "oot3d_game_log.h"
#include "oot3d_native_a32_memory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>

namespace Oot3dNativeGame {
namespace {

using oot3d::recomp::a32::GuestState;
using oot3d::recomp::a32::MemoryBus;

constexpr uint32_t kActorProjectedPosition = 0xECU;
constexpr uint32_t kActorProjectedW = 0xF8U;
constexpr uint32_t kActorUncullForward = 0xFCU;
constexpr uint32_t kActorUncullScale = 0x100U;
constexpr uint32_t kActorUncullDownward = 0x104U;
constexpr uint32_t kActorCullingState = 0x120U;
constexpr uint32_t kObjMure2DistanceScalar = 0x1DCU;
constexpr uint32_t kActorParams = 0x1CU;
// Actor_Draw applies the distance fade only with this flag (0x002D6254).
constexpr uint32_t kActorFlagDistanceFade = 0x80000000U;
constexpr uint32_t kActorFade = 0x19EU;
constexpr uint32_t kActorFadeMaximum = 0x19FU;
// The Kokiri SoH stops fading: En_Ko has its own fade, En_Md (Mido) and
// En_Sa (Saria) share Actor_UpdateAlphaByDistance.
constexpr uint16_t kActorEnSa = 0x0146U;
constexpr uint16_t kActorEnMd = 0x016DU;

// The view-projection the title culls actors with (play+0x5BB4, row-major).
constexpr uint32_t kPlayCullingViewProjection = 0x5BB4U;

// Obj_Mure's spawn distances (sZClip: 1600, 1600, 1000, 1000, 1000) and the
// 40 its retirement adds, each read from guest memory at every use. SoH
// scales both the spawn and the retirement distance by 5.
constexpr uint32_t kObjMureDistanceTable = 0x005354B8U;
constexpr uint32_t kObjMureRetireMargin = 0x003CC430U;
constexpr std::array kObjMureNativeDistances{1600.0F, 1600.0F, 1000.0F, 1000.0F, 1000.0F, 40.0F};

// Both the culling walk and En_Wood02 keep play + 0x2000 in a register.
constexpr uint32_t kPlayHighHalf = 0x2000U;

constexpr std::array kHookPcs{
    kEnKoFadeDistanceA, kEnKoFadeDistanceB, kActorDrawEntry, kAlphaByDistanceCompare,
    kEnWood02SpawnZoneResult, kActorLightAllocate, kObjMure2ActiveEntry, kObjMure2SpawnEntry,
    kObjMureSpawnEntry, kObjMureActiveEntry, kActorLightEpochReset, kActorCullingEntry,
    kActorCullingResultHook, kActorCullingNext, kActorDeferredDrawReturn, kActorCullingEnd,
};

bool ReadFloat(MemoryBus& memory, uint32_t address, float& value) {
    uint32_t bits = 0U;
    if (!memory.Read32(address, &bits)) return false;
    value = std::bit_cast<float>(bits);
    return true;
}

bool ReadCullingSample(MemoryBus& memory, uint32_t actor, ActorCullingSample& sample) {
    return ReadFloat(memory, actor + kActorProjectedPosition + 0U, sample.X) &&
           ReadFloat(memory, actor + kActorProjectedPosition + 4U, sample.Y) &&
           ReadFloat(memory, actor + kActorProjectedPosition + 8U, sample.Z) &&
           ReadFloat(memory, actor + kActorProjectedW, sample.W) &&
           ReadFloat(memory, actor + kActorUncullForward, sample.Forward) &&
           ReadFloat(memory, actor + kActorUncullScale, sample.Scale) &&
           ReadFloat(memory, actor + kActorUncullDownward, sample.Downward);
}

// How much wider and taller the presented view is than the culling
// projection. The x and y rows of the culling view-projection scale by the
// inverse half-extent tangents (their spatial norms, as the view is rigid);
// the presented view is that frustum after ResolvePerspectiveFov, as the
// frustum hook applies it to the rendered camera.
ActorCullingVolume PresentedVolume(MemoryBus& memory, uint32_t play,
                                   const NativeActorVisibilityState& visibility) {
    ActorCullingVolume volume;
    volume.DistanceMultiplier = visibility.DistanceMultiplier;
    if (play == 0U) return volume;
    std::array<float, 8> rows{};
    for (uint32_t i = 0; i < rows.size(); ++i)
        if (!ReadFloat(memory, play + kPlayCullingViewProjection + i * 4U, rows[i])) return volume;
    const float scaleX = std::sqrt(rows[0] * rows[0] + rows[1] * rows[1] + rows[2] * rows[2]);
    const float scaleY = std::sqrt(rows[4] * rows[4] + rows[5] * rows[5] + rows[6] * rows[6]);
    if (!std::isfinite(scaleX) || !std::isfinite(scaleY) || scaleX <= 1.0e-6F || scaleY <= 1.0e-6F)
        return volume;
    const auto resolved = Fast::Oot3d::ResolvePerspectiveFov({
        {-1.0F / scaleX, 1.0F / scaleX, -1.0F / scaleY, 1.0F / scaleY, 1.0F, 2.0F},
        visibility.FovMultiplier,
        visibility.Presentation,
        true,
    });
    volume.Horizontal = resolved.HorizontalScale;
    volume.Vertical = resolved.VerticalScale;
    return volume;
}

bool Extends(const ActorCullingVolume& volume) {
    return volume.DistanceMultiplier > 1.0F || volume.Horizontal > 1.0F || volume.Vertical > 1.0F;
}

bool ExtendsDistance(const NativeActorVisibilityState& visibility) {
    return visibility.DistanceMultiplier > 1.0F;
}

bool ReadExclusion(MemoryBus& memory, uint32_t actor, uint16_t id, bool& excluded) {
    uint16_t params = 0U;
    if (!memory.Read16(actor + kActorParams, &params)) return false;
    excluded = ActorExtensionExcluded(id, params);
    return true;
}


// The native arena is Graph_ThreadEntry's scratch buffer. Validate the live
// bounds on every admission; an epoch counter is never used as free space.
bool ReadLightUsage(MemoryBus& memory, uint32_t arena, uint32_t& used) {
    uint32_t size = 0, base = 0, head = 0, tail = 0;
    if (arena == 0U || !memory.Read32(arena, &size) ||
        !memory.Read32(arena + 4U, &base) || !memory.Read32(arena + 8U, &head) ||
        !memory.Read32(arena + 12U, &tail) || size != 0x4000U ||
        base != 0x005656A0U || head != base || tail < base || tail > base + size ||
        ((base + size - tail) % 0x80U) != 0U) return false;
    used = (base + size - tail) / 0x80U;
    return true;
}

uint64_t ActorIdentity(uint32_t actor, uint16_t id) {
    return (static_cast<uint64_t>(actor) << 16U) | id;
}

NativeActorVisibilityReservation* Reservation(NativeActorVisibilityState& v, uint32_t actor) {
    for (auto& entry : v.Reservations)
        if (entry.Actor == actor) return &entry;
    return nullptr;
}

void ReleaseReservation(NativeActorVisibilityState& v, NativeActorVisibilityReservation& entry) {
    if (!entry.Reserved) return;
    entry.Reserved = false;
    if (v.RemainingLights >= entry.LightCost) v.RemainingLights -= entry.LightCost;
    else v.BudgetValid = false;
}

void RestoreNativeCulling(MemoryBus& memory, NativeActorVisibilityReservation& entry) {
    if (!entry.Restore) return;
    entry.Restore = false;
    if (entry.Handoff) {
        // Only the fade was the host's; a draw callback's own write stays.
        uint8_t fade = 0U;
        if (memory.Read8(entry.Actor + kActorFade, &fade) && fade == entry.HandoffFade)
            (void)memory.Write8(entry.Actor + kActorFade, entry.NativeFade);
        return;
    }
    uint32_t flags = 0U;
    (void)memory.Read8(entry.Actor + kActorFade, &entry.ExtendedFade);
    if (memory.Read32(entry.Actor + 4U, &flags)) {
        flags = (flags & ~0x40U) | (entry.NativeFade != 0U ? 0x40U : 0U);
        (void)memory.Write32(entry.Actor + 4U, flags);
        (void)memory.Write8(entry.Actor + kActorCullingState, entry.NativeState);
        (void)memory.Write8(entry.Actor + kActorFade, entry.NativeFade);
        if (!entry.NativeDraw) (void)memory.Write8(entry.Actor + 0x121U, 0U);
    }
}

// At Actor_Draw's entry for an actor the title shows again after an
// extension: the title's fade restarted at its boundary, so show the
// extension's continued fade instead, for this draw only.
void ApplyHandoffFade(MemoryBus& memory, NativeActorVisibilityReservation& entry) {
    if (!entry.Handoff || entry.Restore) return;
    entry.Handoff = false;
    uint32_t flags = 0U;
    uint8_t fade = 0U, maximum = 0U;
    if (!memory.Read32(entry.Actor + 4U, &flags) || (flags & kActorFlagDistanceFade) == 0U ||
        !memory.Read8(entry.Actor + kActorFade, &fade) || !memory.Read8(entry.Actor + kActorFadeMaximum, &maximum))
        return;
    const uint8_t shown = ActorHandoffFade(entry.ExtendedFade, fade, maximum);
    if (shown == fade || !memory.Write8(entry.Actor + kActorFade, shown)) return;
    entry.Handoff = true;
    entry.NativeFade = fade;
    entry.HandoffFade = shown;
    entry.Restore = true;
}

// These draw callbacks can allocate additional records (Fishing also loops
// over pond props/effects). Leave their whole walk native instead of guessing
// their costs. The helper's update/init callers precede this walk and are
// already included by the live cursor. Player's one get-item record is
// reserved per actor alongside its wrapper.
bool HasUnbudgetedDraw(uint32_t draw) {
    return draw == 0x00135384U || draw == 0x00228A34U || draw == 0x00278B20U || draw == 0x0037A590U ||
           draw == 0x00382B1CU || draw == 0x003B5290U;
}

// Projection is for PRIORITY only. Safety never depends on predicting a
// native culling result or on a previous frame's projected coordinates.
bool PrioritySample(MemoryBus& memory, uint32_t actor, const std::array<float, 16>& matrix,
                    ActorCullingSample& sample) {
    std::array<float, 3> world{};
    for (uint32_t i = 0; i < world.size(); ++i)
        if (!ReadFloat(memory, actor + 0x28U + i * 4U, world[i])) return false;
    std::array<float, 4> projected{};
    for (size_t row = 0; row < projected.size(); ++row)
        projected[row] = matrix[row * 4] * world[0] + matrix[row * 4 + 1] * world[1] +
                         matrix[row * 4 + 2] * world[2] + matrix[row * 4 + 3];
    sample.X = projected[0]; sample.Y = projected[1];
    sample.Z = -projected[2]; sample.W = projected[3];
    return ReadFloat(memory, actor + kActorUncullForward, sample.Forward) &&
           ReadFloat(memory, actor + kActorUncullScale, sample.Scale) &&
           ReadFloat(memory, actor + kActorUncullDownward, sample.Downward);
}

void BeginBudgetWalk(GuestState& state, MemoryBus& memory, NativeActorVisibilityState& v) {
    v.BudgetWalk = ExtendsDistance(v);
    v.BudgetValid = false;
    v.Reservations.clear();
    v.RemainingLights = 0U;
    v.WalkDenied = 0U;
    v.DeferredActor = 0U;
    if (!v.BudgetWalk) { v.Retained.clear(); return; }
    // This reservation contract belongs to the normal Gameplay_Draw pass.
    // An independent invocation must not borrow its singleton reservations.
    if (state.r[14] != 0x002E2CE0U || state.r[1] != state.r[0] + 0x208CU) return;
    uint32_t used = 0U;
    if (!memory.Read32(state.r[0], &v.Arena) || !ReadLightUsage(memory, v.Arena, used)) return;
    v.LightAllocations = used;
    v.PeakLightAllocations = std::max(v.PeakLightAllocations, used);
    v.WindowPeakLightAllocations = std::max(v.WindowPeakLightAllocations, used);
    std::array<float, 16> matrix{};
    for (uint32_t i = 0; i < matrix.size(); ++i)
        if (!ReadFloat(memory, state.r[0] + kPlayCullingViewProjection + i * 4U, matrix[i])) return;
    std::vector<ActorExtensionCandidate> candidates;
    // ActorContext+0x0C: twelve {count, head} lists; actor+0x130: next.
    // One reservation for EVERY actor, even a null draw, init, no-light flag,
    // or native reject. This bounds one Actor_Draw wrapper without forecasting
    // which actors will become visible later in the same walk. Player costs
    // two, to include its optional get-item draw.
    for (uint32_t category = 0; category < 12U; ++category) {
        uint32_t actor = 0U;
        if (!memory.Read32(state.r[1] + 0x10U + category * 8U, &actor)) return;
        while (actor != 0U) {
            if (v.Reservations.size() >= 1024U || actor > 0xFFFFFE00U || Reservation(v, actor)) return;
            NativeActorVisibilityReservation entry;
            entry.Actor = actor;
            uint32_t next = 0U, init = 0U;
            if (!memory.Read16(actor, &entry.Id) || !memory.Read32(actor + 0x140U, &entry.Draw) ||
                !memory.Read32(actor + 0x134U, &init) || !memory.Read32(actor + 0x130U, &next) ||
                HasUnbudgetedDraw(entry.Draw)) return;
            // Player_Draw can tail-call Player_DrawGetItem once. Reserve
            // it per instance, including unusual duplicate-player states.
            entry.LightCost = entry.Id == 0U || entry.Draw == 0x004BF618U ? 2U : 1U;
            const auto identity = ActorIdentity(actor, entry.Id);
            const auto retained = std::find_if(v.Retained.begin(), v.Retained.end(),
                [identity](const auto& prior) { return prior.Identity == identity; });
            if (retained != v.Retained.end()) entry.ExtendedFade = retained->Fade;
            v.Reservations.push_back(entry);
            v.RemainingLights += entry.LightCost;
            ActorCullingSample sample;
            bool excluded = true;
            if (init == 0U && entry.Draw != 0U && ReadExclusion(memory, actor, entry.Id, excluded) && !excluded &&
                PrioritySample(memory, actor, matrix, sample) &&
                !ActorInsideNativeCullingVolume(sample) &&
                ActorInsideExtendedCullingVolume(sample, v.CullingVolume)) {
                candidates.push_back({identity, sample.X * sample.X + sample.Y * sample.Y + sample.Z * sample.Z,
                    retained != v.Retained.end(), false});
            }
            actor = next;
        }
    }
    SelectActorExtensions(candidates, v.ExtensionLimit);
    v.Candidates += candidates.size();
    for (const auto& candidate : candidates) {
        if (!candidate.Selected) {
            ++v.Capped;
            continue;
        }
        if (auto* entry = Reservation(v, static_cast<uint32_t>(candidate.Identity >> 16U)))
            entry->Selected = true;
    }
    v.BudgetValid = true;
}

void EndBudgetWalk(MemoryBus& memory, NativeActorVisibilityState& v) {
    if (!v.BudgetWalk) return;
    v.Retained.clear();
    for (auto& entry : v.Reservations) {
        RestoreNativeCulling(memory, entry); // also covers deferred draws skipped by the title
        if (entry.Extended) v.Retained.push_back({ActorIdentity(entry.Actor, entry.Id), entry.ExtendedFade});
        // The title's fade is still behind the shown one: keep continuing it.
        else if (entry.HandoffFade > entry.NativeFade)
            v.Retained.push_back({ActorIdentity(entry.Actor, entry.Id), entry.HandoffFade});
    }
    if (!v.BudgetValid) ++v.BudgetInvalidWalks;
    if (v.WalkDenied != 0U || !v.BudgetValid) ++v.BudgetLimitedWalks;
    v.BudgetWalk = false;
    v.BudgetValid = false;
    v.RemainingLights = 0U;
}

bool ApplyBudgetedCulling(GuestState& state, MemoryBus& memory, NativeActorVisibilityState& v,
                         uint8_t nativeState) {
    auto* entry = Reservation(v, state.r[4]);
    uint32_t used = 0U;
    const bool selected = v.BudgetWalk && v.BudgetValid && entry && entry->Reserved && entry->Selected;
    if (!selected || !ReadLightUsage(memory, v.Arena, used) || !ActorLightBudgetAllows(used, v.RemainingLights)) {
        if (selected) ++v.LightDenied;
        ++v.BudgetDenied;
        ++v.WalkDenied;
        return false;
    }
    uint8_t fade = 0U;
    uint32_t flags = 0U, init = 0U, draw = 0U;
    if (!memory.Read8(entry->Actor + kActorFade, &fade) || !memory.Read32(entry->Actor + 4U, &flags) ||
        !memory.Read32(entry->Actor + 0x134U, &init) || !memory.Read32(entry->Actor + 0x140U, &draw) ||
        draw != entry->Draw || HasUnbudgetedDraw(draw)) {
        v.BudgetValid = false;
        ++v.BudgetDenied;
        ++v.WalkDenied;
        return false;
    }
    entry->NativeState = nativeState;
    entry->NativeFade = NativeCulledActorFade(static_cast<ActorCullingClass>(nativeState), fade);
    entry->NativeDraw = init == 0U && draw != 0U && ((flags & 0x20U) != 0U || entry->NativeFade != 0U);
    if (!memory.Write8(entry->Actor + kActorCullingState, 0U)) return false;
    const uint8_t beforeFade = std::max(entry->ExtendedFade,
        static_cast<uint8_t>(entry->NativeFade != 0U ? entry->NativeFade - 1U : 0U));
    (void)memory.Write8(entry->Actor + kActorFade, beforeFade);
    entry->Restore = true;
    entry->Extended = true;
    // Let the title ramp the extension's separate host-owned alpha. Restore
    // native history after drawing so grants cannot create unbudgeted
    // ten-frame fade liabilities during a burst of new native visibility.
    state.r[1] = static_cast<uint32_t>(ActorCullingClass::Far);
    state.r[0] = 1U;
    ++v.Extended;
    return true;
}

void ApplyActorCulling(GuestState& state, MemoryBus& memory, NativeActorVisibilityState& visibility) {
    const uint32_t actor = state.r[4];
    visibility.DeferredCountBefore = state.r[5];
    if (state.r[0] != 0U) {
        ++visibility.NativeVisible;
        // An actor the extension showed last walk: continue its fade.
        if (visibility.BudgetWalk)
            if (auto* entry = Reservation(visibility, actor); entry != nullptr && entry->ExtendedFade != 0U)
                entry->Handoff = true;
        return;
    }
    ++visibility.NativeCulled;
    const auto volume = visibility.CullingVolumeValid
        ? visibility.CullingVolume
        : PresentedVolume(memory, state.r[7] - kPlayHighHalf, visibility);
    if (!Extends(volume)) return;
    uint16_t id = 0U;
    ActorCullingSample sample;
    uint8_t nativeState = 0U;
    bool excluded = true;
    if (!memory.Read16(actor, &id) || !ReadExclusion(memory, actor, id, excluded) || excluded ||
        !ReadCullingSample(memory, actor, sample) || !memory.Read8(actor + kActorCullingState, &nativeState))
        return;
    const auto classified = ClassifyExtendedActorCulling(sample, volume);
    if (ExtendsDistance(visibility)) {
        if (classified == ActorCullingClass::Inside)
            (void)ApplyBudgetedCulling(state, memory, visibility, nativeState);
        return;
    }
    const auto next = static_cast<uint8_t>(classified);
    // A pass is the title's own: state 0 and a true result. The fade that
    // follows ramps in from state 2 and fills at once from state 1. A
    // failure inside the extended depth is no longer a far one.
    if (next != nativeState && !memory.Write8(actor + kActorCullingState, next)) return;
    if (classified == ActorCullingClass::Inside) {
        state.r[0] = 1U;
        ++visibility.Extended;
    }
}

// Restore native Obj_Mure population bounds before either reader uses them.
// Accept the old policy's 5x values, including states saved by that policy.
void AssertObjMureDistances(NativeActorVisibilityState& visibility) {
    if (visibility.HostMemory == nullptr) return;
    auto& memory = *visibility.HostMemory;
    // Spawned groups persist beyond this frame and may bypass culling. Keep
    // native population bounds; undo a known old extended value on restore.
    const float scale = 1.0F;
    const auto address = [](size_t i) {
        return i + 1U < kObjMureNativeDistances.size()
            ? kObjMureDistanceTable + static_cast<uint32_t>(i) * 4U
            : kObjMureRetireMargin;
    };
    for (size_t i = 0; i < kObjMureNativeDistances.size(); ++i) {
        uint32_t bits = 0U;
        const float native = kObjMureNativeDistances[i];
        const auto wanted = std::bit_cast<uint32_t>(native * scale);
        if (!memory.Read32(address(i), &bits)) return;
        if (bits == wanted) continue;
        if (bits != std::bit_cast<uint32_t>(native) &&
            bits != std::bit_cast<uint32_t>(native * kExtendedActorDistanceMultiplier)) {
            if (!visibility.ReportedUnknownConstants) {
                char line[96];
                std::snprintf(line, sizeof(line), "actor_visibility: Obj_Mure distance %08x at %08x is not the EUR value",
                              bits, address(i));
                WriteGameLog(line);
            }
            visibility.ReportedUnknownConstants = true;
            return;
        }
        (void)memory.WriteHost<uint32_t>(address(i), wanted);
    }
}

void ApplyObjMure2(GuestState& state, MemoryBus& memory, const NativeActorVisibilityState& visibility) {
    const uint32_t actor = state.r[0];
    float scalar = 0.0F;
    float x = 0.0F;
    float z = 0.0F;
    if (!ReadFloat(memory, actor + kObjMure2DistanceScalar, scalar) ||
        !ReadFloat(memory, actor + kActorProjectedPosition + 0U, x) ||
        !ReadFloat(memory, actor + kActorProjectedPosition + 8U, z))
        return;
    // The update stores 1 or 4 before every action; anything else is ours.
    if (scalar != 1.0F && scalar != 4.0F) return;
    const auto volume = PresentedVolume(memory, state.r[1], visibility);
    const float adjusted =
        ObjMure2DistanceScalar(scalar, x, z, volume.Horizontal, false);
    if (adjusted != scalar && std::isfinite(adjusted))
        (void)memory.Write32(actor + kObjMure2DistanceScalar, std::bit_cast<uint32_t>(adjusted));
}

// En_Wood02 offspring (EUR 0x0034E7B4..0x0034E874): the title's view test
// takes |1 / stored| of the spawner's +0xF8, which holds 1 / W, so it scales
// by W and its lateral test seldom passes; offspring then appear only through
// the fallback |z| + offset < forward * s16 near the camera. With
// ExtendGroups the result passes inside the extended volume with the correct
// 1 / W. The offspring are ordinary actors: culled and drawn as any other.
void ApplyEnWood02(GuestState& state, MemoryBus& memory, const NativeActorVisibilityState& visibility) {
    if (!visibility.ExtendGroups || !ExtendsDistance(visibility) || state.r[0] != 0U) return;
    const uint32_t spawner = state.r[4];
    ActorCullingSample sample;
    float inverseW = 0.0F;
    if (!ReadCullingSample(memory, spawner, sample) || !ReadFloat(memory, spawner + kActorProjectedW, inverseW) ||
        !(inverseW > 0.0F) || !std::isfinite(inverseW))
        return;
    sample.W = 1.0F / inverseW;
    if (ActorInsideExtendedCullingVolume(sample, PresentedVolume(memory, state.r[11] - kPlayHighHalf, visibility)))
        state.r[0] = 1U;
}

} // namespace

bool NativeActorVisibilityHasPendingRestore(const NativeActorVisibilityState& visibility) noexcept {
    return std::any_of(visibility.Reservations.begin(), visibility.Reservations.end(),
                       [](const auto& entry) { return entry.Restore; });
}

void ResetNativeActorVisibilityAfterStateLoad(NativeActorVisibilityState& visibility) noexcept {
    visibility.CullingVolumeValid = false;
    visibility.LightEpoch = 0U;
    visibility.LightAllocations = 0U;
    visibility.PreviousLightAllocations = 0U;
    visibility.WalkDenied = 0U;
    visibility.RemainingLights = 0U;
    visibility.Arena = 0U;
    visibility.DeferredCountBefore = 0U;
    visibility.DeferredActor = 0U;
    visibility.BudgetWalk = false;
    visibility.BudgetValid = false;
    visibility.Reservations.clear();
    visibility.Retained.clear();
}

std::string TakeNativeActorVisibilityPerfLine(NativeActorVisibilityState& v) {
    const auto& start = v.WindowStart;
    char line[320];
    const auto delta = [](uint64_t now, uint64_t then) { return static_cast<unsigned long long>(now - then); };
    std::snprintf(line, sizeof(line),
                  "actor_visibility: multiplier=%.0f groups=%u walks=%llu culled=%llu extended=%llu candidates=%llu "
                  "capped=%llu light_denied=%llu denied=%llu limited_walks=%llu invalid_walks=%llu peak_lights=%u "
                  "limit=%u\n",
                  static_cast<double>(v.DistanceMultiplier), v.ExtendGroups ? 1U : 0U, delta(v.Walks, start.Walks),
                  delta(v.NativeCulled, start.NativeCulled), delta(v.Extended, start.Extended),
                  delta(v.Candidates, start.Candidates), delta(v.Capped, start.Capped),
                  delta(v.LightDenied, start.LightDenied), delta(v.BudgetDenied, start.BudgetDenied),
                  delta(v.BudgetLimitedWalks, start.BudgetLimitedWalks),
                  delta(v.BudgetInvalidWalks, start.BudgetInvalidWalks), v.WindowPeakLightAllocations,
                  v.ExtensionLimit);
    v.WindowStart = {v.Walks,       v.NativeCulled, v.Extended,           v.Candidates,        v.Capped,
                     v.LightDenied, v.BudgetDenied, v.BudgetLimitedWalks, v.BudgetInvalidWalks};
    v.WindowPeakLightAllocations = 0U;
    return line;
}

std::optional<std::string> TakeNativeActorVisibilitySettingsLine(NativeActorVisibilityState& v,
                                                                 ActorDrawDistance choice) {
    const NativeActorVisibilityState::LoggedSettings now{static_cast<uint8_t>(choice), v.DistanceMultiplier,
                                                         v.ExtendGroups, v.NoKokiriFade};
    const auto& logged = v.Logged;
    if (logged.Choice == now.Choice && logged.DistanceMultiplier == now.DistanceMultiplier &&
        logged.ExtendGroups == now.ExtendGroups && logged.NoKokiriFade == now.NoKokiriFade)
        return std::nullopt;
    v.Logged = now;
    char line[160];
    std::snprintf(line, sizeof(line),
                  "draw_distance: choice=%s multiplier=%.0f groups=%s kokiri_fade=%s limit=%u "
                  "(L3+R3 > Video > Draw distance, config/draw_distance)",
                  ActorDrawDistanceWord(choice), static_cast<double>(v.DistanceMultiplier),
                  v.ExtendGroups ? "extended" : "native", v.NoKokiriFade ? "off" : "native", v.ExtensionLimit);
    return std::string(line);
}

std::span<const uint32_t> NativeActorVisibilityHookPcs() noexcept {
    return kHookPcs;
}

bool ApplyNativeActorVisibilityHook(
    uint32_t pc, GuestState& state, MemoryBus& memory,
    NativeActorVisibilityState& visibility) {
    switch (pc) {
    case kActorLightEpochReset:
        visibility.PreviousLightAllocations = visibility.LightAllocations;
        visibility.LightAllocations = 0U;
        ++visibility.LightEpoch;
        return true;
    case kActorLightAllocate: {
        uint32_t used = 0U;
        if (ReadLightUsage(memory, state.r[1], used)) {
            visibility.LightAllocations = used + 1U;
            visibility.PeakLightAllocations = std::max(visibility.PeakLightAllocations, used + 1U);
            visibility.WindowPeakLightAllocations = std::max(visibility.WindowPeakLightAllocations, used + 1U);
        }
        return true;
    }
    case kActorDrawEntry:
        if (visibility.BudgetWalk) {
            if (auto* entry = Reservation(visibility, state.r[1])) {
                ReleaseReservation(visibility, *entry);
                ApplyHandoffFade(memory, *entry);
            } else visibility.BudgetValid = false;
            if (state.r[14] == kActorDeferredDrawReturn) visibility.DeferredActor = state.r[1];
        }
        return true;
    case kActorCullingNext:
        if (visibility.BudgetWalk) {
            if (auto* entry = Reservation(visibility, state.r[4])) {
                if (state.r[5] == visibility.DeferredCountBefore) {
                    ReleaseReservation(visibility, *entry);
                    RestoreNativeCulling(memory, *entry);
                }
                // Otherwise the title enqueued this actor for a lens draw:
                // retain both its reservation and temporary culling state.
            } else visibility.BudgetValid = false;
        }
        return true;
    case kActorDeferredDrawReturn:
        if (visibility.BudgetWalk)
            if (auto* entry = Reservation(visibility, visibility.DeferredActor))
                RestoreNativeCulling(memory, *entry);
        visibility.DeferredActor = 0U;
        return true;
    case kActorCullingEnd:
        EndBudgetWalk(memory, visibility);
        return true;
    case kActorCullingEntry:
        // The culling matrix is fixed for the walk: resolve its view once.
        ++visibility.Walks;
        visibility.CullingVolume = PresentedVolume(memory, state.r[0], visibility);
        visibility.CullingVolumeValid = true;
        BeginBudgetWalk(state, memory, visibility);
        return true;
    case kActorCullingResultHook:
        ApplyActorCulling(state, memory, visibility);
        return true;
    case kObjMureSpawnEntry:
    case kObjMureActiveEntry:
        AssertObjMureDistances(visibility);
        return true;
    case kObjMure2SpawnEntry:
    case kObjMure2ActiveEntry:
        ApplyObjMure2(state, memory, visibility);
        return true;
    case kEnWood02SpawnZoneResult:
        ApplyEnWood02(state, memory, visibility);
        return true;
    case kEnKoFadeDistanceA:
    case kEnKoFadeDistanceB:
        // Distance 0 is inside any appear distance: the Kokiri fade in with
        // the title's own smoothing and stay.
        if (visibility.NoKokiriFade) state.vfp[0] = 0U;
        return true;
    case kAlphaByDistanceCompare: {
        // Distance 0 takes the near branch: the title raises the alpha to
        // 255 with its own smoothing.
        uint16_t id = 0U;
        if (visibility.NoKokiriFade && memory.Read16(state.r[4], &id) &&
            (id == kActorEnMd || id == kActorEnSa))
            state.vfp[0] = 0U;
        return true;
    }
    default:
        return false;
    }
}

} // namespace Oot3dNativeGame
