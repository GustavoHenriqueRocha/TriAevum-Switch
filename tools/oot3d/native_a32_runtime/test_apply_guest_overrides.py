from __future__ import annotations

import unittest

import apply_guest_overrides as overrides
from whole_aot_cpp import GIANT_COLD_ATTRIBUTE


SHARD = """#include "oot3d_whole_aot_generated_internal.h"
#include "oot3d_native_a32_memory.h"

#include <atomic>

namespace Oot3dNativeGame::GeneratedWholeAot {
{attribute}Oot3dWholeAotFlow Execute_Giant_00002000(
    Oot3dWholeAotFrame& frame, Oot3dWholeAotContext& context,
    Oot3dAotArchitecturalState& state,
    uint32_t entryPc) {
    switch (entryPc) {
    }
block_00002000:
    {
    }
block_00002010:
    {
    }
block_00002020:
    {
    }
}
} // namespace Oot3dNativeGame::GeneratedWholeAot
"""


class ApplyGuestOverridesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.saved = (dict(overrides.BLOCK_OVERRIDES), set(overrides.INLINE_OVERRIDES))

    def tearDown(self) -> None:
        overrides.BLOCK_OVERRIDES.clear()
        overrides.BLOCK_OVERRIDES.update(self.saved[0])
        overrides.INLINE_OVERRIDES.clear()
        overrides.INLINE_OVERRIDES.update(self.saved[1])

    def test_finds_giant_cold_functions(self) -> None:
        # A generation without a hot list (the PGO=generate build) marks giant
        # functions cold on their signature line: their overrides must still
        # be wired in.
        overrides.BLOCK_OVERRIDES[0x2010] = (0x2000, (0x2020,))
        overrides.INLINE_OVERRIDES.update({0x2010, 0x2000})
        for attribute in ("", GIANT_COLD_ATTRIBUTE):
            for entry, marker in ((0x2010, "block_00002010: /*guest-override-inline*/"),
                                  (0x2000, "GuestOverrides::Override_00002000(")):
                with self.subTest(attribute=bool(attribute), entry=entry):
                    text, patched = overrides.patch_text(
                        SHARD.replace("{attribute}", attribute), [entry])
                    self.assertEqual(patched, [entry])
                    self.assertIn(marker, text)
                    self.assertEqual(len(text.split("\n")), len(SHARD.split("\n")))


if __name__ == "__main__":
    unittest.main()
