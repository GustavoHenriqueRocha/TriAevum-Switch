// What started the game (oot3d_switch_launch_environment.h).
#include "oot3d_switch_launch_environment.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void Expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << "FAILED: " << message << '\n';
    ++gFailures;
  }
}

using Oot3dNativeGame::DropRedundantLaunchArguments;
using Oot3dNativeGame::FirstLine;
using Oot3dNativeGame::FormatSwitchLaunchEnvironment;
using Oot3dNativeGame::FormatSwitchLaunchLine;
using Oot3dNativeGame::SwitchLaunchEnvironment;

void Arguments() {
  const std::string nro = "sdmc:/switch/oot3dre/oot3d_native_game.nro";
  Expect(DropRedundantLaunchArguments({nro}).size() == 1U, "the NRO path alone stays");
  Expect(DropRedundantLaunchArguments({nro, nro}).size() == 1U, "the path repeated is dropped");
  Expect(DropRedundantLaunchArguments({nro, '"' + nro + '"'}).size() == 1U, "the path repeated in quotes is dropped");
  Expect(DropRedundantLaunchArguments({'"' + nro + '"', nro}).size() == 1U, "a quoted first path matches");
  Expect(DropRedundantLaunchArguments({nro, "", "\"\""}).size() == 1U, "empty arguments are dropped");
  const auto kept = DropRedundantLaunchArguments({nro, "--renderer", "deko3d", nro});
  Expect(kept.size() == 3U && kept[1] == "--renderer" && kept[2] == "deko3d",
         "a real command line keeps its arguments, without the repeated path");
  Expect(DropRedundantLaunchArguments({}).empty(), "no arguments stay none");
}

void Format() {
  SwitchLaunchEnvironment environment;
  environment.Loader = FirstLine("nx-hbloader v2.4.5\nsecond line");
  environment.AppletType = 0;
  environment.ProgramId = 0x010E3D7237302000ULL;
  environment.HeapBytes = 3000ULL << 20U;
  environment.UsedMemory = 130ULL << 20U;
  environment.TotalMemory = 3285ULL << 20U;
  environment.Arguments = {"sdmc:/switch/oot3dre/oot3d_native_game.nro"};
  const auto text = FormatSwitchLaunchEnvironment(environment);
  Expect(text ==
             "loader=nx-hbloader v2.4.5\n"
             "applet_type=0\n"
             "program_id=0x010E3D7237302000\n"
             "heap_mib=3000\n"
             "memory_mib=130/3285\n"
             "argc=1\n"
             "argv0=sdmc:/switch/oot3dre/oot3d_native_game.nro\n",
         "boot-status lines");
  Expect(FormatSwitchLaunchLine(environment) ==
             "launch: loader=nx-hbloader v2.4.5 applet_type=0 program_id=0x010E3D7237302000 heap_mib=3000 "
             "memory_mib=130/3285 argc=1 argv0=sdmc:/switch/oot3dre/oot3d_native_game.nro",
         "game log line");
  Expect(FormatSwitchLaunchEnvironment({}).rfind("loader=-\napplet_type=-1\n", 0) == 0,
         "no loader information is written as -");
}

} // namespace

int main() {
  Arguments();
  Format();
  if (gFailures != 0) return EXIT_FAILURE;
  std::cout << "launch environment tests passed\n";
  return EXIT_SUCCESS;
}
