#pragma once

// Actor cost profile (config/actor_profile; off by default): game-logic time
// of Actor_UpdateAll, of each actor's update, of CollisionCheck_AT/OC, of
// Actor_DrawAll and of each actor's draw, measured with the host counter at
// block-entry hooks and printed once per logs/perf.txt window as an
// "actors:" line. Header-only; tests in oot3d_native_perf_diagnostics_tests.cpp.
//
// Each guest PC below is an emitted whole-AOT block label (checked in the
// generated shards): the hooks fire through the block-entry policy
// (oot3d_native_a32_projection_policy.cpp) only while the profile is on, so
// the default build observes nothing new. A function containing a hook runs
// its observed variant (one filter test per block); each hook costs a guest
// register flush and reload plus the policy call.

#include "oot3d_native_actor_names.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>

namespace Oot3dNativeGame {

// Actor_UpdateAll, entered from Gameplay_Update's BLEQ at 0x002E4054.
inline constexpr uint32_t kActorProfileUpdateAllEntry = 0x00461344U;
inline constexpr uint32_t kActorProfileUpdateAllReturn = 0x002E4058U;
// Per-actor update: r4 = actor; the block calls actor->update (Actor+0x13C)
// with BLX at 0x004617B0 and resumes at 0x004617B4 (0x004617A4 is inside the
// block, not a label).
inline constexpr uint32_t kActorProfileActorUpdateBlock = 0x00461784U;
inline constexpr uint32_t kActorProfileActorUpdateReturn = 0x004617B4U;
// CollisionCheck_AT and CollisionCheck_OC, called at 0x002E4018 and 0x002E4024.
inline constexpr uint32_t kActorProfileCollisionAtEntry = 0x00460548U;
inline constexpr uint32_t kActorProfileCollisionAtReturn = 0x002E401CU;
inline constexpr uint32_t kActorProfileCollisionOcEntry = 0x0045DC34U;
inline constexpr uint32_t kActorProfileCollisionOcReturn = 0x002E4028U;
// Actor_DrawAll (also the actor visibility policy's culling entry), its one
// call site returns to 0x002E2CE0.
inline constexpr uint32_t kActorProfileDrawAllEntry = 0x00461904U;
inline constexpr uint32_t kActorProfileDrawAllReturn = 0x002E2CE0U;
// Actor_Draw (r1 = actor), from the culling walk and from the lens pass.
inline constexpr uint32_t kActorProfileActorDrawEntry = 0x002D5F68U;
inline constexpr uint32_t kActorProfileActorDrawReturn = 0x00461BB8U;
inline constexpr uint32_t kActorProfileActorDrawLensReturn = 0x00461D2CU;

inline constexpr std::array<uint32_t, 13> kActorProfileHookPcs{
    kActorProfileUpdateAllEntry,   kActorProfileUpdateAllReturn,  kActorProfileActorUpdateBlock,
    kActorProfileActorUpdateReturn, kActorProfileCollisionAtEntry, kActorProfileCollisionAtReturn,
    kActorProfileCollisionOcEntry, kActorProfileCollisionOcReturn, kActorProfileDrawAllEntry,
    kActorProfileDrawAllReturn,    kActorProfileActorDrawEntry,   kActorProfileActorDrawReturn,
    kActorProfileActorDrawLensReturn,
};

// Host counter: cntpct_el0 on the Switch (19.2 MHz), nanoseconds elsewhere.
[[nodiscard]] inline uint64_t NativeProfileTicks() {
#if defined(__SWITCH__)
  uint64_t ticks;
  asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
  return ticks;
#else
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
#endif
}

[[nodiscard]] inline double NativeProfileTicksPerMs() {
#if defined(__SWITCH__)
  uint64_t frequency;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
  return static_cast<double>(frequency) / 1000.0;
#else
  return 1.0e6;
#endif
}

enum class ActorCost : uint8_t { Update, Draw };

// One perf.txt window of the profile, in counter ticks. update and collide are
// per Actor_UpdateAll call (one per game step), draw per Actor_DrawAll call;
// the per-actor shares use the same divisors. No allocation but the line.
class NativeActorProfileWindow {
public:
  // Ids past the name table share the last slot.
  static constexpr size_t kActorSlots = kNativeActorNames.size() + 1U;
  static constexpr size_t kCategorySlots = kNativeActorCategoryNames.size() + 1U;
  static constexpr size_t kTopActors = 8U;

  struct Actor {
    uint64_t UpdateTicks = 0U;
    uint64_t DrawTicks = 0U;
    uint32_t Updates = 0U;
    uint32_t Draws = 0U;
    uint8_t Category = 0xFFU;
  };

