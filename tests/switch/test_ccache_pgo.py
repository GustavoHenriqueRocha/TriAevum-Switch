#!/usr/bin/env python3
"""Regressão: os shards com PGO do GCC passam pelo ccache (scripts/switch/ccache-pgo.sh).

Bug (05/11/2026): o ccache recusava -fprofile-partial-training e não achava o
.gcda nomeado por -fprofile-prefix-path, então todo build "use" recompilava os
256 shards. O launcher tira as flags de PGO da linha do ccache, põe no wrapper
do compilador e põe o .gcda do objeto no hash.
"""
from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

LAUNCHER = Path(__file__).resolve().parents[2] / "scripts/switch/ccache-pgo.sh"
PGO = ["-fprofile-use=PROFILE", "-fprofile-prefix-path=BUILD", "-fprofile-partial-training"]


class CcachePgoTest(unittest.TestCase):
    def run_launcher(self, flags: list[str]) -> tuple[list[str], dict[str, str], Path]:
        scratch = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, scratch)
        (scratch / "bin").mkdir()
        # ccache falso: grava os argumentos e o ambiente que recebeu.
        fake = scratch / "bin/ccache"
        fake.write_text('#!/bin/sh\nprintf "%s\\n" "$@" > "$PWD/ccache.args"\n'
                        'env | grep ^CCACHE_ > "$PWD/ccache.env" || true\n')
        fake.chmod(0o755)
        compiler = scratch / "bin/aarch64-none-elf-g++"
        compiler.write_text('#!/bin/sh\necho "aarch64-none-elf-g++ (teste) 16.1.0"\n')
        compiler.chmod(0o755)
        profile = scratch / "pgo"
        profile.mkdir()
        (profile / "CMakeFiles#lib.dir#shard_007.gcda").write_bytes(b"gcda")
        build = scratch / "build"
        build.mkdir()
        flags = [flag.replace("PROFILE", str(profile)).replace("BUILD", str(build)) for flag in flags]
        environment = dict(os.environ, PATH=f"{scratch / 'bin'}:{os.environ['PATH']}")
        subprocess.run([str(LAUNCHER), str(profile), str(compiler), "-O3", *flags,
                        "-o", "CMakeFiles/lib.dir/shard_007.o", "-c", "shard_007.cpp"],
                       cwd=build, env=environment, check=True)
        args = (build / "ccache.args").read_text().splitlines()
        env = dict(line.split("=", 1) for line in (build / "ccache.env").read_text().splitlines())
        return args, env, profile

    def test_pgo_flags_go_to_wrapper_and_gcda_is_hashed(self) -> None:
        args, env, profile = self.run_launcher(PGO)
        self.assertFalse([arg for arg in args if arg.startswith("-fprofile")])
        wrapper = Path(args[0])
        self.assertEqual(wrapper.name, "aarch64-none-elf-g++")
        body = wrapper.read_text()
        self.assertIn("-fprofile-partial-training", body)
        self.assertIn(f"-fprofile-use={profile}", body)
        self.assertIn("16.1.0", body)  # versão do GCC real no conteúdo comparado
        self.assertEqual(env["CCACHE_COMPILERCHECK"], "content")
        self.assertEqual(env["CCACHE_EXTRAFILES"], str(profile / "CMakeFiles#lib.dir#shard_007.gcda"))

    def test_without_pgo_runs_plain_ccache(self) -> None:
        args, env, _ = self.run_launcher([])
        self.assertTrue(args[0].endswith("/bin/aarch64-none-elf-g++"))
        self.assertNotIn("CCACHE_EXTRAFILES", env)


if __name__ == "__main__":
    unittest.main()
