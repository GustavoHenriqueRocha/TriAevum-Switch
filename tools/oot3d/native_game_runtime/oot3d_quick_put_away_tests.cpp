// Quick put away (oot3d_quick_put_away.h): the config file and the default.
#include "oot3d_quick_put_away.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace {

using namespace Oot3dNativeGame;

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

void TestConfig() {
  Require(!QuickPutAwayEnabled().load(), "off by default");
  Require(kQuickPutAwayFrames < 30U && kQuickPutAwayHookPc < kQuickPutAwayStorePc, "one frame, then the store");
  const auto config = std::filesystem::temp_directory_path() / "oot3d_quick_put_away_tests";
  std::error_code ignored;
  std::filesystem::remove_all(config, ignored);
  Require(!ReadQuickPutAwayEnabled(config), "no file: off");
  Require(SaveQuickPutAwayEnabled(config, true) && ReadQuickPutAwayEnabled(config), "on writes the file");
  Require(SaveQuickPutAwayEnabled(config, false) && !ReadQuickPutAwayEnabled(config), "off removes it");
  Require(SaveQuickPutAwayEnabled(config, false), "off twice");
  std::filesystem::remove_all(config, ignored);
}

} // namespace

int main() {
  TestConfig();
  std::puts("oot3d_quick_put_away_tests: ok");
  return 0;
}
