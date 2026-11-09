// Roll dash runtime (oot3d_roll_dash_runtime.h): the dash state at the
// Player_Update entry and its native query, the speed sites, Navi's hat
// redirect, the CSAB answer from zelda_link_ext.zar and the 3.1 cross-fade,
// on a synthetic guest. Adapted from coccofresco/TriAevum's
// oot3d_top_screen_gameplay_runtime_tests.cpp (54fe8edf, 5b34bd61, 3c2ed0f3,
// a3eeb1e2), without its other options.
#include "oot3d_roll_dash_runtime.h"

#include <nlohmann/json.hpp>

#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace Oot3dNativeGame;
using GuestState = oot3d::recomp::a32::GuestState;
using ExecutionResult = oot3d::recomp::a32::ExecutionResult;

constexpr std::uint32_t kPlay = 0x01000000U;
constexpr std::uint32_t kPlayer = 0x01100000U;
constexpr std::uint32_t kArchive = 0x01200000U;
constexpr std::uint32_t kIdleObject = 0x01300000U;
constexpr std::uint32_t kNavi = 0x01400000U;
constexpr std::uint32_t kStack = 0x02000000U;
constexpr std::uint32_t kRollAction = 0x1234U;
constexpr std::uint32_t kRunAction = 0x5678U;
constexpr std::uint32_t kNmlDash = 0x6DU; // kTopScreenLinkExtAnimations[1]
constexpr std::uint32_t kNativeCsabEntry = 0x01201000U;

void Require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

float F(std::uint32_t bits) { return std::bit_cast<float>(bits); }
std::uint32_t B(float value) { return std::bit_cast<std::uint32_t>(value); }

void Put32(std::vector<std::uint8_t> &bytes, std::size_t offset, std::uint32_t value) {
  if (bytes.size() < offset + 4U) bytes.resize(offset + 4U);
  std::memcpy(bytes.data() + offset, &value, 4U);
}

// A ZAR v1 archive with one csab type: adult/<name>.csab files whose
// payload starts with "csab", +0x14 the frame header offset (0x20) and
// +0x30 the frame count (29).
std::vector<std::uint8_t> MakeLinkExt(const std::vector<std::string> &names) {
  std::vector<std::uint8_t> bytes(0x20U, 0U);
  Put32(bytes, 0U, 0x0152415AU);
  bytes[8] = 1U;
  const std::size_t types = bytes.size();
  bytes.resize(types + 0x10U);
  const auto text = [&](const std::string &value) {
    const std::size_t at = bytes.size();
    bytes.insert(bytes.end(), value.begin(), value.end());
    bytes.push_back(0U);
    while (bytes.size() % 4U != 0U) bytes.push_back(0U);
    return static_cast<std::uint32_t>(at);
  };
  std::vector<std::uint32_t> nameOffsets;
  for (const auto &name : names) nameOffsets.push_back(text("adult/" + name + ".csab"));
  const std::uint32_t typeName = text("csab");
  const std::size_t list = bytes.size();
  for (std::size_t i = 0; i < names.size(); ++i) Put32(bytes, list + i * 4U, static_cast<std::uint32_t>(i));
  const std::size_t meta = bytes.size();
  const std::size_t data = meta + names.size() * 8U;
  bytes.resize(data + names.size() * 4U);
  for (std::size_t i = 0; i < names.size(); ++i) {
    const std::size_t at = bytes.size();
    Put32(bytes, meta + i * 8U, 0x40U);
    Put32(bytes, meta + i * 8U + 4U, nameOffsets[i]);
    Put32(bytes, data + i * 4U, static_cast<std::uint32_t>(at));
    bytes.resize(at + 0x40U, 0U);
    std::memcpy(bytes.data() + at, "csab", 4U);
    Put32(bytes, at + 0x14U, 0x20U);
    Put32(bytes, at + 0x30U, 29U);
  }
  Put32(bytes, 0x0CU, static_cast<std::uint32_t>(types));
  Put32(bytes, 0x10U, static_cast<std::uint32_t>(meta));
  Put32(bytes, 0x14U, static_cast<std::uint32_t>(data));
  Put32(bytes, types + 0U, static_cast<std::uint32_t>(names.size()));
  Put32(bytes, types + 4U, static_cast<std::uint32_t>(list));
  Put32(bytes, types + 8U, typeName);
  return bytes;
}

