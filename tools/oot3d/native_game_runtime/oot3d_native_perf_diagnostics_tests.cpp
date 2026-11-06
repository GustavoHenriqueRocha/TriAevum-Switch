// Console log diagnostics, pure parts: the game's area (scene, room) for
// hitches.txt and perf.txt (oot3d_native_area.h), the actor name tables
// (oot3d_native_actor_names.h) and the actor cost profile's pairing,
// per-window aggregation, top actors and line (oot3d_native_actor_profile.h).

#include "oot3d_native_actor_names.h"
#include "oot3d_native_actor_profile.h"
#include "oot3d_native_indirect_profile.h"
#include "oot3d_native_area.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>

namespace {

using namespace Oot3dNativeGame;

void Require(bool condition, const std::string &message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    std::exit(1);
  }
}

void RequireEqual(const std::string &actual, const std::string &expected, const char *message) {
  if (actual != expected) {
    std::fprintf(stderr, "FAIL: %s\n  actual:   %s\n  expected: %s\n", message, actual.c_str(),
                 expected.c_str());
    std::exit(1);
  }
}

// Little-endian guest memory; unwritten bytes are unmapped.
struct GuestMemory {
  std::map<uint32_t, uint8_t> Bytes;

  void Put8(uint32_t address, uint8_t value) { Bytes[address] = value; }
  void Put16(uint32_t address, uint16_t value) {
    Put8(address, static_cast<uint8_t>(value));
    Put8(address + 1U, static_cast<uint8_t>(value >> 8U));
  }
  void Put32(uint32_t address, uint32_t value) {
    Put16(address, static_cast<uint16_t>(value));
    Put16(address + 2U, static_cast<uint16_t>(value >> 16U));
  }
  bool Read8(uint32_t address, uint8_t *value) {
    const auto found = Bytes.find(address);
    if (found == Bytes.end()) return false;
    *value = found->second;
    return true;
  }
  bool Read16(uint32_t address, uint16_t *value) {
    uint8_t low = 0U, high = 0U;
    if (!Read8(address, &low) || !Read8(address + 1U, &high)) return false;
    *value = static_cast<uint16_t>(low | (high << 8U));
    return true;
  }
  bool Read32(uint32_t address, uint32_t *value) {
    uint16_t low = 0U, high = 0U;
    if (!Read16(address, &low) || !Read16(address + 2U, &high)) return false;
    *value = static_cast<uint32_t>(low) | (static_cast<uint32_t>(high) << 16U);
    return true;
  }
};

