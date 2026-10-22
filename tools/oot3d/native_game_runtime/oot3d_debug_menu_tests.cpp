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
    // Segurar L3+R3 abre uma vez só (não pisca a cada quadro).
    DebugMenuCombo combo;
    Require(!combo.Update(false), "solto");
    Require(combo.Update(true), "apertou");
    for (int frame = 0; frame < 30; ++frame) Require(!combo.Update(true), "segurando");
    Require(!combo.Update(false), "soltou");
    Require(combo.Update(true), "apertou de novo");
  }
  {
    // Abre e fecha.
    Require(!DebugMenuOpen(), "começa fechado");
    ToggleDebugMenu();
    Require(DebugMenuOpen(), "abriu");
    ToggleDebugMenu();
    Require(!DebugMenuOpen(), "fechou");
  }
  std::puts("debug menu: PASS");
  return 0;
}