std::filesystem::path WriteLinkExt() {
  const auto directory = std::filesystem::temp_directory_path() / "oot3d_roll_dash_runtime_tests";
  std::filesystem::create_directories(directory);
  const auto path = directory / kRollDashAnimationsName;
  const auto bytes = MakeLinkExt({"nml_dash", "nml_dash_free", "ft_dash"});
  std::ofstream(path, std::ios::binary)
      .write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return path;
}

struct Fixture {
  NativeA32Memory Memory;
  RollDashRuntime Runtime;
  ExecutionResult Result{};
  std::uint32_t Blocks = 0;

  explicit Fixture(std::filesystem::path animations = {}) : Runtime(Memory, std::move(animations)) {
    std::string error;
    Require(Memory.MapRegion({"globals", 0x00504000U, 0x1000U, true, false, {}}, &error) &&
                Memory.MapRegion({"time state pointer", 0x0051B000U, 0x1000U, true, false, {}}, &error) &&
                Memory.MapRegion({"roll table", 0x0053A000U, 0x1000U, true, false, {}}, &error) &&
                Memory.MapRegion({"save", 0x00587000U, 0x1000U, true, false, {}}, &error) &&
                Memory.MapRegion({"play", kPlay, 0x10000U, true, false, {}}, &error) &&
                Memory.MapRegion({"player", kPlayer, 0x3000U, true, false, {}}, &error) &&
                Memory.MapRegion({"archive", kArchive, 0x2000U, true, false, {}}, &error) &&
                Memory.MapRegion({"idle", kIdleObject, 0x1000U, true, false, {}}, &error) &&
                Memory.MapRegion({"navi", kNavi, 0x1000U, true, false, {}}, &error) &&
                Memory.MapRegion({"stack", kStack - 0x10000U, 0x10000U, true, false, {}}, &error),
            "cannot map the fixture");
    W32(0x005043D4U + 0x0CU, kPlay);    // the play state pointer
    W8(kPlay + 0x100U, 3U);              // gameplay
    W8(kPlay + 0x101U, 2U);              // running
    W32(kPlay + 0x20ACU, kPlayer);
    W16(0x00587958U + 0x44U, 1U);        // alive
    W32(0x0014E9F0U * 4U + 4U, kRollAction); // roll action of animation age 1
    W8(kPlayer + 0x1B3U, 1U);
    W32(kPlayer + 0x284U, kRunAction);
    W16(kPlayer + 0x90U, 1U);            // on the ground
    // Link's archive: group 0 of 0x246 animations, a native entry for nml_dash.
    W32(kPlayer + 0x258U, kArchive);
    W32(kArchive + 0x20U, 0U);
    W32(kArchive + 0x0CU, kArchive + 0x100U);
    W32(kArchive + 0x100U, 0x246U);
    W32(kArchive + 0x50U, kArchive + 0x200U);
    W32(kArchive + 0x200U + kNmlDash * 4U, kNativeCsabEntry);
    W32(kNativeCsabEntry, 0x01201800U);
    // The idle word 3.0 swaps while dashing, and the update rate (2: 30 Hz).
    W32(0x0051B2F4U, kIdleObject);
    W16(kIdleObject + 0x60U, 0x1B2U);
    W16(kIdleObject + 0x110U, 2U);
  }
  void W8(std::uint32_t address, std::uint8_t value) {
    Require(Memory.Write8(address, value), "fixture byte write failed");
  }
  void W16(std::uint32_t address, std::uint16_t value) {
    Require(Memory.Write16(address, value), "fixture halfword write failed");
  }
  void W32(std::uint32_t address, std::uint32_t value) {
    Require(Memory.Write32(address, value), "fixture word write failed");
  }
  std::uint32_t R32(std::uint32_t address) {
    std::uint32_t value = 0U;
    Require(Memory.Read32(address, &value), "fixture word read failed");
    return value;
  }
  std::uint16_t R16(std::uint32_t address) {
    std::uint16_t value = 0U;
    Require(Memory.Read16(address, &value), "fixture halfword read failed");
    return value;
  }
  // As the window's block-entry policy calls it.
  bool Execute(std::uint32_t pc, GuestState &state) {
    Blocks = 0U;
    if (!Runtime.ShouldObserve(pc)) return false;
    const bool handled = Runtime.Execute(pc, state, &Result, &Blocks);
    if (handled)
      Require(Blocks == 1U && Result.kind == oot3d::recomp::a32::ExitKind::Branch &&
                  Result.pc == state.r[15],
              "a handled hook did not branch once to r15");
    return handled;
  }
  // One Player_Update with A held or not and Link rolling or running; when
  // the preconditions hold, the native query returns `blocked`.
  void Update(bool aHeld, bool rolling, bool blocked = false) {
    W32(kPlay + 0x14U, aHeld ? 1U : 0U);
    W32(kPlayer + 0x284U, rolling ? kRollAction : kRunAction);
    GuestState entry;
    entry.r[0] = kPlayer;
    entry.r[1] = kPlay;
    entry.r[13] = kStack;
    entry.r[14] = 0x001E0000U;
    entry.r[15] = kRollDashStateEntry;
    const GuestState title = entry;
    if (!Execute(kRollDashStateEntry, entry)) {
      Require(!aHeld || !Runtime.Enabled(), "the dash state did not query the native block");
      return;
    }
    Require(entry.r[15] == 0x0037577CU && entry.r[14] == kRollDashStateEntry && entry.r[0] == kPlay &&
                Runtime.DashQueryPending(),
            "the dash state did not call FUN_0037577c(play) returning to the entry");
    // FUN_0037577c returns to the entry with its own registers (#91).
    entry.r[0] = blocked ? 1U : 0U;
    entry.r[1] = kPlayer + 0x1000U;
    entry.r[14] = 0x00375798U;
    Require(!Execute(kRollDashStateEntry, entry), "the query's return left the native block");
    Require(!Runtime.DashQueryPending() && entry.r[0] == title.r[0] && entry.r[1] == title.r[1] &&
                entry.r[14] == title.r[14] && entry.r[13] == title.r[13],
            "the title's registers were not restored after the query");
  }
};

