// Simple shadow, game side (oot3d_simple_shadow.h): the mod's conditions, the
// actor and floor reads, the hook's foot shadows at alpha 0 and the config file.
#include "oot3d_simple_shadow.h"

#include <cstdio>
#include <cstdlib>
#include <map>

namespace {

using namespace Oot3dNativeGame;

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

struct FakeMemory {
  std::map<std::uint32_t, std::uint8_t> Bytes;
  int Writes = 0;
  bool Read8(std::uint32_t address, std::uint8_t *value) {
    const auto it = Bytes.find(address);
    if (it == Bytes.end()) return false;
    *value = it->second;
    return true;
  }
  bool Read16(std::uint32_t address, std::uint16_t *value) {
    std::uint8_t low = 0U, high = 0U;
    if (!Read8(address, &low) || !Read8(address + 1U, &high)) return false;
    *value = static_cast<std::uint16_t>(low | (high << 8U));
    return true;
  }
  bool Read32(std::uint32_t address, std::uint32_t *value) {
    std::uint16_t low = 0U, high = 0U;
    if (!Read16(address, &low) || !Read16(address + 2U, &high)) return false;
    *value = low | (static_cast<std::uint32_t>(high) << 16U);
    return true;
  }
  bool Write8(std::uint32_t address, std::uint8_t value) {
    Bytes[address] = value;
    ++Writes;
    return true;
  }
  void Put8(std::uint32_t address, std::uint8_t value) { Bytes[address] = value; }
  void Put16(std::uint32_t address, std::uint16_t value) {
    Put8(address, static_cast<std::uint8_t>(value));
    Put8(address + 1U, static_cast<std::uint8_t>(value >> 8U));
  }
  void Put32(std::uint32_t address, std::uint32_t value) {
    Put16(address, static_cast<std::uint16_t>(value));
    Put16(address + 2U, static_cast<std::uint16_t>(value >> 16U));
  }
  void PutFloat(std::uint32_t address, float value) { Put32(address, std::bit_cast<std::uint32_t>(value)); }
};

constexpr std::uint32_t kLink = 0x08400000U;
constexpr std::uint32_t kPoly = 0x08600010U;
constexpr std::uint32_t kCaller = 0x002D649CU;

SimpleShadowActor Standing() {
  SimpleShadowActor actor;
  actor.Address = kLink;
  actor.Id = 0U;
  actor.Position = {100.0F, 20.0F, -50.0F};
  actor.FloorPoly = kPoly;
  actor.FloorHeight = 20.0F;
  actor.ShadowAlpha = 255U;
  actor.FloorNormal = {0, 32767, 0};
  return actor;
}

void PutLink(FakeMemory &memory, const SimpleShadowActor &actor) {
  memory.Put16(kLink + kSimpleShadowIdOffset, actor.Id);
  for (std::uint32_t axis = 0; axis < 3U; ++axis)
    memory.PutFloat(kLink + kSimpleShadowPositionOffset + 4U * axis, actor.Position[axis]);
  memory.Put32(kLink + kSimpleShadowFloorPolyOffset, actor.FloorPoly);
  memory.PutFloat(kLink + kSimpleShadowFloorHeightOffset, actor.FloorHeight);
  memory.Put8(kLink + kSimpleShadowAlphaOffset, actor.ShadowAlpha);
  memory.Put32(kLink + kSimpleShadowStateFlags1Offset, actor.StateFlags1);
  memory.Put32(kLink + kSimpleShadowStateFlags2Offset, actor.StateFlags2);
  for (std::uint32_t axis = 0; axis < 3U; ++axis)
    memory.Put16(kPoly + kSimpleShadowPolyNormalOffset + 2U * axis,
                 static_cast<std::uint16_t>(actor.FloorNormal[axis]));
}

void TestDecision() {
  const auto frame = DecideSimpleShadow(Standing());
  Require(frame.has_value(), "standing on a flat floor");
  Require(frame->Actor == kLink && frame->Height == 0.0F && frame->FloorPoint[0] == 100.0F &&
              frame->FloorPoint[1] == 20.0F && frame->FloorPoint[2] == -50.0F && frame->FloorNormal[1] == 1.0F,
          "the floor under him");
  Require(frame->Opacity == Fast::Oot3d::PlayerShadowOpacity(255U, 0.0F), "the mod's opacity");
  const auto reject = [](auto change, const char *message) {
    auto actor = Standing();
    change(actor);
    Require(!DecideSimpleShadow(actor).has_value(), message);
  };
  reject([](SimpleShadowActor &a) { a.Id = 1U; }, "not the player");
  reject([](SimpleShadowActor &a) { a.StateFlags1 = kSimpleShadowRiding; }, "on Epona: the game draws none");
  reject([](SimpleShadowActor &a) { a.StateFlags1 = kSimpleShadowCutscene; }, "in a cutscene: the foot shadows");
  reject([](SimpleShadowActor &a) { a.StateFlags2 = kSimpleShadowHidden; }, "the flag the mod skips");
  reject([](SimpleShadowActor &a) { a.FloorPoly = 0U; }, "no floor");
  reject([](SimpleShadowActor &a) { a.ShadowAlpha = 0U; }, "no shadow alpha");
  reject([](SimpleShadowActor &a) { a.Position[1] = a.FloorHeight - 60.0F; }, "50 units under the floor");
  reject([](SimpleShadowActor &a) { a.Position[1] = a.FloorHeight + 1500.0F; }, "1500 units over it");
  reject([](SimpleShadowActor &a) { a.FloorNormal = {30000, 6000, 0}; }, "a floor steeper than 0.3");
  auto high = Standing();
  high.Position[1] += 400.0F;
  Require(DecideSimpleShadow(high).has_value(), "high up, the mod still draws it");
  auto sloped = Standing();
  sloped.FloorNormal = {0, 26214, 19660};
  const auto onSlope = DecideSimpleShadow(sloped);
  Require(onSlope && std::abs(onSlope->FloorNormal[1] * onSlope->FloorNormal[1] +
                              onSlope->FloorNormal[2] * onSlope->FloorNormal[2] - 1.0F) < 1.0e-4F,
          "a sloped floor's unit normal");
}

void TestRead() {
  FakeMemory memory;
  auto actor = Standing();
  actor.FloorNormal = {-200, 32000, 300};
  actor.StateFlags1 = 0x10U;
  PutLink(memory, actor);
  const auto read = ReadSimpleShadowActor(memory, kLink);
  Require(read && read->Id == 0U && read->Position == actor.Position && read->FloorPoly == kPoly &&
              read->FloorHeight == 20.0F && read->ShadowAlpha == 255U && read->StateFlags1 == 0x10U &&
              read->FloorNormal == actor.FloorNormal,
          "the actor and its floor normal");
  Require(!ReadSimpleShadowActor(memory, 0U).has_value(), "no actor");
  FakeMemory missing = memory;
  missing.Bytes.erase(kPoly + kSimpleShadowPolyNormalOffset + 2U);
  Require(!ReadSimpleShadowActor(missing, kLink).has_value(), "an unreadable floor");
}

void TestHook() {
  FakeMemory memory;
  PutLink(memory, Standing());
  SimpleShadowRuntime runtime;
  oot3d::recomp::a32::GuestState state;
  state.r[0] = kLink;
  state.r[14] = kCaller;
  Require(!runtime.Execute(state, memory, false, true).has_value() && memory.Writes == 0 && !runtime.Pending() &&
              !runtime.TakeFrame().has_value(),
          "off: the block runs as is");
  Require(!runtime.Execute(state, memory, true, false).has_value() && memory.Writes == 0 && !runtime.Pending(),
          "outside compiled code: no sequence starts");

  Require(!runtime.Execute(state, memory, true, true).has_value(), "on: the block runs on");
  std::uint8_t alpha = 0xFFU;
  Require(memory.Read8(kLink + kSimpleShadowAlphaOffset, &alpha) && alpha == 0U, "the foot shadows at alpha 0");
  Require(state.r[14] == kSimpleShadowHookPc && runtime.Pending(), "ActorShadow_DrawFeet returns into the hook");
  state.r[0] = 0x1234U; // clobbered by the foot shadows
  state.r[14] = 0x001D0818U;
  const auto exit = runtime.Execute(state, memory, true, true);
  Require(exit == kCaller && state.r[14] == kCaller && !runtime.Pending(), "back to Actor_Draw");
  Require(memory.Read8(kLink + kSimpleShadowAlphaOffset, &alpha) && alpha == 255U, "the alpha back");
  const auto frame = runtime.TakeFrame();
  Require(frame && frame->Actor == kLink && !runtime.TakeFrame().has_value() && runtime.ShadowFrames() == 1U,
          "the frame's shadow, taken once");

  state.r[0] = kLink;
  state.r[1] = 0x08500000U; // lights
  state.r[2] = 0x08001000U; // play
  state.r[14] = kCaller;
  (void)runtime.Execute(state, memory, true, true);
  Require(runtime.Execute(state, memory, false, true) == kCaller, "turned off between the two: still finished");

  state.r[0] = kLink;
  state.r[14] = kCaller;
  (void)runtime.Execute(state, memory, true, true);
  state.r[0] = 0x1234U;
  state.r[1] = state.r[2] = 0U;
  state.r[14] = 0x001D0818U;
  Require(!runtime.Execute(state, memory, true, false).has_value() && !runtime.Pending(),
          "a return outside compiled code: the block runs again");
  Require(state.r[0] == kLink && state.r[1] == 0x08500000U && state.r[2] == 0x08001000U && state.r[14] == kCaller,
          "with the call's arguments, back to the caller");
  Require(memory.Read8(kLink + kSimpleShadowAlphaOffset, &alpha) && alpha == 255U, "the foot shadows visible");

  auto riding = Standing();
  riding.StateFlags1 = kSimpleShadowRiding;
  FakeMemory onEpona;
  PutLink(onEpona, riding);
  state.r[0] = kLink;
  state.r[14] = kCaller;
  Require(!runtime.Execute(state, onEpona, true, true).has_value() && onEpona.Writes == 0 && !runtime.Pending(),
          "on Epona: the game's own (no) shadow");

  state.r[14] = kCaller;
  (void)runtime.Execute(state, memory, true, true);
  state.r[14] = kCaller; // a new call before the foot shadows came back
  Require(!runtime.Execute(state, memory, true, true).has_value() && runtime.Pending() &&
              state.r[14] == kSimpleShadowHookPc,
          "a new call: the alpha back, then a new sequence");
  state.r[14] = 0x001D0818U;
  Require(runtime.Execute(state, memory, true, true) == kCaller, "which returns as usual");
  Require(memory.Read8(kLink + kSimpleShadowAlphaOffset, &alpha) && alpha == 255U, "the alpha kept");

  state.r[14] = kCaller;
  (void)runtime.Execute(state, memory, true, true);
  runtime.Reset();
  Require(!runtime.Pending() && !runtime.TakeFrame().has_value(), "a state load resets it");
}

void TestConfig() {
  Require(!SimpleShadowEnabled().load(), "off by default");
  const auto config = std::filesystem::temp_directory_path() / "oot3d_simple_shadow_tests";
  std::error_code ignored;
  std::filesystem::remove_all(config, ignored);
  Require(!ReadSimpleShadowEnabled(config), "no file: off");
  Require(SaveSimpleShadowEnabled(config, true) && ReadSimpleShadowEnabled(config), "on writes the file");
  Require(SaveSimpleShadowEnabled(config, false) && !ReadSimpleShadowEnabled(config), "off removes it");
  std::filesystem::remove_all(config, ignored);
}

} // namespace

int main() {
  TestDecision();
  TestRead();
  TestHook();
  TestConfig();
  std::puts("oot3d_simple_shadow_tests: ok");
  return 0;
}
