#include "oot3d_native_actor_interactions.h"
#include "fast/oot3d/grass_interaction_bridge.h"
#include "oot3d/actor_spawn.h"
#include "oot3d_native_a32_memory.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <unordered_set>
#include <vector>

namespace Oot3dNativeGame {
namespace {
template <class T>
bool Read(const NativeA32Memory &memory, uint32_t address, T &value) {
  return memory.ReadBytes(address,
                          {reinterpret_cast<uint8_t *>(&value), sizeof(value)});
}
static_assert(sizeof(Oot3dActor) == OOT3D_ACTOR_SIZE);
static_assert(offsetof(Oot3dActor, worldPos) == 0x28);
static_assert(offsetof(Oot3dActor, colChkInfo) == 0xA0);
static_assert(offsetof(Oot3dActor, next) == 0x130);

// A grass collider from an actor with a collision cylinder, typed by its
// native actor-profile ID: Player, En_Horse, En_Horse_Link_Child.
// En_Horse's initialized variant byte distinguishes Epona from Ingo.
// Renderer code receives typed roles and opaque identities, not IDs.
std::optional<Fast::Oot3d::GrassInteractor>
MakeInteractor(const NativeA32Memory &memory, uint32_t address,
               const Oot3dActor &actor, uint32_t playState,
               uint64_t sourceFrame) {
  const auto &collision = actor.colChkInfo;
  if (actor.update == 0 || actor.draw == 0 || collision.cylinderRadius <= 0 ||
      collision.cylinderHeight <= 0)
    return std::nullopt;
  Fast::Oot3d::GrassInteractor next;
  uint8_t horseVariant = 1;
  const bool adultEpona = actor.id == 0x0014 &&
      Read(memory, address + 0x1B0, horseVariant) && horseVariant == 0;
  using Kind = Fast::Oot3d::GrassInteractor::Kind;
  next.Type = actor.id == 0x0000 ? Kind::PlayerLink
            : adultEpona || actor.id == 0x009A ? Kind::Epona
            : Kind::SceneActor;
  next.StableId = (uint64_t{static_cast<uint16_t>(actor.id)} << 32) | address;
  next.ContextId = playState;
  next.Position = {actor.worldPos.x,
                   actor.worldPos.y + collision.cylinderYShift,
                   actor.worldPos.z};
  next.PreviousPosition = next.Position;
  next.Radius = static_cast<float>(collision.cylinderRadius);
  next.HalfHeight = static_cast<float>(collision.cylinderHeight) * 0.5F;
  next.FrameId = sourceFrame;
  return next;
}
} // namespace

std::vector<Fast::Oot3d::GrassInteractor>
CollectNativeActorInteractions(const NativeA32Memory &memory,
                               uint32_t playState, uint64_t sourceFrame) {
  std::vector<Fast::Oot3d::GrassInteractor> actors;
  Oot3dActorContext context{};
  if (playState == 0 ||
      playState > std::numeric_limits<uint32_t>::max() -
                      OOT3D_ACTOR_CONTEXT_PLAY_OFFSET ||
      !Read(memory, playState + OOT3D_ACTOR_CONTEXT_PLAY_OFFSET, context)) {
    return actors;
  }
  std::unordered_set<uint32_t> visited;
  const bool diagnostic = std::getenv("OOT3D_GRASS_DIAGNOSTICS") != nullptr &&
                          sourceFrame % 60 == 0;
  for (uint32_t category = 0; category < OOT3D_ACTOR_CATEGORY_COUNT;
       ++category) {
    auto address = context.lists[category].head;
    // Native total is u8; corrupt/cyclic lists cannot turn rendering into an
    // unbounded walk.
    const auto count = std::min(context.lists[category].count, uint32_t{255});
    for (uint32_t i = 0; address != 0 && i < count; ++i) {
      Oot3dActor actor{};
      if (!visited.insert(address).second || !Read(memory, address, actor) ||
          actor.category != category)
        break;
      const auto &collision = actor.colChkInfo;
      if (diagnostic) {
        std::fprintf(
            stderr,
            "[grass-actor] frame=%llu address=%08x id=%d category=%u "
            "pos=%.2f,%.2f,%.2f radius=%d height=%d yshift=%d update=%08x\n",
            static_cast<unsigned long long>(sourceFrame), address, actor.id,
            category, actor.worldPos.x, actor.worldPos.y, actor.worldPos.z,
            collision.cylinderRadius, collision.cylinderHeight,
            collision.cylinderYShift, actor.update);
      }
      if (auto next = MakeInteractor(memory, address, actor, playState,
                                     sourceFrame))
        actors.push_back(*next);
      address = actor.next;
    }
  }
  return actors;
}

void PublishNativeActorInteractions(const NativeA32Memory &memory,
                                    uint32_t playState, uint64_t sourceFrame) {
  Fast::Oot3d::GrassInteractionBridge::Instance().PublishFrame(
      sourceFrame,
      CollectNativeActorInteractions(memory, playState, sourceFrame));
}

std::vector<Fast::Oot3d::GrassInteractor>
CollectNativeGrassColliders(const NativeA32Memory &memory, uint32_t playState,
                            uint64_t sourceFrame) {
  std::vector<Fast::Oot3d::GrassInteractor> colliders;
  constexpr uint32_t kBackground = 1U, kPlayer = 2U;
  constexpr uint32_t kListsOffset = offsetof(Oot3dActorContext, lists);
  if (playState == 0 ||
      playState > std::numeric_limits<uint32_t>::max() -
                      OOT3D_ACTOR_CONTEXT_PLAY_OFFSET - 0x100U)
    return colliders;
  const uint32_t lists = playState + OOT3D_ACTOR_CONTEXT_PLAY_OFFSET + kListsOffset;
  const auto add = [&](uint32_t address) {
    Oot3dActor actor{};
    if (Read(memory, address, actor))
      if (auto next = MakeInteractor(memory, address, actor, playState,
                                     sourceFrame);
          next && next->Type != Fast::Oot3d::GrassInteractor::Kind::SceneActor)
        colliders.push_back(*next);
  };
  Oot3dActorListEntry player{};
  if (Read(memory, lists + kPlayer * sizeof(Oot3dActorListEntry), player) &&
      player.count != 0 && player.head != 0)
    add(player.head);
  Oot3dActorListEntry background{};
  if (!Read(memory, lists + kBackground * sizeof(Oot3dActorListEntry),
            background))
    return colliders;
  uint32_t address = background.head;
  const uint32_t count = std::min(background.count, uint32_t{16});
  for (uint32_t i = 0; address != 0 && i < count; ++i) {
    int16_t id = -1;
    uint32_t next = 0;
    if (!Read(memory, address + offsetof(Oot3dActor, id), id) ||
        !Read(memory, address + offsetof(Oot3dActor, next), next))
      break;
    if (id == 0x0014 || id == 0x009A) add(address);
    if (next == address) break;
    address = next;
  }
  return colliders;
}
} // namespace Oot3dNativeGame