std::string ReadText(const char *path) {
  std::ifstream file(path);
  Require(file.good(), std::string("cannot open ") + path);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

void TestAreaFormat() {
  RequireEqual(FormatNativeArea({}), "scene=- room=-", "outside gameplay");
  RequireEqual(FormatNativeArea({0x55, 0}), "scene=0x55:spot04 room=0", "Kokiri Forest");
  RequireEqual(FormatNativeArea({0x00, 12}), "scene=0x00:ydan room=12", "Deku Tree room 12");
  RequireEqual(FormatNativeArea({0x65, 3}), "scene=0x65 room=3", "scene without a stem");
  RequireEqual(FormatNativeArea({0x51, -1}), "scene=0x51:spot00 room=-", "scene without a room");
  RequireEqual(FormatNativeArea({0x1234, 1}), "scene=0x1234 room=1", "scene past the table");
  Require(NativeSceneStem(-1) == nullptr && NativeSceneStem(108) == nullptr, "stem bounds");
  Require(NativeSceneIdByStem("spot04") == 0x55 && NativeSceneIdByStem("spot99") == 107 &&
              NativeSceneIdByStem("nowhere") == -1,
          "id by stem");
  for (size_t scene = 0; scene < kNativeSceneStems.size(); ++scene)
    if (kNativeSceneStems[scene] != nullptr)
      Require(NativeSceneIdByStem(kNativeSceneStems[scene]) == static_cast<int>(scene),
              std::string("unique stem ") + kNativeSceneStems[scene]);
}

// The stems are scene_lights.py's SCENE_NAMES, id for id.
void TestSceneStemsMatchSceneLights() {
  const std::string text = ReadText(OOT3D_SCENE_LIGHTS_SCRIPT);
  const auto begin = text.find("SCENE_NAMES = {");
  Require(begin != std::string::npos, "SCENE_NAMES in scene_lights.py");
  const std::string table = text.substr(begin, text.find('}', begin) - begin);
  const std::regex entry(R"re((\d+): "([a-z0-9_]+)")re");
  size_t named = 0U;
  for (auto it = std::sregex_iterator(table.begin(), table.end(), entry); it != std::sregex_iterator(); ++it) {
    const int scene = std::stoi((*it)[1].str());
    const char *stem = NativeSceneStem(scene);
    Require(stem != nullptr && (*it)[2].str() == stem, "scene stem " + std::to_string(scene));
    ++named;
  }
  size_t stems = 0U;
  for (const char *stem : kNativeSceneStems) stems += stem != nullptr ? 1U : 0U;
  Require(named == stems && named == 102U, "scene stem count");
}

void TestReadArea() {
  constexpr uint32_t kPlay = 0x08100000U;
  GuestMemory memory;
  Require(ReadNativeArea(memory) == NativeArea{}, "unmapped pointer");
  memory.Put32(kNativeGameStatePointer, 0U);
  Require(ReadNativeArea(memory) == NativeArea{}, "no game state");
  memory.Put32(kNativeGameStatePointer, kPlay);
  memory.Put8(kPlay + kNativeGameStateTypeOffset, 2U);
  memory.Put16(kPlay + kNativePlaySceneOffset, 0x55U);
  memory.Put8(kPlay + kNativePlayRoomOffset, 1U);
  Require(ReadNativeArea(memory) == NativeArea{}, "file select is not gameplay");
  memory.Put8(kPlay + kNativeGameStateTypeOffset, kNativeGameStatePlay);
  Require(ReadNativeArea(memory) == NativeArea{0x55, 1}, "gameplay area");
  memory.Put8(kPlay + kNativePlayRoomOffset, 0xFFU);
  Require(ReadNativeArea(memory) == NativeArea{0x55, -1}, "no room");
  memory.Put16(kPlay + kNativePlaySceneOffset, 0xFFFFU);
  Require(ReadNativeArea(memory) == NativeArea{}, "invalid scene");
}

void TestAreaWindow() {
  NativeAreaWindow window;
  Require(window.Dominant().Area == NativeArea{} && window.Dominant().SceneChanges == 0U, "empty window");
  // 10 frames of title, 30 in Kokiri Forest (20 in room 0, 10 in room 1),
  // then 25 in the Deku Tree: Kokiri Forest room 0, one change.
  for (int frame = 0; frame < 10; ++frame) window.Observe({});
  for (int frame = 0; frame < 20; ++frame) window.Observe({0x55, 0});
  for (int frame = 0; frame < 10; ++frame) window.Observe({0x55, 1});
  for (int frame = 0; frame < 25; ++frame) window.Observe({0x00, 0});
  auto summary = window.Dominant();
  Require(summary.Area == NativeArea{0x55, 0} && summary.SceneChanges == 1U, "dominant scene and room");
  // The next window: a change across the boundary counts once; a load
  // through no gameplay between two scenes counts once.
  window.Reset();
  window.Observe({0x55, 2});
  for (int frame = 0; frame < 5; ++frame) window.Observe({});
  window.Observe({0x00, 0});
  summary = window.Dominant();
  Require(summary.SceneChanges == 2U, "changes across the boundary and a load");
  Require(summary.Area == NativeArea{}, "mostly outside gameplay");
  // More distinct areas than slots: the first ones still count.
  window.Reset();
  for (int16_t scene = 0; scene < 40; ++scene) window.Observe({scene, 0});
  for (int frame = 0; frame < 3; ++frame) window.Observe({3, 0});
  summary = window.Dominant();
  Require(summary.Area == NativeArea{3, 0} && summary.SceneChanges == 40U, "full window");
}

// Every name is the Oot3dActorId enumerator of actor_object_semantics.h.
void TestActorNamesMatchDecompIds() {
  const std::string text = ReadText(OOT3D_ACTOR_SEMANTICS_HEADER);
  const auto begin = text.find("typedef enum Oot3dActorId");
  Require(begin != std::string::npos, "Oot3dActorId enum");
  const std::string table = text.substr(begin, text.find('}', begin) - begin);
  const std::regex entry(R"(ACTOR_([A-Z0-9_]+) = (0x[0-9A-Fa-f]+))");
  size_t count = 0U;
  for (auto it = std::sregex_iterator(table.begin(), table.end(), entry); it != std::sregex_iterator(); ++it) {
    const std::string raw = (*it)[1].str();
    const unsigned id = static_cast<unsigned>(std::stoul((*it)[2].str(), nullptr, 16));
    std::string expected;
    if (raw.rfind("UNSET_", 0) != 0) {
      bool start = true;
      for (const char c : raw) {
        expected += start ? c : static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        start = c == '_';
      }
    }
    const char *name = NativeActorName(static_cast<uint16_t>(id));
    Require(expected.empty() ? name == nullptr : name != nullptr && expected == name,
            "actor " + std::to_string(id) + " " + raw);
    ++count;
  }
  Require(count == kNativeActorNames.size(), "actor count");
  Require(std::string(NativeActorName(0)) == "Player" && NativeActorName(0x1D7) == nullptr, "actor bounds");
  Require(std::string(NativeActorCategoryName(5)) == "enemy" && NativeActorCategoryName(12) == nullptr,
          "categories");
}

constexpr uint32_t kPlayer = 0x08200000U;
constexpr uint32_t kTestA = 0x08201000U;
constexpr uint32_t kTestB = 0x08202000U;

void PutActor(GuestMemory &memory, uint32_t actor, uint16_t id, uint8_t category) {
  memory.Put16(actor, id);
  memory.Put8(actor + 2U, category);
}

// One step (Actor_UpdateAll with three actors, AT, OC) and one draw pass
// (Actor_DrawAll drawing two actors, one in the lens pass), in microseconds.
void RunStep(NativeActorProfile &profile, GuestMemory &memory, uint64_t base) {
  const auto hook = [&](uint32_t pc, uint32_t r1, uint32_t r4, uint32_t lr, uint64_t at) {
    Require(profile.Observe(pc, r1, r4, lr, memory, base + at), "profile pc");
    profile.AddHookOverhead(10U);
  };
  hook(kActorProfileUpdateAllEntry, 0U, 0U, kActorProfileUpdateAllReturn, 0U);
  hook(kActorProfileActorUpdateBlock, 0U, kPlayer, 0U, 100U);
  hook(kActorProfileActorUpdateReturn, 0U, kPlayer, 0U, 1100U);
  hook(kActorProfileActorUpdateBlock, 0U, kTestA, 0U, 1200U);
  hook(kActorProfileActorUpdateReturn, 0U, kTestA, 0U, 3200U);
  hook(kActorProfileActorUpdateBlock, 0U, kTestB, 0U, 3300U);
  hook(kActorProfileActorUpdateReturn, 0U, kTestB, 0U, 4300U);
  hook(kActorProfileUpdateAllReturn, 0U, 0U, 0U, 5000U);
  hook(kActorProfileCollisionAtEntry, 0U, 0U, kActorProfileCollisionAtReturn, 6000U);
  hook(kActorProfileCollisionAtReturn, 0U, 0U, 0U, 6500U);
  hook(kActorProfileCollisionOcEntry, 0U, 0U, kActorProfileCollisionOcReturn, 7000U);
  hook(kActorProfileCollisionOcReturn, 0U, 0U, 0U, 8000U);
  hook(kActorProfileDrawAllEntry, 0U, 0U, kActorProfileDrawAllReturn, 9000U);
  hook(kActorProfileActorDrawEntry, kPlayer, 0U, kActorProfileActorDrawReturn, 9100U);
  hook(kActorProfileActorDrawReturn, 0U, 0U, 0U, 9600U);
  hook(kActorProfileActorDrawEntry, kTestA, 0U, kActorProfileActorDrawLensReturn, 9700U);
  hook(kActorProfileActorDrawLensReturn, 0U, 0U, 0U, 9900U);
  hook(kActorProfileDrawAllReturn, 0U, 0U, 0U, 11000U);
}

void TestProfileLine() {
  GuestMemory memory;
  PutActor(memory, kPlayer, 0x0000U, 2U);
  PutActor(memory, kTestA, 0x0002U, 5U);
  PutActor(memory, kTestB, 0x0002U, 5U);
  NativeActorProfile profile(1000.0); // ticks are microseconds
  Require(!profile.Observe(0x00100000U, 0U, 0U, 0U, memory, 0U), "other pcs are not the profile's");
  RunStep(profile, memory, 0U);
  RequireEqual(profile.TakeLine("scene=0x55:spot04 room=0"),
               "actors: scene=0x55:spot04 room=0 steps=1 draws=1 n=3.0 update_ms=5.00 "
               "actor_update_ms=4.00 collide_ms=1.50 at_ms=0.50 oc_ms=1.00 draw_ms=2.00 "
               "actor_draw_ms=0.70 hooks=18 overhead_ms=0.180 dropped=0 "
               "by_cat=enemy:3.20,player:1.50 top=En_Test/enemy:3.20x2,Player/player:1.50x1\n",
               "one step");
  // Per step: two steps give the same per-step values.
  RunStep(profile, memory, 100000U);
  RunStep(profile, memory, 200000U);
  RequireEqual(profile.TakeLine("scene=0x55:spot04 room=0"),
               "actors: scene=0x55:spot04 room=0 steps=2 draws=2 n=3.0 update_ms=5.00 "
               "actor_update_ms=4.00 collide_ms=1.50 at_ms=0.50 oc_ms=1.00 draw_ms=2.00 "
               "actor_draw_ms=0.70 hooks=18 overhead_ms=0.180 dropped=0 "
               "by_cat=enemy:3.20,player:1.50 top=En_Test/enemy:3.20x2,Player/player:1.50x1\n",
               "two steps");
  RequireEqual(profile.TakeLine("scene=- room=-"),
               "actors: scene=- room=- steps=0 draws=0 n=0.0 update_ms=0.00 actor_update_ms=0.00 "
               "collide_ms=0.00 at_ms=0.00 oc_ms=0.00 draw_ms=0.00 actor_draw_ms=0.00 hooks=0 "
               "overhead_ms=0.000 dropped=0 by_cat=- top=-\n",
               "empty window");
}

void TestProfilePairing() {
  GuestMemory memory;
  PutActor(memory, kPlayer, 0x0000U, 2U);
  PutActor(memory, kTestA, 0x0002U, 5U);
  NativeActorProfile profile(1000.0);
  // Another caller (lr elsewhere) is not timed; its return pc alone is ignored.
  profile.Observe(kActorProfileCollisionOcEntry, 0U, 0U, 0x00123456U, memory, 0U);
  profile.Observe(kActorProfileCollisionOcReturn, 0U, 0U, 0U, memory, 500U);
  profile.Observe(kActorProfileActorDrawEntry, kPlayer, 0U, 0x00123456U, memory, 600U);
  profile.Observe(kActorProfileActorDrawReturn, 0U, 0U, 0U, memory, 700U);
  Require(profile.Window().Dropped() == 0U && profile.Window().DrawPasses() == 0U, "foreign callers");
  Require(profile.Window().ActorSlot(0).Draws == 0U, "foreign actor draw");
  // A second entry before the return drops the first interval.
  profile.Observe(kActorProfileUpdateAllEntry, 0U, 0U, kActorProfileUpdateAllReturn, memory, 1000U);
  profile.Observe(kActorProfileUpdateAllEntry, 0U, 0U, kActorProfileUpdateAllReturn, memory, 2000U);
  // The return of another actor drops the update.
  profile.Observe(kActorProfileActorUpdateBlock, 0U, kPlayer, 0U, memory, 2100U);
  profile.Observe(kActorProfileActorUpdateReturn, 0U, kTestA, 0U, memory, 2200U);
  // An actor that cannot be read is not timed.
  profile.Observe(kActorProfileActorUpdateBlock, 0U, 0x09000000U, 0U, memory, 2300U);
  profile.Observe(kActorProfileActorUpdateReturn, 0U, 0x09000000U, 0U, memory, 2400U);
  // An update still open when Actor_UpdateAll returns is dropped.
  profile.Observe(kActorProfileActorUpdateBlock, 0U, kTestA, 0U, memory, 2500U);
  profile.Observe(kActorProfileUpdateAllReturn, 0U, 0U, 0U, memory, 3000U);
  Require(profile.Window().Dropped() == 3U, "dropped intervals");
  Require(profile.Window().Steps() == 1U && profile.Window().ActorUpdates() == 0U, "step without actors");
  // Over 250 ms (a pause or a stall in between) is dropped.
  profile.Observe(kActorProfileCollisionAtEntry, 0U, 0U, kActorProfileCollisionAtReturn, memory, 10000U);
  profile.Observe(kActorProfileCollisionAtReturn, 0U, 0U, 0U, memory, 10000U + 260000U);
  Require(profile.Window().Dropped() == 4U, "interval too long");
}

void TestProfileTop() {
  GuestMemory memory;
  NativeActorProfile profile(1000.0);
  // 11 actor ids with 1..11 ms of update in one step; ids 20 and 21 tie at 5 ms;
  // id 0x0001 has no name and 0x0300 is past the table.
  const uint16_t ids[] = {0x0001, 0x0300, 4, 7, 8, 9, 10, 11, 12, 20, 21};
  const uint64_t ms[] = {12, 11, 1, 2, 3, 4, 6, 7, 8, 5, 5};
  profile.Observe(kActorProfileUpdateAllEntry, 0U, 0U, kActorProfileUpdateAllReturn, memory, 0U);
  uint64_t now = 0U;
  for (size_t index = 0; index < std::size(ids); ++index) {
    const uint32_t actor = 0x08300000U + static_cast<uint32_t>(index) * 0x1000U;
    PutActor(memory, actor, ids[index], 5U);
    profile.Observe(kActorProfileActorUpdateBlock, 0U, actor, 0U, memory, now);
    now += ms[index] * 1000U;
    profile.Observe(kActorProfileActorUpdateReturn, 0U, actor, 0U, memory, now);
  }
  profile.Observe(kActorProfileUpdateAllReturn, 0U, 0U, 0U, memory, now);
  std::array<NativeActorProfileWindow::TopEntry, NativeActorProfileWindow::kTopActors> top{};
  const size_t count = profile.Window().Top(1000.0, top);
  Require(count == 8U, "top count");
  const uint16_t expected[] = {0x0001, NativeActorProfileWindow::kActorSlots - 1U, 12, 11, 10, 20, 21, 9};
  for (size_t index = 0; index < count; ++index)
    Require(top[index].Id == expected[index], "top order " + std::to_string(index));
  const std::string line = profile.TakeLine("scene=- room=-");
  Require(line.find(" top=0x0001/enemy:12.00x1,other/enemy:11.00x1,Bg_Hidan_Firewall/enemy:8.00x1,") !=
              std::string::npos,
          "top names: " + line);
  Require(line.find(",En_Door/enemy:4.00x1\n") != std::string::npos, "top ends at 8: " + line);
}

// config/indirect_profile: pairs counted per window, the hottest first (ties
// by return then target), the line and the reset.
void TestIndirectProfile() {
  Oot3dIndirectProfile profile;
  RequireEqual(profile.TakeLine(), "indirect: calls=0 pairs=0 dropped=0 top=-\n", "empty indirect line");
  for (int index = 0; index < 5; ++index) profile.Record(0x00461788U, 0x0021A000U);
  for (int index = 0; index < 3; ++index) profile.Record(0x002E4058U, 0x00461344U);
  profile.Record(0x002E4058U, 0x00461340U);
  profile.Record(0x00100004U, 0x00461340U);
  Require(profile.Calls() == 10U && profile.Pairs() == 4U, "indirect counts");
  RequireEqual(profile.TakeLine(),
               "indirect: calls=10 pairs=4 dropped=0 "
               "top=00461788>0021A000:5,002E4058>00461344:3,00100004>00461340:1,002E4058>00461340:1\n",
               "indirect line");
  Require(profile.Calls() == 0U && profile.Pairs() == 0U, "indirect reset");
  // More distinct pairs than the top holds: the line keeps the 16 hottest.
  for (uint32_t pair = 0; pair < 40U; ++pair) {
    for (uint32_t count = 0; count <= pair; ++count) profile.Record(0x00200000U + pair * 4U, 0x00300000U);
  }
  const std::string line = profile.TakeLine();
  Require(line.find(" top=0020009C>00300000:40,00200098>00300000:39,") != std::string::npos, "indirect top: " + line);
  Require(line.find(",00200060>00300000:25\n") != std::string::npos, "indirect top ends at 16: " + line);
  // A full table drops instead of evicting.
  for (uint32_t pair = 0; pair < 2U * Oot3dIndirectProfile::kSlots; ++pair) profile.Record(pair * 4U, 0U);
  Require(profile.Dropped() != 0U && profile.Pairs() + profile.Dropped() == 2U * Oot3dIndirectProfile::kSlots,
          "indirect drops when full");
  {
    Oot3dIndirectProfileInstall install;
    Require(gOot3dIndirectProfile == &install.Profile(), "indirect profile installed");
  }
  Require(gOot3dIndirectProfile == nullptr, "indirect profile removed");
}

} // namespace

int main() {
  TestAreaFormat();
  TestSceneStemsMatchSceneLights();
  TestReadArea();
  TestAreaWindow();
  TestActorNamesMatchDecompIds();
  TestProfileLine();
  TestProfilePairing();
  TestProfileTop();
  TestIndirectProfile();
  std::puts("perf diagnostics: PASS");
  return 0;
}