GuestState RollCall(std::uint32_t index, float speed) {
  GuestState state;
  state.r[4] = kPlayer;
  state.r[8] = index;
  state.r[9] = kPlay;
  state.r[13] = kStack;
  state.r[14] = 0x002BE000U;
  state.r[15] = kRollDashRollSite;
  state.vfp[0] = B(speed);
  return state;
}

void OffObservesNothing() {
  Fixture f(WriteLinkExt());
  f.Runtime.Prepare(false);
  for (const std::uint32_t pc : RollDashRuntime::HookPcs())
    Require(!f.Runtime.ShouldObserve(pc), "a hook observes with Roll dash off");
  f.W32(kPlay + 0x14U, 1U);
  f.W32(kPlayer + 0x284U, kRollAction);
  GuestState state;
  state.r[15] = kRollDashStateEntry;
  const GuestState before = state;
  Require(!f.Runtime.Execute(kRollDashStateEntry, state, &f.Result, &f.Blocks) &&
              state.r == before.r && f.Runtime.DashQueries() == 0U,
          "the dash state ran with Roll dash off");
  Require(!f.Memory.IsMapped(kRollDashLinkExtBase, 1U) && !f.Memory.IsMapped(kRollDashBlendBase, 1U),
          "Roll dash off mapped its regions");
  Require(f.R16(kIdleObject + 0x60U) == 0x1B2U, "Roll dash off touched the idle word");
}

void RollCallStaysNativeOutsideADash() {
  // Turned on without a dash: the roll block runs natively (no exit).
  Fixture f;
  f.Runtime.Prepare(true);
  auto state = RollCall(kNmlDash, 1.5F);
  const GuestState before = state;
  Require(!f.Execute(kRollDashRollSite, state) && state.r == before.r && state.vfp == before.vfp,
          "the roll call left the native block outside a dash");
  Require(f.Runtime.DashBlends() == 0U && f.Runtime.DashSpeedApplications() == 0U,
          "a native roll call was counted");
  Require(!f.Runtime.AnimationsReady() && !f.Runtime.LinkExtError().empty(),
          "missing animations were not reported");
}