  struct TopEntry {
    uint16_t Id = 0U;
    uint8_t Category = 0xFFU;
    double Ms = 0.0;        // update + draw, per step
    double Instances = 0.0; // updates per step (draws per draw pass without steps)
  };

  void AddUpdateAll(uint64_t ticks) {
    mUpdateAllTicks += ticks;
    ++mSteps;
  }
  void AddCollision(bool oc, uint64_t ticks) {
    (oc ? mOcTicks : mAtTicks) += ticks;
    if (!oc) ++mCollisionPasses;
  }
  void AddDrawAll(uint64_t ticks) {
    mDrawAllTicks += ticks;
    ++mDrawPasses;
  }
  void AddActor(ActorCost cost, uint16_t id, uint8_t category, uint64_t ticks) {
    Actor &actor = mActors[std::min<size_t>(id, kActorSlots - 1U)];
    actor.Category = category;
    mCategoryTicks[std::min<size_t>(category, kCategorySlots - 1U)][cost == ActorCost::Draw ? 1 : 0] += ticks;
    if (cost == ActorCost::Update) {
      actor.UpdateTicks += ticks;
      ++actor.Updates;
      mActorUpdateTicks += ticks;
      ++mActorUpdates;
    } else {
      actor.DrawTicks += ticks;
      ++actor.Draws;
      mActorDrawTicks += ticks;
      ++mActorDraws;
    }
  }
  void AddDropped() { ++mDropped; }
  void AddHook(uint64_t overheadTicks) {
    ++mHooks;
    mOverheadTicks += overheadTicks;
  }

  [[nodiscard]] uint32_t Steps() const { return mSteps; }
  [[nodiscard]] uint32_t DrawPasses() const { return mDrawPasses; }
  [[nodiscard]] uint32_t ActorUpdates() const { return mActorUpdates; }
  [[nodiscard]] uint32_t Dropped() const { return mDropped; }
  [[nodiscard]] const Actor &ActorSlot(uint16_t id) const {
    return mActors[std::min<size_t>(id, kActorSlots - 1U)];
  }

  // The actors with the most time (update per step + draw per draw pass),
  // most first; equal times by id. Returns how many were written.
  size_t Top(double ticksPerMs, std::span<TopEntry> out) const {
    std::array<uint16_t, kActorSlots> order{};
    size_t count = 0U;
    for (size_t id = 0; id < kActorSlots; ++id)
      if (mActors[id].UpdateTicks + mActors[id].DrawTicks != 0U) order[count++] = static_cast<uint16_t>(id);
    const auto ms = [&](uint16_t id) { return ActorMs(mActors[id], ticksPerMs); };
    const size_t wanted = std::min(count, out.size());
    std::partial_sort(order.begin(), order.begin() + wanted, order.begin() + count,
                      [&](uint16_t a, uint16_t b) {
                        const double left = ms(a), right = ms(b);
                        return left != right ? left > right : a < b;
                      });
    for (size_t index = 0; index < wanted; ++index) {
      const Actor &actor = mActors[order[index]];
      out[index] = {order[index], actor.Category, ms(order[index]),
                    mSteps != 0U ? static_cast<double>(actor.Updates) / mSteps
                                 : static_cast<double>(actor.Draws) / PerPass(mDrawPasses)};
    }
    return wanted;
  }

