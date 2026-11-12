// Switch six-axis -> 3DS motion (oot3d_switch_motion.h): the axis conversion
// against known rotations and the existing SDL path, the averaging of the
// six-axis LIFO into one observation per poll, and the game log's motion line.
#include "oot3d_switch_motion.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using namespace Oot3dNativeGame;
using ThreeDsRecomp::Input::MotionObservation;
using Vector = std::array<float, 3>;

void Require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "oot3d_switch_motion_tests: %s\n", message);
    std::exit(1);
  }
}

bool Near(float a, float b, float tolerance = 1e-3F) { return std::abs(a - b) <= tolerance; }

bool Near(const Vector& a, const Vector& b, float tolerance = 1e-3F) {
  return Near(a[0], b[0], tolerance) && Near(a[1], b[1], tolerance) && Near(a[2], b[2], tolerance);
}

Vector Cross(const Vector& a, const Vector& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

float Dot(const Vector& a, const Vector& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

Vector Scale(const Vector& a, float s) { return {a[0] * s, a[1] * s, a[2] * s}; }

// The Switch frame: +X right, +Y toward the top edge, +Z out of the screen.
// Gravity as the sensor reports it, for a few ways of holding the console.
constexpr Vector kFaceUp{0.0F, 0.0F, -1.0F};
constexpr Vector kUpright{0.0F, -1.0F, 0.0F};  // screen facing the player
const Vector kTilted{0.0F, -std::sqrt(0.5F), -std::sqrt(0.5F)};  // halfway

void TestRestPoses() {
  const auto flat = ConvertSwitchSixAxis(kFaceUp, {});
  Require(flat.AccelerometerValid && flat.GyroscopeValid, "conversion did not mark both sensors valid");
  // The fork's neutral pose (MappingConfig::AccelerometerNeutral) is a 3DS
  // lying face up.
  Require(Near(flat.Accelerometer, {0.0F, -1.0F, 0.0F}), "face up did not read the 3DS neutral pose");
  const auto upright = ConvertSwitchSixAxis(kUpright, {});
  Require(Near(upright.Accelerometer, {0.0F, 0.0F, -1.0F}), "upright did not read gravity along -Z");
  const auto right = ConvertSwitchSixAxis({1.0F, 0.0F, 0.0F}, {});
  Require(Near(right.Accelerometer, {-1.0F, 0.0F, 0.0F}), "Switch +X did not map to 3DS -X");
  Require(Near(ConvertSwitchSixAxis({}, {1.0F, 0.0F, 0.0F}).GyroscopeDegreesPerSecond, {-360.0F, 0.0F, 0.0F}),
          "one revolution per second is not 360 deg/s");
}

void TestMatchesSdlPath() {
  // libnx -> SDL (yuzu, Ryujinx): accel_sdl = g * (-a.x, -a.z, a.y),
  // gyro_sdl = 2pi * (w.x, w.z, -w.y); then the fork's ConvertSdlMotion.
  constexpr float kGravity = 9.80665F;
  constexpr float kTwoPi = 6.28318530717958647692F;
  const std::array<std::pair<Vector, Vector>, 4> cases{{
      {{0.1F, -0.2F, -0.97F}, {0.3F, -0.05F, 0.12F}},
      {kUpright, {0.0F, 0.25F, 0.0F}},
      {{0.5F, 0.5F, -0.7F}, {-0.4F, 0.6F, -0.2F}},
      {{-0.9F, 0.1F, 0.4F}, {0.01F, -0.02F, 0.9F}},
  }};
  for (const auto& [a, w] : cases) {
    const auto direct = ConvertSwitchSixAxis(a, w);
    const auto viaSdl = ThreeDsRecomp::Input::ConvertSdlMotion(
        {-a[0] * kGravity, -a[2] * kGravity, a[1] * kGravity}, true,
        {w[0] * kTwoPi, w[2] * kTwoPi, -w[1] * kTwoPi}, true);
    Require(Near(direct.Accelerometer, viaSdl.Accelerometer), "accelerometer differs from the SDL path");
    Require(Near(direct.GyroscopeDegreesPerSecond, viaSdl.GyroscopeDegreesPerSecond, 1e-2F),
            "gyroscope differs from the SDL path");
  }
}

void TestRotationIsCoherent() {
  // A world-fixed vector seen from a body turning at w changes as -w x v.
  // The accelerometer and gyroscope must be mapped by the same proper
  // rotation, or the guest's fusion sees gravity move against the gyro.
  const std::array<Vector, 3> rates{{{0.2F, 0.0F, 0.0F}, {0.0F, -0.3F, 0.1F}, {0.05F, 0.4F, -0.2F}}};
  for (const auto& gravity : {kFaceUp, kUpright, kTilted}) {
    for (const auto& w : rates) {
      const auto converted = ConvertSwitchSixAxis(gravity, w);
      const auto changeInSwitch = Cross(w, gravity);
      const auto expected = ConvertSwitchSixAxis(changeInSwitch, {}).Accelerometer;
      const auto actual = Cross(Scale(converted.GyroscopeDegreesPerSecond, 1.0F / 360.0F), converted.Accelerometer);
      Require(Near(expected, actual), "gyroscope and accelerometer are not the same rotation");
    }
  }
}

ThreeDsRecomp::Input::HidState Resolve(ThreeDsRecomp::Input::MotionSource source, const MotionObservation& motion,
                                       std::int32_t mouseX = 0, std::int32_t mouseY = 0, bool invertY = false) {
  ThreeDsRecomp::Input::MappingConfig config;
  config.NativeMotionSource = source;
  config.NativeMotionInvertY = invertY;
  ThreeDsRecomp::Input::PhysicalInputState input;
  input.ControllerMotion = motion;
  input.MouseDeltaX = mouseX;
  input.MouseDeltaY = mouseY;
  return ThreeDsRecomp::Input::ResolveInput(config, input, {}).Hid;
}

void TestAimFollowsTheConsole() {
  using ThreeDsRecomp::Input::MotionSource;
  // Pitching the back of the console (-Z) up is a positive turn about +X.
  // It must reach the guest with the sign of the mouse moving up.
  const auto tiltUp = ConvertSwitchSixAxis(kUpright, {0.1F, 0.0F, 0.0F});
  const float gyroUp = Resolve(MotionSource::ControllerGyroscope, tiltUp).GyroscopeDegreesPerSecond[0];
  const float mouseUp = Resolve(MotionSource::Mouse, {}, 0, -10).GyroscopeDegreesPerSecond[0];
  Require(gyroUp < 0.0F && mouseUp < 0.0F, "tilting the console up does not aim like the mouse moving up");
  Require(Near(gyroUp, -36.0F), "0.1 rev/s did not reach the guest as 36 deg/s");
  // invert_y (aim.invert_y in the config) flips it.
  Require(Resolve(MotionSource::ControllerGyroscope, tiltUp, 0, 0, true).GyroscopeDegreesPerSecond[0] > 0.0F,
          "invert Y did not flip console pitch");

  // Turning left while lying flat is a positive turn about the screen normal
  // (+Z); the mouse moving left yaws the same way at the virtual flat pose.
  const auto flatLeft = ConvertSwitchSixAxis(kFaceUp, {0.0F, 0.0F, 0.1F});
  const float gyroLeft = Resolve(MotionSource::ControllerGyroscope, flatLeft).GyroscopeDegreesPerSecond[1];
  const float mouseLeft = Resolve(MotionSource::Mouse, {}, -10, 0).GyroscopeDegreesPerSecond[1];
  Require(gyroLeft > 0.0F && mouseLeft > 0.0F, "turning left does not aim like the mouse moving left");

  // Turning left about world up, however the console is held, turns the 3DS
  // body left about its own world up (minus gravity).
  for (const auto& gravity : {kFaceUp, kUpright, kTilted}) {
    const auto left = ConvertSwitchSixAxis(gravity, Scale(gravity, -0.1F));
    Require(Dot(left.GyroscopeDegreesPerSecond, Scale(left.Accelerometer, -1.0F)) > 0.0F,
            "turning left about world up did not reach the 3DS as a left turn");
  }
  // Upright, that turn is about the 3DS's long axis (Z), as the fork's own
  // virtual motion composes yaw when pitched (ResolveMotion).
  const auto uprightLeft = ConvertSwitchSixAxis(kUpright, {0.0F, 0.1F, 0.0F});
  Require(Near(uprightLeft.GyroscopeDegreesPerSecond, {0.0F, 0.0F, 36.0F}), "upright yaw is not about 3DS +Z");
}

SwitchSixAxisSample Sample(std::uint64_t number, Vector angularVelocity, std::uint64_t deltaNanoseconds = 5'000'000,
                           Vector acceleration = kFaceUp, bool connected = true) {
  return {number, deltaNanoseconds, acceleration, angularVelocity, connected};
}

void TestAccumulator() {
  SwitchMotionAccumulator accumulator;
  std::uint64_t now = 1'000'000;
  accumulator.Reset(now);
  // After a start the LIFO may hold the stopped sensor's state: position only.
  std::vector<SwitchSixAxisSample> lifo{Sample(10, {1.0F, 0.0F, 0.0F})};
  Require(accumulator.Observe(lifo, now) == 0, "first read after a start was averaged");
  Require(!accumulator.Take(false, now).GyroscopeValid, "motion reported before any new sample");

  // Newest first; 11 at 5 ms and 12 at 15 ms: time-weighted mean.
  now += 20'000;
  lifo = {Sample(12, {0.0F, 0.0F, 0.2F}, 15'000'000), Sample(11, {0.0F, 0.0F, 0.4F}), Sample(10, {1.0F, 0.0F, 0.0F})};
  Require(accumulator.Observe(lifo, now) == 2, "new samples were not taken exactly once");
  auto motion = accumulator.Take(false, now);
  Require(motion.GyroscopeValid && motion.AccelerometerValid, "averaged motion is not valid");
  Require(Near(motion.GyroscopeDegreesPerSecond[1], 360.0F * 0.25F), "mean is not weighted by delta time");
  Require(motion.GyroscopeTimestampMicroseconds == now && motion.AccelerometerTimestampMicroseconds == now,
          "timestamps do not follow the newest samples");
  const auto firstStamp = motion.GyroscopeTimestampMicroseconds;

  // Nothing new: same value and timestamp (calibration counts it once).
  now += 2'000;
  Require(accumulator.Observe(lifo, now) == 0, "already seen samples were averaged again");
  motion = accumulator.Take(true, now);
  Require(motion.GyroscopeTimestampMicroseconds == firstStamp && Near(motion.GyroscopeDegreesPerSecond[1], 90.0F),
          "a poll without new samples changed the observation");
  // Consumed: the next window holds only new samples; disconnected entries
  // and the default delta time.
  now += 10'000;
  lifo.insert(lifo.begin(), {Sample(14, {0.0F, 0.0F, 0.6F}, 0), Sample(13, {0.0F, 0.0F, 5.0F}, 5'000'000, kFaceUp, false)});
  Require(accumulator.Observe(lifo, now) == 1, "disconnected sample was averaged");
  motion = accumulator.Take(true, now);
  Require(Near(motion.GyroscopeDegreesPerSecond[1], 216.0F) && motion.GyroscopeTimestampMicroseconds > firstStamp,
          "consumed window leaked into the next one");

  // A stopped sensor leaves its last state behind: stale after 200 ms.
  now += SwitchMotionAccumulator::StaleMicroseconds + 1;
  Require(accumulator.Observe(lifo, now) == 0 && accumulator.Stale(now), "stalled sensor not detected");
  Require(!accumulator.Take(false, now).GyroscopeValid, "stale motion was reported as valid");

  // A counter that went backwards (sensor restarted) only resets the position.
  lifo = {Sample(3, {0.0F, 0.0F, 1.0F})};
  Require(accumulator.Observe(lifo, now) == 0, "restarted counter replayed old samples");
  now += 5'000;
  lifo.insert(lifo.begin(), Sample(4, {0.0F, 0.0F, 0.1F}));
  Require(accumulator.Observe(lifo, now) == 1 && Near(accumulator.Take(false, now).GyroscopeDegreesPerSecond[1], 36.0F),
          "samples after a counter restart were lost");
}

void TestCalibrationSeesFreshSamples() {
  // The fork's calibration skips observations whose timestamp did not move.
  SwitchMotionAccumulator accumulator;
  ThreeDsRecomp::Input::MotionCalibrationAccumulator calibration;
  std::uint64_t now = 5'000'000;
  accumulator.Reset(now);
  std::uint64_t number = 100;
  std::vector<SwitchSixAxisSample> lifo{Sample(number, {0.001F, 0.0F, 0.0F})};
  accumulator.Observe(lifo, now);
  bool done = false;
  for (int poll = 0; poll < 200 && !done; ++poll) {
    now += 16'000;
    if (poll % 2 == 0) {  // every other poll brings new samples
      lifo = {Sample(number + 3, {0.001F, 0.0F, 0.0F}), Sample(number + 2, {0.001F, 0.0F, 0.0F}),
              Sample(number + 1, {0.001F, 0.0F, 0.0F})};
      number += 3;
    }
    accumulator.Observe(lifo, now);
    done = calibration.Observe(accumulator.Take(true, now));
    Require(calibration.SamplesCollected() <= static_cast<std::uint32_t>(poll / 2 + 1),
            "calibration counted a repeated observation");
  }
  Require(done, "calibration never completed from the Switch sensor");
  const auto mean = calibration.Mean();
  Require(Near(mean.GyroscopeDegreesPerSecond[0], -0.36F) && Near(mean.Accelerometer, {0.0F, -1.0F, 0.0F}),
          "calibration mean is not the converted bias and pose");
}

bool Contains(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

void TestMotionLog() {
  SwitchMotionLog log;
  std::uint64_t now = 10'000'000;
  SwitchMotionLogSample sample;
  sample.Wanted = true;
  sample.ControllerInstance = 0;
  sample.Available = SwitchMotionSource::Handheld;
  sample.Running = SwitchMotionSource::Handheld;
  auto line = log.Observe(sample, now);
  Require(line && Contains(*line, "motion: reason=start pad=0 wanted=1 available=handheld sensor=handheld start_rc=0x0"),
          "the first poll did not write the state line");

  // A minute of polls at 60 Hz: one line, at the minute, with the counts.
  sample.SamplesAdded = 3;
  sample.GyroscopeValid = true;
  sample.PhysicalAim = true;
  sample.GuestGyroscopeEnabled = false;
  int lines = 0;
  for (int poll = 0; poll < 3600; ++poll) {
    now += 16'667;
    sample.GuestGyroscopeSamples += 1;
    if (poll % 2 == 0) sample.GuestNonzeroGyroscopeSamples += 1;
    if ((line = log.Observe(sample, now))) {
      ++lines;
      Require(Contains(*line, "reason=interval") && Contains(*line, " polls=3600 wanted_polls=3600 samples=10800") &&
                  Contains(*line, " valid=3600 restarts=0 physical_aim=3600 guest_gyro=off guest_ring=3600") &&
                  Contains(*line, " guest_nonzero=1800"),
              "the minute line does not count the minute's polls");
    }
  }
  Require(lines == 1, "a minute of steady polls did not give exactly one line");

  // The game enables its gyro: a change line, but not within a second of
  // the previous line, so a state flipping every poll stays at one a second.
  sample.GuestGyroscopeEnabled = true;
  now += 16'667;
  Require(!log.Observe(sample, now), "a change within a second of the previous line was written at once");
  now += 1'000'000;
  line = log.Observe(sample, now);
  Require(line && Contains(*line, "reason=change") && Contains(*line, "guest_gyro=on"),
          "the game's gyro enable did not write a change line");
  sample.GuestGyroscopeEnabled = false;
  int changes = 0;
  for (int poll = 0; poll < 120; ++poll) {
    now += 16'667;
    sample.GuestGyroscopeEnabled = !sample.GuestGyroscopeEnabled;
    changes += log.Observe(sample, now) ? 1 : 0;
  }
  Require(changes <= 2, "a state flipping every poll wrote more than one line a second");

  // Not wanted and no guest gyro samples: the state line, then nothing more.
  sample = {};
  sample.ControllerInstance = 1;
  sample.Available = SwitchMotionSource::Handheld;
  now += 2'000'000;
  line = log.Observe(sample, now);
  Require(line && Contains(*line, "reason=change pad=1 wanted=0 available=handheld sensor=none"),
          "selecting another pad did not write a change line");
  for (int poll = 0; poll < 3 * 3600; ++poll) {
    now += 16'667;
    Require(!log.Observe(sample, now), "a quiet minute wrote a line");
  }

  // A failed start shows its result.
  sample.Wanted = true;
  sample.ControllerInstance = 0;
  sample.Running = SwitchMotionSource::Handheld;
  sample.StartResult = 0x1234;
  now += 2'000'000;
  line = log.Observe(sample, now);
  Require(line && Contains(*line, "sensor=handheld start_rc=0x1234"), "a failed start did not show its result");
}

} // namespace

int main() {
  TestRestPoses();
  TestMatchesSdlPath();
  TestRotationIsCoherent();
  TestAimFollowsTheConsole();
  TestAccumulator();
  TestCalibrationSeesFreshSamples();
  TestMotionLog();
  std::puts("oot3d_switch_motion_tests: ok");
  return 0;
}