void DashWithoutAnimations() {
  // The dash runs on the native run animation: faster roll (1.425) and run.
  Fixture f;
  f.Runtime.Prepare(true);
  f.Update(true, false);
  Require(f.Runtime.DashFrames() == 0U, "running with A held started a dash");
  f.Update(true, true);
  Require(f.Runtime.DashQueries() == 2U && f.Runtime.DashFrames() == 1U, "the roll did not start the dash");
  Require(f.R16(kIdleObject + 0x60U) == 0x1C9U, "the dash did not swap the idle word");
  auto roll = RollCall(kNmlDash, 1.0F);
  Require(f.Execute(kRollDashRollSite, roll) && roll.r[15] == 0x002DD8B0U && roll.r[14] == 0x002BE8BCU &&
              roll.r[0] == kPlayer && roll.r[1] == kPlay && F(roll.vfp[0]) == 1.425F,
          "the roll animation was not scaled by 1.425 without the dash animations");
  // Out of the roll, still holding A: the run scale.
  f.Update(true, false);
  Require(f.Runtime.DashFrames() == 2U, "the dash did not continue out of the roll");
  GuestState move;
  move.r[4] = kPlayer;
  move.r[6] = kPlayer + 0x2000U;
  move.r[7] = kPlayer + 0x1000U;
  move.r[15] = kRollDashMoveSpeedSite;
  f.W32(move.r[6] + 0x21CU, B(4.0F));
  f.W16(move.r[7] + 0x20U, 0x4000U);
  Require(f.Execute(kRollDashMoveSpeedSite, move) && move.r[15] == 0x0035FB14U &&
              move.r[14] == 0x00251D4CU && move.r[0] == kPlayer && F(move.vfp[0]) == 4.0F * 1.425F,
          "the move speed site did not call Actor_UpdateVelocityXZGravity at the run scale");
  Require(F(f.R32(kPlayer + 0x6CU)) == 4.0F * 1.425F && f.R16(kPlayer + 0x36U) == 0x4000U,
          "the move speed site did not store the speed and the yaw");
  GuestState turn = move;
  turn.r[15] = kRollDashTurnSpeedSite;
  f.W16(kPlayer + 0x36U, 0x0000U);
  f.W16(turn.r[7] + 0x20U, 0x7000U); // 0x7000 away: the far branch
  Require(f.Execute(kRollDashTurnSpeedSite, turn) && turn.r[15] == 0x00251B64U &&
              turn.r[9] == 0x7000U && turn.r[0] == 0x7000U && F(turn.vfp[23]) == 4.0F * 1.425F &&
              (turn.cpsr & 0xF0000000U) == 0x20000000U,
          "the turn speed site did not take the far branch with the run scale");
  turn = move;
  turn.r[15] = kRollDashTurnSpeedSite;
  f.W16(turn.r[7] + 0x20U, 0x1000U);
  Require(f.Execute(kRollDashTurnSpeedSite, turn) && turn.r[15] == 0x00251B80U,
          "the turn speed site did not take the near branch");
  // In the air for more than two updates: the airborne scale.
  f.W16(kPlayer + 0x90U, 0U);
  f.Update(true, false);
  f.Update(true, false);
  f.Update(true, false);
  move.r[15] = kRollDashMoveSpeedSite;
  Require(f.Execute(kRollDashMoveSpeedSite, move) && F(move.vfp[0]) == 4.0F * 1.18F,
          "the airborne run scale was not applied");
  f.W16(kPlayer + 0x90U, 1U);
  // A released: the dash ends, the idle word goes back, the sites stay native.
  f.Update(false, false);
  Require(f.R16(kIdleObject + 0x60U) == 0x1B2U, "the end of the dash did not restore the idle word");
  Require(!f.Runtime.ShouldObserve(kRollDashMoveSpeedSite) && !f.Runtime.ShouldObserve(kRollDashNaviSite),
          "the dash sites stayed observed after the dash");
  // A blocked query (a cutscene) never starts a dash.
  f.Update(true, true, true);
  Require(f.Runtime.DashFrames() == 5U && !f.Runtime.ShouldObserve(kRollDashNaviSite),
          "a blocked query started a dash");
}