  // "actors: <area> steps=.. n=.. update_ms=.. ... top=Name/category:ms xN,..."
  [[nodiscard]] std::string Line(double ticksPerMs, std::string_view area) const {
    const double steps = PerPass(mSteps);
    const double draws = PerPass(mDrawPasses);
    const double collisions = PerPass(mCollisionPasses);
    const auto ms = [&](uint64_t ticks, double passes) { return static_cast<double>(ticks) / ticksPerMs / passes; };
    // Overhead and hooks per game step (draw passes while paused).
    const double frames = PerPass(std::max(mSteps, mDrawPasses));
    std::string line;
    line.reserve(640U);
    char text[320];
    std::snprintf(text, sizeof(text),
                  "actors: %.*s steps=%u draws=%u n=%.1f update_ms=%.2f actor_update_ms=%.2f "
                  "collide_ms=%.2f at_ms=%.2f oc_ms=%.2f draw_ms=%.2f actor_draw_ms=%.2f "
                  "hooks=%.0f overhead_ms=%.3f dropped=%u",
                  static_cast<int>(area.size()), area.data(), mSteps, mDrawPasses,
                  static_cast<double>(mActorUpdates) / steps, ms(mUpdateAllTicks, steps),
                  ms(mActorUpdateTicks, steps), ms(mAtTicks + mOcTicks, collisions), ms(mAtTicks, collisions),
                  ms(mOcTicks, collisions), ms(mDrawAllTicks, draws), ms(mActorDrawTicks, draws),
                  static_cast<double>(mHooks) / frames, ms(mOverheadTicks, frames), mDropped);
    line += text;
    // Per actor category, most first.
    std::array<std::pair<double, size_t>, kCategorySlots> categories{};
    size_t categoryCount = 0U;
    for (size_t category = 0; category < kCategorySlots; ++category) {
      const double value = ms(mCategoryTicks[category][0], steps) + ms(mCategoryTicks[category][1], draws);
      if (mCategoryTicks[category][0] + mCategoryTicks[category][1] != 0U)
        categories[categoryCount++] = {value, category};
    }
    std::sort(categories.begin(), categories.begin() + categoryCount,
              [](const auto &a, const auto &b) { return a.first != b.first ? a.first > b.first : a.second < b.second; });
    line += " by_cat=";
    for (size_t index = 0; index < categoryCount; ++index) {
      const char *name = NativeActorCategoryName(static_cast<uint8_t>(categories[index].second));
      std::snprintf(text, sizeof(text), "%s%s:%.2f", index != 0U ? "," : "", name != nullptr ? name : "other",
                    categories[index].first);
      line += text;
    }
    if (categoryCount == 0U) line += '-';
    std::array<TopEntry, kTopActors> top{};
    const size_t topCount = Top(ticksPerMs, top);
    line += " top=";
    for (size_t index = 0; index < topCount; ++index) {
      const TopEntry &entry = top[index];
      char id[8];
      const char *name = entry.Id < kNativeActorNames.size() ? NativeActorName(entry.Id) : "other";
      if (name == nullptr) {
        std::snprintf(id, sizeof(id), "0x%04X", static_cast<unsigned>(entry.Id));
        name = id;
      }
      const char *category = NativeActorCategoryName(entry.Category);
      std::snprintf(text, sizeof(text), "%s%s/%s:%.2fx%.3g", index != 0U ? "," : "", name,
                    category != nullptr ? category : "other", entry.Ms, entry.Instances);
      line += text;
    }
    if (topCount == 0U) line += '-';
    line += '\n';
    return line;
  }

  void Reset() { *this = NativeActorProfileWindow{}; }

private:
  static double PerPass(uint32_t passes) { return passes != 0U ? static_cast<double>(passes) : 1.0; }
  double ActorMs(const Actor &actor, double ticksPerMs) const {
    return static_cast<double>(actor.UpdateTicks) / ticksPerMs / PerPass(mSteps) +
           static_cast<double>(actor.DrawTicks) / ticksPerMs / PerPass(mDrawPasses);
  }

  std::array<Actor, kActorSlots> mActors{};
  std::array<std::array<uint64_t, 2>, kCategorySlots> mCategoryTicks{};
  uint64_t mUpdateAllTicks = 0U;
  uint64_t mActorUpdateTicks = 0U;
  uint64_t mAtTicks = 0U;
  uint64_t mOcTicks = 0U;
  uint64_t mDrawAllTicks = 0U;
  uint64_t mActorDrawTicks = 0U;
  uint64_t mOverheadTicks = 0U;
  uint64_t mHooks = 0U;
  uint32_t mSteps = 0U;
  uint32_t mCollisionPasses = 0U;
  uint32_t mDrawPasses = 0U;
  uint32_t mActorUpdates = 0U;
  uint32_t mActorDraws = 0U;
  uint32_t mDropped = 0U;
};

// Pairs each hooked entry with its return and adds the interval to the
// window. An entry whose return address is not the expected call site (another
// caller) is ignored; a second entry before the return, a return of another
// actor or an interval over kMaximumIntervalMs counts as dropped.
class NativeActorProfile {
public:
  static constexpr double kMaximumIntervalMs = 250.0;

  explicit NativeActorProfile(double ticksPerMs) : mTicksPerMs(ticksPerMs) {}

  [[nodiscard]] static std::span<const uint32_t> HookPcs() { return kActorProfileHookPcs; }

