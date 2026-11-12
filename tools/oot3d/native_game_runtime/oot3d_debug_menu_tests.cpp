#include "fast/oot3d/debug_menu.h"

#include <cstdio>
#include <cstdlib>

namespace {

void Require(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

} // namespace

int main() {
  using namespace Fast::Oot3d;
  {
    // Holding L3+R3 opens it only once (doesn't flicker every frame).
    DebugMenuCombo combo;
    Require(!combo.Update(false), "released");
    Require(combo.Update(true), "pressed");
    for (int frame = 0; frame < 30; ++frame) Require(!combo.Update(true), "holding");
    Require(!combo.Update(false), "let go");
    Require(combo.Update(true), "pressed again");
  }
  {
    // Opens and closes.
    Require(!DebugMenuOpen(), "starts closed");
    ToggleDebugMenu();
    Require(DebugMenuOpen(), "opened");
    ToggleDebugMenu();
    Require(!DebugMenuOpen(), "closed");
  }
  std::puts("debug menu: PASS");
  return 0;
}