void NaviEntersTheHatWhenHerReachTimerExpiresWhileDashing() {
  Fixture f;
  const auto naviCaller = [] {
    GuestState state;
    state.r[4] = kNavi + 0x900U;
    state.r[5] = kNavi;
    state.r[7] = kPlayer + 0x1000U;
    state.r[13] = kStack;
    state.r[15] = kRollDashNaviSite;
    return state;
  };
  auto state = naviCaller();
  Require(!f.Execute(kRollDashNaviSite, state), "Navi's return left the native block without Roll dash");
  f.Runtime.Prepare(true);
  Require(!f.Execute(kRollDashNaviSite, state), "Navi's return left the native block without a dash");
  f.Update(true, true);
  state = naviCaller();
  Require(f.Execute(kRollDashNaviSite, state) && state.r[15] == 0x00129680U && state.r[4] == kNavi + 0x900U &&
              state.r[5] == kNavi && state.r[7] == kPlayer + 0x1000U,
          "Navi did not go into the hat while dashing");
  Require(f.Runtime.DashNaviHides() == 1U, "the hat entry was not counted");
}

void DashAnimationsAnswerAndCrossFade() {
  Fixture f(WriteLinkExt());
  f.W8(kPlayer + 0x2C8U, 21U);                // limbs
  f.W32(kPlayer + 0x2CCU, kPlayer + 0x2800U); // joint table
  f.W32(kPlayer + 0x27CU, kPlayer + 0x2900U); // frame table
  f.W8(kPlayer + 0x2D5U, 3U);                 // animation flags
  f.W32(kPlayer + 0x2254U, B(14.5F));         // run frame
  f.Runtime.Prepare(true);
  Require(f.Runtime.AnimationsReady() && f.Runtime.LinkExtError().empty(),
          "the Link animations were not mapped");
  Require(f.R32(kRollDashLinkExtBase) == TopScreenLinkExtImage::kMagic &&
              f.Memory.IsWritable(kRollDashBlendBase, kRollDashBlendSize),
          "the animation image or the blend page is missing");
  const std::uint32_t slot = TopScreenLinkExtImage::SlotAddress(kRollDashLinkExtBase, 0U, 1U);
  // Not dashing: the CSAB query is native.
  f.Update(true, false);
  GuestState query;
  query.r[0] = kArchive;
  query.r[1] = kNmlDash;
  query.r[14] = 0x002F0000U;
  query.r[15] = kRollDashCsabQuery;
  Require(!f.Execute(kRollDashCsabQuery, query), "the CSAB query was answered outside a dash");

  f.Update(true, true);
  Require(f.Execute(kRollDashCsabQuery, query) && query.r[0] == slot && query.r[3] == slot &&
              query.r[15] == 0x002F0000U && f.Runtime.LinkExtAnswers() == 1U,
          "the dash animation was not answered from its slot");
  Require(f.R32(slot) >= kRollDashLinkExtBase + TopScreenLinkExtImage::kData,
          "the slot does not hold the CSAB pointer");
  // Another animation, an animation the archive lacks, another archive: native.
  GuestState other = query;
  other.r[1] = 0x100U;
  Require(!f.Execute(kRollDashCsabQuery, other), "a native animation was answered");
  other.r[1] = 0x09BU; // nml_heavy_dash: not in this archive
  Require(!f.Execute(kRollDashCsabQuery, other), "a missing dash animation was answered");
  other = query;
  other.r[0] = kArchive + 0x1000U;
  other.r[1] = kNmlDash;
  Require(!f.Execute(kRollDashCsabQuery, other), "another archive was answered");

  // The dash roll at the adult scale (0.8) on consecutive frames: no fade.
  f.W32(kPlay + 0x5BF4U, 100U);
  auto roll = RollCall(kNmlDash, 1.0F);
  Require(f.Execute(kRollDashRollSite, roll) && F(roll.vfp[0]) == 0.8F && roll.r[14] == 0x002BE8BCU,
          "the dash roll was not scaled by 0.8");
  // A released between two frames of the same animation: the answer switches
  // and Link's pose fades from the dash animation.
  f.Update(false, false);
  f.W32(kPlay + 0x5BF4U, 101U);
  roll = RollCall(kNmlDash, 1.0F);
  const GuestState caller = roll;
  Require(f.Execute(kRollDashRollSite, roll) && roll.r[15] == 0x002DD8B0U &&
              roll.r[14] == kRollDashRollSite && f.Runtime.DashBlends() == 1U && f.Runtime.Pending(),
          "the switch did not start a fade returning to the site");
  // Weight 5/6 after one 30 Hz update, ratio clamped to 0.8 / 1.0.
  Require(std::abs(F(roll.vfp[0]) - (1.0F + (0.8F - 1.0F) * (5.0F / 6.0F))) < 1.0e-6F,
          "the fade did not ease the speed");
  // The animation call returns: AnimationContext_AddEntry.
  GuestState step = caller;
  step.r[0] = 0xDEADU;
  Require(f.Execute(kRollDashRollSite, step) && step.r[15] == 0x00324154U &&
              step.r[0] == kPlay + 0x3410U && step.r[1] == kPlayer + 0x254U && step.r[14] == kRollDashRollSite,
          "the fade did not add an animation context entry");
  // It returns the entry: decode the dash animation's frame into the page.
  step = caller;
  step.r[0] = 0x01005000U;
  Require(f.Execute(kRollDashRollSite, step) && step.r[15] == 0x002BD9ECU && step.r[0] == 0x01005000U &&
              step.r[2] == slot && step.r[3] == 21U && step.r[13] == kStack - 0x20U &&
              std::abs(F(step.vfp[0]) - 15.0F) < 1.0e-4F && f.R32(kStack - 0x20U) == kRollDashBlendBase &&
              f.R32(kStack - 0x1CU) == 3U && f.R32(kStack - 0x18U) == 0U &&
              step.r[1] == kPlayer + 0x2900U,
          "the fade did not decode the dash frame into the blend page");
  // The decode returns with its argument frame: another context entry.
  step = caller;
  step.r[13] = kStack - 0x20U;
  Require(f.Execute(kRollDashRollSite, step) && step.r[15] == 0x00324154U && step.r[13] == kStack,
          "the fade did not add the interpolation entry");
  // Then AnimationEntry_InitInterpAll by the weight, back to the native continuation.
  step = caller;
  step.r[0] = 0x01005100U;
  Require(f.Execute(kRollDashRollSite, step) && step.r[15] == 0x002C3814U && step.r[14] == 0x002BE8BCU &&
              step.r[1] == 21U && step.r[2] == kPlayer + 0x2800U && step.r[3] == kRollDashBlendBase &&
              std::abs(F(step.vfp[0]) - 5.0F / 6.0F) < 1.0e-6F,
          "the fade did not blend the joints toward the dash pose");
  Require(!f.Runtime.Pending() && f.Runtime.DashBlendPoses() == 1U, "the fade did not finish");
  // A re-entry from another stack is refused.
  f.W32(kPlay + 0x5BF4U, 102U);
  roll = RollCall(kNmlDash, 1.0F);
  Require(f.Execute(kRollDashRollSite, roll) && f.Runtime.Pending(), "the fade did not continue");
  step = caller;
  step.r[13] = kStack - 0x100U;
  bool refused = false;
  try {
    f.Execute(kRollDashRollSite, step);
  } catch (const std::runtime_error &) {
    refused = true;
  }
  Require(refused, "a mismatched re-entry was accepted");
}