  // At a block entry: r1, r4 and lr (r14) as the guest has them. Memory: any
  // guest memory with Read8/Read16 (oot3d::recomp::a32::MemoryBus). Returns
  // whether the pc is one of the profile's hooks.
  template <typename Memory>
  bool Observe(uint32_t pc, uint32_t r1, uint32_t r4, uint32_t lr, Memory &memory, uint64_t now) {
    switch (pc) {
    case kActorProfileUpdateAllEntry:
      if (lr == kActorProfileUpdateAllReturn) Begin(mUpdateAll, now);
      return true;
    case kActorProfileUpdateAllReturn: {
      uint64_t ticks = 0U;
      if (mActorUpdate.Active) {
        mActorUpdate.Active = false;
        mWindow.AddDropped();
      }
      if (End(mUpdateAll, now, ticks)) mWindow.AddUpdateAll(ticks);
      return true;
    }
    case kActorProfileActorUpdateBlock:
      BeginActor(mActorUpdate, r4, memory, now);
      return true;
    case kActorProfileActorUpdateReturn:
      EndActor(mActorUpdate, ActorCost::Update, r4, true, now);
      return true;
    case kActorProfileCollisionAtEntry:
      if (lr == kActorProfileCollisionAtReturn) Begin(mAt, now);
      return true;
    case kActorProfileCollisionAtReturn: {
      uint64_t ticks = 0U;
      if (End(mAt, now, ticks)) mWindow.AddCollision(false, ticks);
      return true;
    }
    case kActorProfileCollisionOcEntry:
      if (lr == kActorProfileCollisionOcReturn) Begin(mOc, now);
      return true;
    case kActorProfileCollisionOcReturn: {
      uint64_t ticks = 0U;
      if (End(mOc, now, ticks)) mWindow.AddCollision(true, ticks);
      return true;
    }
    case kActorProfileDrawAllEntry:
      if (lr == kActorProfileDrawAllReturn) Begin(mDrawAll, now);
      return true;
    case kActorProfileDrawAllReturn: {
      uint64_t ticks = 0U;
      if (mActorDraw.Active) {
        mActorDraw.Active = false;
        mWindow.AddDropped();
      }
      if (End(mDrawAll, now, ticks)) mWindow.AddDrawAll(ticks);
      return true;
    }
    case kActorProfileActorDrawEntry:
      if (lr == kActorProfileActorDrawReturn || lr == kActorProfileActorDrawLensReturn)
        BeginActor(mActorDraw, r1, memory, now);
      return true;
    case kActorProfileActorDrawReturn:
    case kActorProfileActorDrawLensReturn:
      EndActor(mActorDraw, ActorCost::Draw, 0U, false, now);
      return true;
    default:
      return false;
    }
  }

  void AddHookOverhead(uint64_t ticks) { mWindow.AddHook(ticks); }

  // The window's line, then a new window.
  [[nodiscard]] std::string TakeLine(std::string_view area) {
    std::string line = mWindow.Line(mTicksPerMs, area);
    mWindow.Reset();
    return line;
  }

  [[nodiscard]] const NativeActorProfileWindow &Window() const { return mWindow; }

private:
  struct Open {
    uint64_t Start = 0U;
    uint32_t Actor = 0U;
    uint16_t Id = 0U;
    uint8_t Category = 0xFFU;
    bool Active = false;
  };

  void Begin(Open &open, uint64_t now) {
    if (open.Active) mWindow.AddDropped();
    open.Active = true;
    open.Start = now;
  }

  bool End(Open &open, uint64_t now, uint64_t &ticks) {
    if (!open.Active) return false;
    open.Active = false;
    if (now < open.Start || static_cast<double>(now - open.Start) > kMaximumIntervalMs * mTicksPerMs) {
      mWindow.AddDropped();
      return false;
    }
    ticks = now - open.Start;
    return true;
  }

  // Actor+0x00 s16 id, Actor+0x02 u8 category (decomp_support oot3d/actor.h).
  template <typename Memory> void BeginActor(Open &open, uint32_t actor, Memory &memory, uint64_t now) {
    uint16_t id = 0U;
    uint8_t category = 0xFFU;
    if (actor == 0U || !memory.Read16(actor, &id) || !memory.Read8(actor + 2U, &category)) {
      if (open.Active) mWindow.AddDropped();
      open.Active = false;
      return;
    }
    Begin(open, now);
    open.Actor = actor;
    open.Id = id;
    open.Category = category;
  }

  void EndActor(Open &open, ActorCost cost, uint32_t actor, bool checkActor, uint64_t now) {
    if (open.Active && checkActor && actor != open.Actor) {
      open.Active = false;
      mWindow.AddDropped();
      return;
    }
    uint64_t ticks = 0U;
    if (End(open, now, ticks)) mWindow.AddActor(cost, open.Id, open.Category, ticks);
  }

  double mTicksPerMs;
  NativeActorProfileWindow mWindow;
  Open mUpdateAll, mActorUpdate, mAt, mOc, mDrawAll, mActorDraw;
};

} // namespace Oot3dNativeGame
