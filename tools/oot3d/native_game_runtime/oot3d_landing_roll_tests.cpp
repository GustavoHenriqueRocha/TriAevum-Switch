// Landing roll (N64): the decision at the fall action's landing (0x004BC080)
// and the reads it is made from (oot3d_landing_roll.h).
#include "oot3d_landing_roll.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <vector>

namespace {

using namespace Oot3dNativeGame;

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

// Guest memory holding one player at kPlayer (little endian, like the 3DS).
class FakeBus {
public:
  static constexpr std::uint32_t kPlayer = 0x08100000U;
  std::vector<std::uint8_t> Bytes = std::vector<std::uint8_t>(0x3000U, 0U);
  bool Read8(std::uint32_t address, std::uint8_t *value) { return Copy(address, value, 1U); }
  bool Read16(std::uint32_t address, std::uint16_t *value) { return Copy(address, value, 2U); }
  bool Read32(std::uint32_t address, std::uint32_t *value) { return Copy(address, value, 4U); }
  template <class T> void Put(std::uint32_t offset, T value) {
    std::memcpy(Bytes.data() + offset, &value, sizeof(T));
  }

private:
  bool Copy(std::uint32_t address, void *value, std::uint32_t size) {
    if (address < kPlayer || address - kPlayer + size > Bytes.size()) return false;
    std::memcpy(value, Bytes.data() + (address - kPlayer), size);
    return true;
  }
};

constexpr std::uint32_t kOtherLandingAnimation = 0x6AU;  // not the run jump
constexpr std::int8_t kStickNone = -1, kStickForward = 0, kStickRight = 1, kStickBack = 2;

struct Case {
  const char *Name;
  std::uint32_t Animation;
  std::int16_t ActionVar2;
  std::uint32_t StateFlags1;
  std::int16_t FallDistance;
  std::int8_t Stick;
  LandingRollChoice Expected;
};

constexpr auto kRoll = LandingRollChoice::Roll;
constexpr auto kGame = LandingRollChoice::GameLanding;
constexpr std::uint32_t kJump = kLandingRollRunJumpAnimation;

// A run jump off a ledge reaches the ground with actionVar2 = -1 (or -2
// after the long-fall voice), the run-jump animation and, unless noted,
// nothing carried and no lock-on.
constexpr Case kCases[] = {
    {"fall 80, forward: short landing", kJump, -1, 0U, 80, kStickForward, kGame},
    {"fall 81, forward: roll", kJump, -1, 0U, 81, kStickForward, kRoll},
    {"fall 400, forward: roll, no damage", kJump, -1, 0U, 400, kStickForward, kRoll},
    {"fall 799, forward: roll, no damage", kJump, -1, 0U, 799, kStickForward, kRoll},
    {"fall 800, forward: big fall damage", kJump, -1, 0U, 800, kStickForward, kGame},
    {"fall 400 after the long-fall voice (av2 -2), forward: roll", kJump, -2, 0U, 400, kStickForward, kRoll},
    {"fall 80, stick released", kJump, -1, 0U, 80, kStickNone, kGame},
    {"fall 400, stick released", kJump, -1, 0U, 400, kStickNone, kGame},
    {"fall 799, stick released", kJump, -1, 0U, 799, kStickNone, kGame},
    {"fall 800, stick released", kJump, -1, 0U, 800, kStickNone, kGame},
    {"fall 400, stick back", kJump, -1, 0U, 400, kStickBack, kGame},
    {"fall 400, stick sideways", kJump, -1, 0U, 400, kStickRight, kGame},
    {"fall 400, forward, carrying", kJump, -1, kLandingRollStateCarrying, 400, kStickForward, kGame},
    {"fall 400, forward, hostile lock-on", kJump, -1, kLandingRollStateHostileLockOn, 400, kStickForward,
     kGame},
    {"fall 799, forward, lock-on and carrying", kJump, -1,
     kLandingRollStateHostileLockOn | kLandingRollStateCarrying, 799, kStickForward, kGame},
    {"fall 400, forward, unrelated state bits", kJump, -1, 0x00040001U, 400, kStickForward, kRoll},
    // Run-jump end: the jump landed before N64 would have switched animation.
    {"run-jump end (av2 1), fall 400, forward", kJump, 1, 0U, 400, kStickForward, kGame},
    {"run-jump end (av2 0), fall 400, forward", kJump, 0, 0U, 400, kStickForward, kGame},
    // Without the run-jump animation the game's own check already decides.
    {"walked off, fall 400, forward: game's own roll", kOtherLandingAnimation, -1, 0U, 400, kStickForward,
     kGame},
    {"walked off, fall 800, forward", kOtherLandingAnimation, -1, 0U, 800, kStickForward, kGame},
};

void Decisions() {
  for (const auto &test : kCases) {
    LandingRollInputs in;
    in.Animation = test.Animation;
    in.ActionVar2 = test.ActionVar2;
    in.StateFlags1 = test.StateFlags1;
    in.FallDistance = test.FallDistance;
    in.StickDirection = static_cast<std::uint8_t>(test.Stick);
    if (DecideLandingRoll(in) != test.Expected) {
      std::fprintf(stderr, "  case: %s\n", test.Name);
      Require(false, "decision");
    }
  }
  static_assert(DecideLandingRoll({kJump, 0U, -1, 400, 0U}) == LandingRollChoice::Roll);
}

// The same cases through the guest reads, with the stick ring index at each
// of its four slots (Player_ProcessControlStick: (index + 1) % 4).
void ReadsFromGuestMemory() {
  int checked = 0;
  for (const auto &test : kCases) {
    for (std::uint8_t index = 0U; index < 4U; ++index) {
      FakeBus bus;
      bus.Put<std::uint32_t>(kLandingRollAnimationOffset, test.Animation);
      bus.Put<std::uint32_t>(kLandingRollStateFlags1Offset, test.StateFlags1);
      bus.Put<std::int16_t>(kLandingRollActionVar2Offset, test.ActionVar2);
      bus.Put<std::int16_t>(kLandingRollFallDistanceOffset, test.FallDistance);
      bus.Put<std::uint8_t>(kLandingRollStickIndexOffset, index);
      for (std::uint8_t slot = 0U; slot < 4U; ++slot)  // other slots: the opposite answer
        bus.Put<std::int8_t>(kLandingRollStickDirectionsOffset + slot,
                             test.Stick == kStickForward ? kStickNone : kStickForward);
      bus.Put<std::int8_t>(kLandingRollStickDirectionsOffset + index, test.Stick);
      LandingRollInputs in;
      Require(ReadLandingRollInputs(bus, FakeBus::kPlayer, &in), "fields read");
      if (DecideLandingRoll(in) != test.Expected) {
        std::fprintf(stderr, "  case: %s (stick slot %u)\n", test.Name, index);
        Require(false, "decision from guest memory");
      }
      ++checked;
    }
  }
  Require(checked == static_cast<int>(std::size(kCases)) * 4, "every case at every slot");
  FakeBus bus;
  LandingRollInputs in;
  Require(!ReadLandingRollInputs(bus, 0U, &in), "no player, no decision");
  Require(!ReadLandingRollInputs(bus, FakeBus::kPlayer + 0x1000U, &in), "a failed read declines");
}

void Setting() {
  Require(LandingRollEnabled().load(), "on by default");
  const auto dir = std::filesystem::temp_directory_path() / "oot3d_landing_roll_test";
  std::error_code error;
  std::filesystem::remove_all(dir, error);
  Require(ReadLandingRollEnabled(dir), "absent = on");
  Require(SaveLandingRollEnabled(dir, false) && !ReadLandingRollEnabled(dir), "off writes no_landing_roll");
  Require(std::filesystem::exists(dir / "no_landing_roll"), "file name");
  Require(SaveLandingRollEnabled(dir, true) && ReadLandingRollEnabled(dir), "on removes the file");
  Require(SaveLandingRollEnabled(dir, true), "removing an absent file is fine");
  std::filesystem::remove_all(dir, error);
}

} // namespace

int main() {
  Decisions();
  ReadsFromGuestMemory();
  Setting();
  std::printf("landing roll: PASS (%zu cases)\n", std::size(kCases));
  return 0;
}