void ResetAfterAStateLoad() {
  // A state saved before the regions existed: the load drops them; Reset and
  // the next Prepare map them again, and no native call is still pending.
  Fixture f(WriteLinkExt());
  const auto withoutRegions = f.Memory.CaptureState();
  f.Runtime.Prepare(true);
  f.W32(kPlay + 0x14U, 1U);
  GuestState entry;
  entry.r[15] = kRollDashStateEntry;
  Require(f.Execute(kRollDashStateEntry, entry) && f.Runtime.Pending(), "the query did not start");
  std::string error;
  Require(f.Memory.RestoreState(withoutRegions, &error), "cannot load the state");
  Require(!f.Memory.IsMapped(kRollDashLinkExtBase, 1U) && !f.Memory.IsMapped(kRollDashBlendBase, 1U),
          "the loaded state kept the regions");
  f.Runtime.Reset();
  Require(!f.Runtime.Pending() && !f.Runtime.DashQueryPending() && !f.Runtime.AnimationsReady(),
          "Reset kept host state");
  f.Runtime.Prepare(true);
  Require(f.Runtime.AnimationsReady() && f.R32(kRollDashLinkExtBase) == TopScreenLinkExtImage::kMagic &&
              f.Memory.IsWritable(kRollDashBlendBase, kRollDashBlendSize),
          "Prepare did not map the regions again");
  // Another image in the region (a state saved with other animations) is refreshed.
  f.W32(kRollDashLinkExtBase + 4U, 0x12345678U);
  f.Runtime.Reset();
  f.Runtime.Prepare(true);
  Require(f.Runtime.AnimationsReady() && f.R32(kRollDashLinkExtBase + 4U) != 0x12345678U,
          "Prepare did not refresh a foreign image");
}

