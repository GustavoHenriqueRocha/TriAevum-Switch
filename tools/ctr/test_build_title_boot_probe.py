"""Teste do teto de temperatura do build_title_boot_probe.sh sem lm-sensors.

Regressão de 23/11: no WSL do Ryzen não há `sensors`; com `set -euo pipefail`
o `T=$(temp)` do laço de build saía com 127 e o probe nem compilava. Sem
sensores o script tem de seguir sem teto (temperatura vazia, tratada como 0).
"""
from __future__ import annotations

import re
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "build_title_boot_probe.sh"


def temp_function() -> str:
    match = re.search(r"^temp\(\) \{.*\}$", SCRIPT.read_text(), re.MULTILINE)
    assert match, "temp() não encontrada no script"
    return match.group(0)


class TempWithoutSensorsTests(unittest.TestCase):
    def run_temp(self, path: str) -> subprocess.CompletedProcess[str]:
        body = f"set -euo pipefail\n{temp_function()}\nT=$(temp); T=${{T:-0}}; echo \"T=$T\"\n"
        return subprocess.run(["/bin/bash", "-c", body], capture_output=True, text=True,
                              env={"PATH": path})

    def test_missing_sensors_gives_no_ceiling(self) -> None:
        with tempfile.TemporaryDirectory() as empty:
            # Só o awk do sistema no PATH, sem sensors.
            Path(empty, "awk").symlink_to("/usr/bin/awk")
            result = self.run_temp(empty)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "T=0")

    def test_fake_sensors_reading_is_parsed(self) -> None:
        with tempfile.TemporaryDirectory() as bindir:
            Path(bindir, "awk").symlink_to("/usr/bin/awk")
            fake = Path(bindir, "sensors")
            fake.write_text("#!/bin/sh\necho 'Tctl:         +71.5°C'\n")
            fake.chmod(0o755)
            result = self.run_temp(bindir)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "T=71")


if __name__ == "__main__":
    unittest.main()
