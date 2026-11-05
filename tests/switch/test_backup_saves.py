#!/usr/bin/env python3
"""Regressão: backup automático dos saves do Switch (scripts/switch/backup-saves.sh).

Pedido (05/11/2026): "melhora o sistema de saves pra não perder nada". O save do
OoT3D só existia no SD do Switch (sdmc:/switch/oot3dre/savedata) e numa cópia feita
à mão. O script copia os saves para o PC só quando algo mudou (SHA-256 contra o
último snapshot), nunca apaga snapshot e, só com --remote <host> explícito, manda
para o VPS conferindo o SHA-256 do outro lado.

Dados sintéticos: um "SD" falso montado em pasta temporária e lido com --source; o
VPS é um ssh falso que roda o comando localmente. Todo teste roda com um ssh-armadilha
no PATH (registra a chamada e falha), então nenhum teste chega ao VPS de verdade.
"""
from __future__ import annotations

import os
import re
import shutil
import socket
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/switch/backup-saves.sh"
SNAPSHOT_RE = re.compile(r"^\d{8}-\d{6}(-\d+)?$")

SAVE = bytes(range(256)) * 20 + bytes(220)  # 5340 bytes, como save0N.bin
SYSTEM = b"S" * 34


def build_sd(root: Path) -> None:
    """SD falso com o layout real de sdmc:/switch/oot3dre (e o que não deve ir)."""
    title = root / "switch/oot3dre"
    (title / "savedata").mkdir(parents=True)
    for name in ("save00.bin", "save03.bin", "save04.bin"):
        (title / "savedata" / name).write_bytes(SAVE)
    (title / "savedata/system.dat").write_bytes(SYSTEM)
    (title / "config").mkdir()
    (title / "config/fps60_mode").write_bytes(b"1\n")
    (title / "config/resolution_scale").write_bytes(b"1.5\n")
    (title / "config/parallel_presentation").write_bytes(b"")  # flag vazia também conta
    (title / "game_language.json").write_text('{"selected": "es"}\n')
    (title / "imgui.ini").write_text("[Window][Main Game]\n")
    # Não é save: não pode entrar (grande, muda a cada boot ou é derivado).
    (title / "oot3d_native_game.nro").write_bytes(b"NRO0" * 64)
    (title / "boot-status.txt").write_text("stage=runtime_running\n")
    (title / "developer-tools.json.tmp").write_text("{}")
    (title / "logs").mkdir()
    (title / "logs/perf.txt").write_text("host_frames=1\n")
    (title / "game").mkdir()
    (title / "game/code.bin").write_bytes(b"\0" * 64)