void SettingsAndPerfLines() {
  Fixture f(WriteLinkExt());
  f.Runtime.Prepare(false);
  auto line = f.Runtime.TakeSettingsLine(true);
  Require(line.has_value() && line->find("landing_roll=on roll_dash=off animations=-") != std::string::npos,
          "the first settings line is missing");
  Require(!f.Runtime.TakeSettingsLine(true).has_value(), "an unchanged setting was logged again");
  Require(f.Runtime.TakePerfLine().empty(), "a perf line was written with Roll dash off");
  f.Runtime.Prepare(true);
  line = f.Runtime.TakeSettingsLine(false);
  Require(line.has_value() && line->find("landing_roll=off roll_dash=on animations=ready") != std::string::npos,
          "the change was not logged");
  f.Update(true, true);
  const std::string perf = f.Runtime.TakePerfLine();
  Require(perf.starts_with("roll_dash: queries=1 frames=1 ") && perf.ends_with("animations=ready\n"),
          "the perf line does not count the window");
  Require(f.Runtime.TakePerfLine().starts_with("roll_dash: queries=0 frames=0 "),
          "the perf window did not restart");
}

void ConfigFile() {
  const auto directory = std::filesystem::temp_directory_path() / "oot3d_roll_dash_config_tests";
  std::filesystem::remove_all(directory);
  Require(!ReadRollDashEnabled(directory), "Roll dash is on without its file");
  Require(SaveRollDashEnabled(directory, true) && ReadRollDashEnabled(directory) &&
              std::filesystem::exists(directory / "roll_dash"),
          "turning Roll dash on did not write config/roll_dash");
  Require(SaveRollDashEnabled(directory, false) && !ReadRollDashEnabled(directory),
          "turning Roll dash off did not remove config/roll_dash");
  Require(!RollDashAnimationsInstalled(directory), "missing animations reported installed");
  std::ofstream(directory / "zelda_link_ext.zar") << "x";
  Require(RollDashAnimationsInstalled(directory), "installed animations not found");
  Require(!RollDashEnabled().load(), "Roll dash is on by default");
  std::filesystem::remove_all(directory);
}
} // namespace

int main() {
  OffObservesNothing();
  RollCallStaysNativeOutsideADash();
  DashWithoutAnimations();
  NaviEntersTheHatWhenHerReachTimerExpiresWhileDashing();
  DashAnimationsAnswerAndCrossFade();
  ResetAfterAStateLoad();
  SettingsAndPerfLines();
  ConfigFile();
  std::filesystem::remove_all(std::filesystem::temp_directory_path() / "oot3d_roll_dash_runtime_tests");
  std::cout << "Roll dash runtime tests passed\n";
}