class BackupSavesTest(unittest.TestCase):
    def setUp(self) -> None:
        self.scratch = Path(tempfile.mkdtemp(prefix="backup-saves-test-"))
        self.addCleanup(self.cleanup)
        self.sd = self.scratch / "sd"
        self.dest = self.scratch / "dest"
        build_sd(self.sd)
        # ssh-armadilha: qualquer chamada fica registrada e falha (nunca chega ao VPS real).
        self.trap_bin = self.scratch / "trap-bin"
        self.trap_bin.mkdir()
        self.ssh_log = self.scratch / "ssh-chamado.log"
        (self.trap_bin / "ssh").write_text(f'#!/bin/sh\necho "$*" >> "{self.ssh_log}"\nexit 255\n')
        (self.trap_bin / "ssh").chmod(0o755)

    def cleanup(self) -> None:
        for path in self.scratch.rglob("*"):
            if not path.is_symlink():
                path.chmod(0o755 if path.is_dir() else 0o644)
        shutil.rmtree(self.scratch)

    def run_backup(self, *extra: str, env: dict[str, str] | None = None,
                   source: bool = True) -> subprocess.CompletedProcess[str]:
        args = [str(SCRIPT), "--dest", str(self.dest)]
        if source:
            args += ["--source", str(self.sd)]
        args += list(extra)
        if env is None:
            env = dict(os.environ, PATH=f"{self.trap_bin}:{os.environ['PATH']}")
        return subprocess.run(args, capture_output=True, text=True, timeout=120, env=env)

    def snapshots(self) -> list[Path]:
        if not self.dest.is_dir():
            return []
        return sorted(p for p in self.dest.iterdir() if p.is_dir() and SNAPSHOT_RE.match(p.name))

    def files_of(self, snapshot: Path) -> dict[str, bytes]:
        skip = {"SHA256SUMS", "manifest.txt"}
        return {str(p.relative_to(snapshot)): p.read_bytes()
                for p in snapshot.rglob("*") if p.is_file() and p.name not in skip}

    def tree_state(self) -> dict[str, bytes]:
        return {str(p.relative_to(self.dest)): p.read_bytes()
                for p in self.dest.rglob("*") if p.is_file()}

    def assert_ok(self, result: subprocess.CompletedProcess[str]) -> None:
        self.assertEqual(result.returncode, 0, f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")

    def test_first_run_creates_snapshot_with_saves_and_config_only(self) -> None:
        result = self.run_backup()
        self.assert_ok(result)
        snaps = self.snapshots()
        self.assertEqual(len(snaps), 1, result.stdout + result.stderr)
        files = self.files_of(snaps[0])
        base = "switch/oot3dre/"
        self.assertEqual(files[base + "savedata/save03.bin"], SAVE)
        self.assertEqual(files[base + "savedata/system.dat"], SYSTEM)
        self.assertEqual(files[base + "config/fps60_mode"], b"1\n")
        self.assertEqual(files[base + "config/parallel_presentation"], b"")
        self.assertIn(base + "game_language.json", files)
        self.assertIn(base + "imgui.ini", files)
        for excluded in ("oot3d_native_game.nro", "boot-status.txt", "logs/perf.txt",
                         "game/code.bin", "developer-tools.json.tmp"):
            self.assertNotIn(base + excluded, files)
        self.assertTrue((snaps[0] / "manifest.txt").is_file())
        # Nada de pasta temporária deixada para trás.
        leftovers = [p.name for p in self.dest.iterdir() if p.is_dir() and not SNAPSHOT_RE.match(p.name)]
        self.assertEqual(leftovers, [])

    def test_sha256sums_matches_every_file(self) -> None:
        self.assert_ok(self.run_backup())
        snap = self.snapshots()[0]
        sums = (snap / "SHA256SUMS").read_text().splitlines()
        listed = sorted(line.split("  ", 1)[1] for line in sums)
        self.assertEqual(listed, sorted(self.files_of(snap)))
        check = subprocess.run(["sha256sum", "--strict", "-c", "SHA256SUMS"], cwd=snap,
                               capture_output=True, text=True)
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)

    def test_second_run_without_change_creates_nothing(self) -> None:
        self.assert_ok(self.run_backup())
        before = self.tree_state()
        result = self.run_backup()
        self.assert_ok(result)
        self.assertEqual(len(self.snapshots()), 1, result.stdout)
        self.assertEqual(self.tree_state(), before)
        self.assertIn("sem mudança", result.stdout)

    def test_one_byte_change_creates_new_snapshot_and_keeps_old(self) -> None:
        self.assert_ok(self.run_backup())
        first = self.snapshots()[0]
        old_files = self.files_of(first)
        old_sums = (first / "SHA256SUMS").read_bytes()
        changed = bytearray(SAVE)
        changed[1234] ^= 0x01
        (self.sd / "switch/oot3dre/savedata/save03.bin").write_bytes(bytes(changed))
        result = self.run_backup()
        self.assert_ok(result)
        snaps = self.snapshots()
        self.assertEqual(len(snaps), 2, result.stdout)
        self.assertEqual(self.files_of(snaps[0]), old_files)
        self.assertEqual((snaps[0] / "SHA256SUMS").read_bytes(), old_sums)
        self.assertEqual(self.files_of(snaps[1])["switch/oot3dre/savedata/save03.bin"], bytes(changed))
        self.assertIn("savedata/save03.bin", (snaps[1] / "manifest.txt").read_text())

    def test_deleted_save_on_switch_is_kept_in_old_snapshot_and_reported(self) -> None:
        self.assert_ok(self.run_backup())
        (self.sd / "switch/oot3dre/savedata/save04.bin").unlink()
        result = self.run_backup()
        self.assert_ok(result)
        snaps = self.snapshots()
        self.assertEqual(len(snaps), 2)
        self.assertIn("switch/oot3dre/savedata/save04.bin", self.files_of(snaps[0]))
        self.assertNotIn("switch/oot3dre/savedata/save04.bin", self.files_of(snaps[1]))
        self.assertRegex(result.stdout + result.stderr, r"(?i)sumiu.*save04\.bin")

    def test_never_deletes_snapshots_only_warns(self) -> None:
        for value in range(5):
            (self.sd / "switch/oot3dre/config/fps60_mode").write_bytes(str(value).encode())
            self.assert_ok(self.run_backup("--warn-after", "3"))
        snaps = self.snapshots()
        self.assertEqual(len(snaps), 5)
        result = self.run_backup("--warn-after", "3")
        self.assert_ok(result)
        self.assertEqual(len(self.snapshots()), 5)
        self.assertRegex(result.stdout + result.stderr, r"(?i)aviso.*5 snapshots")

    def test_per_title_folders_and_savedata_backups_are_included(self) -> None:
        title = self.sd / "switch/triaevum/00040000000ab300"
        (title / "savedata").mkdir(parents=True)
        (title / "savedata/save.bin").write_bytes(b"mario")
        (title / "extdata/0/00000000/f000000b").mkdir(parents=True)
        (title / "extdata/0/00000000/f000000b/gamecoin.dat").write_bytes(b"coins")
        (title / "game").mkdir()
        (title / "game/code.bin").write_bytes(b"\0")
        backups = self.sd / "switch/oot3dre/savedata-backups/20261105-160000"
        backups.mkdir(parents=True)
        (backups / "save00.bin").write_bytes(SAVE)
        self.assert_ok(self.run_backup())
        files = self.files_of(self.snapshots()[0])
        self.assertEqual(files["switch/triaevum/00040000000ab300/savedata/save.bin"], b"mario")
        self.assertIn("switch/triaevum/00040000000ab300/extdata/0/00000000/f000000b/gamecoin.dat", files)
        self.assertIn("switch/oot3dre/savedata-backups/20261105-160000/save00.bin", files)
        self.assertNotIn("switch/triaevum/00040000000ab300/game/code.bin", files)

    def test_switch_unavailable_exits_zero_without_snapshot(self) -> None:
        with socket.socket() as probe:  # porta local fechada: FTP "desligado"
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        result = self.run_backup("--host", "127.0.0.1", "--port", str(port), source=False)
        self.assert_ok(result)
        self.assertIn("Switch indisponível", result.stdout)
        self.assertEqual(self.snapshots(), [])

    def fake_ssh_env(self, *, fail: bool = False) -> tuple[dict[str, str], Path]:
        """ssh falso: roda o comando remoto localmente com HOME = pasta do "VPS"."""
        bindir = self.scratch / "bin"
        bindir.mkdir(exist_ok=True)
        remote_home = self.scratch / "vps-home"
        remote_home.mkdir(exist_ok=True)
        body = ("#!/bin/sh\nexit 255\n" if fail else
                '#!/bin/sh\nwhile [ "${1#-}" != "$1" ]; do\n'
                '  case "$1" in -o) shift 2 ;; *) shift ;; esac\ndone\n'
                f'shift\ncd "{remote_home}" && HOME="{remote_home}" exec sh -c "$*"\n')
        (bindir / "ssh").write_text(body)
        (bindir / "ssh").chmod(0o755)
        env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}")
        return env, remote_home / "backups/oot3dre-save"

    def test_remote_copy_is_verified_and_idempotent(self) -> None:
        env, remote = self.fake_ssh_env()
        # Snapshot feito sem remoto fica pendente e sobe na próxima rodada com remoto.
        self.assert_ok(self.run_backup())
        (self.sd / "switch/oot3dre/savedata/save00.bin").write_bytes(SAVE[::-1])
        result = self.run_backup("--remote", "vps", env=env)
        self.assert_ok(result)
        local = [p.name for p in self.snapshots()]
        self.assertEqual(sorted(p.name for p in remote.iterdir() if SNAPSHOT_RE.match(p.name)), local)
        for name in local:
            self.assertEqual(self.files_of(remote / name), self.files_of(self.dest / name))
            check = subprocess.run(["sha256sum", "-c", "SHA256SUMS"], cwd=remote / name,
                                   capture_output=True, text=True)
            self.assertEqual(check.returncode, 0, check.stdout)
        self.assertEqual([p.name for p in remote.iterdir() if not SNAPSHOT_RE.match(p.name)], [])
        remote_state = {str(p.relative_to(remote)): p.read_bytes() for p in remote.rglob("*") if p.is_file()}
        self.assert_ok(self.run_backup("--remote", "vps", env=env))
        self.assertEqual({str(p.relative_to(remote)): p.read_bytes()
                          for p in remote.rglob("*") if p.is_file()}, remote_state)

    def test_remote_failure_keeps_local_snapshot_and_returns_error(self) -> None:
        env, _ = self.fake_ssh_env(fail=True)
        result = self.run_backup("--remote", "vps", env=env)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(len(self.snapshots()), 1)
        check = subprocess.run(["sha256sum", "-c", "SHA256SUMS"], cwd=self.snapshots()[0],
                               capture_output=True, text=True)
        self.assertEqual(check.returncode, 0)

    def test_vps_is_off_by_default(self) -> None:
        """Pedido do usuário (05/11): nada vai ao VPS sem ele pedir (--remote vps)."""
        result = self.run_backup()
        self.assert_ok(result)
        self.assertEqual(len(self.snapshots()), 1)
        self.assertFalse(self.ssh_log.exists(),
                         "ssh chamado sem --remote: " + (self.ssh_log.read_text() if self.ssh_log.exists() else ""))
        self.assertNotIn("VPS", result.stdout + result.stderr)
        # Mesmo com snapshots pendentes e mais rodadas (com e sem mudança), continua sem ssh.
        (self.sd / "switch/oot3dre/savedata/save00.bin").write_bytes(SAVE[::-1])
        self.assert_ok(self.run_backup())
        self.assert_ok(self.run_backup())
        self.assertEqual(len(self.snapshots()), 2)
        self.assertFalse(self.ssh_log.exists())

    def corrupt_in_place(self, path: Path) -> None:
        """Simula bit podre no disco: muda 1 byte sem trocar o inode (pega os hardlinks)."""
        path.chmod(0o644)
        data = bytearray(path.read_bytes())
        data[len(data) // 2] ^= 0xFF
        with path.open("r+b") as handle:
            handle.write(bytes(data))
        path.chmod(0o444)

    def assert_snapshot_ok(self, snap: Path) -> None:
        check = subprocess.run(["sha256sum", "--strict", "-c", "SHA256SUMS"], cwd=snap,
                               capture_output=True, text=True)
        self.assertEqual(check.returncode, 0, f"{snap.name}: {check.stdout}{check.stderr}")

    def test_corrupted_previous_snapshot_does_not_block_new_backups(self) -> None:
        """Verificação adversarial (05/11): 1 byte podre no snapshot anterior fazia o ln ligar o
        arquivo ruim e todas as rodadas seguintes saírem com rc=1, sem guardar o save novo."""
        self.assert_ok(self.run_backup())
        first = self.snapshots()[0]
        bad = "switch/oot3dre/savedata/save03.bin"
        self.corrupt_in_place(first / bad)
        changed = bytearray(SAVE)
        changed[7] ^= 0x01
        (self.sd / "switch/oot3dre/savedata/save00.bin").write_bytes(bytes(changed))
        result = self.run_backup()
        self.assert_ok(result)
        snaps = self.snapshots()
        self.assertEqual(len(snaps), 2, result.stdout + result.stderr)
        new = snaps[1]
        self.assert_snapshot_ok(new)
        self.assertEqual(self.files_of(new)["switch/oot3dre/savedata/save00.bin"], bytes(changed))
        self.assertEqual(self.files_of(new)[bad], SAVE)  # copiado de novo da origem
        self.assertFalse((new / bad).samefile(first / bad), "hardlink para o arquivo corrompido")
        # Arquivo íntegro do anterior continua virando hardlink.
        self.assertTrue((new / "switch/oot3dre/savedata/save04.bin").samefile(
            first / "switch/oot3dre/savedata/save04.bin"))
        self.assertRegex(result.stderr, rf"AVISO.*{first.name}.*corrompido.*save03\.bin")
        self.assertIn(f"corrompido_no_anterior\t{bad}", (new / "manifest.txt").read_text())
        # A rodada seguinte, sem mudança, não cria nada nem dá erro.
        again = self.run_backup()
        self.assert_ok(again)
        self.assertEqual(len(self.snapshots()), 2, again.stdout + again.stderr)
        self.assertIn("sem mudança", again.stdout)

    def test_corrupted_previous_snapshot_without_change_publishes_good_copy(self) -> None:
        """Se nada mudou no Switch mas o arquivo guardado apodreceu, o "sem mudança" deixaria o
        save sem nenhuma cópia boa no PC: publica um snapshot novo com a cópia boa."""
        self.assert_ok(self.run_backup())
        first = self.snapshots()[0]
        bad = "switch/oot3dre/savedata/system.dat"
        self.corrupt_in_place(first / bad)
        result = self.run_backup()
        self.assert_ok(result)
        snaps = self.snapshots()
        self.assertEqual(len(snaps), 2, result.stdout + result.stderr)
        self.assert_snapshot_ok(snaps[1])
        self.assertEqual(self.files_of(snaps[1])[bad], SYSTEM)
        self.assertRegex(result.stderr, rf"AVISO.*{first.name}.*corrompido.*system\.dat")


if __name__ == "__main__":
    unittest.main()
