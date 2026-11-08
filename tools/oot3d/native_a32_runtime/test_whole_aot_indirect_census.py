from __future__ import annotations

import contextlib
import io
import json
import tempfile
import unittest
from pathlib import Path

from whole_aot_indirect_census import (
    Census,
    census_files,
    load_program,
    main,
    read_census,
    summarize,
)


def census_text(rows: list[tuple[int, int, int]], dropped: int = 0, threads: int = 2) -> str:
    """A census as Oot3dIndirectProfile::CensusText writes it."""
    calls = sum(count for _, _, count in rows) + dropped
    lines = ["# oot3d indirect census v1",
             f"# calls={calls} pairs={len(rows)} dropped={dropped} threads={threads}",
             "# return target count"]
    lines += [f"{returned:08X} {target:08X} {count}" for returned, target, count in rows]
    return "\n".join(lines) + "\n"


# Caller: a BLX at 0x100010 (returns to 0x100014) and a computed call at
# 0x100040 whose resume is 0x100058; TargetA/TargetB are called.
PROGRAM = {
    "functions": [
        {"name": "Caller", "entry": 0x100000, "end": 0x100100, "indirect_sites": [
            {"site": 0x100010, "kind": "indirect_call"},
            {"site": 0x100040, "kind": "indirect_call"},
            {"site": 0x100080, "kind": "indirect_branch"}]},
        {"name": "TargetA", "entry": 0x200000, "end": 0x200010, "indirect_sites": []},
        {"name": "TargetB", "entry": 0x200100, "end": 0x200110, "indirect_sites": []},
        # No "end": its range ends with its last block.
        {"name": "Helper", "entry": 0x2FFFF0, "blocks": [7], "indirect_sites": []},
    ],
    "blocks": [
        {"pc": 0x100000, "successors": [
            {"kind": "indirect_call", "site": 0x100010},
            {"kind": "resume", "site": 0x100010, "target": 0x100014}]},
        {"pc": 0x100030, "successors": [
            {"kind": "indirect_call", "site": 0x100040},
            {"kind": "resume", "site": 0x100040, "target": 0x100058}]},
        {"pc": 0x100058, "successors": [{"kind": "resume", "site": 0x100060, "target": 0x100064}]},
        {"id": 7, "pc": 0x2FFFF0, "end_pc": 0x300010, "successors": []},
    ],
}

SESSION_1 = census_text([(0x100014, 0x200000, 60), (0x100014, 0x200100, 20),
                         (0x100058, 0x200000, 10)], dropped=3)
SESSION_2 = census_text([(0x100014, 0x200000, 40), (0x300004, 0x200100, 5)])


def pgo_folder(root: Path) -> Path:
    """A copied sdmc:/switch/oot3dre/pgo: two censuses, a cut one, streams."""
    folder = root / "pgo"
    folder.mkdir()
    (folder / "oot3d_pgo_10.indirect.txt").write_text(SESSION_2, encoding="ascii")
    (folder / "oot3d_pgo_9.indirect.txt").write_text(SESSION_1, encoding="ascii")
    (folder / "oot3d_pgo_11.indirect.txt.tmp").write_text("# oot3d indirect", encoding="ascii")
    (folder / "oot3d_pgo_9.stream").write_bytes(b"gcda")
    (folder / "dumps.txt").write_text("x\n", encoding="ascii")
    return folder


class ReadTest(unittest.TestCase):
    def test_folder_sessions_by_number_without_tmp_or_streams(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = pgo_folder(Path(temp))
            single = Path(temp) / "extra.txt"
            single.write_text(SESSION_2, encoding="ascii")
            names = [path.name for path in census_files([folder, single])]
            self.assertEqual(names, ["oot3d_pgo_9.indirect.txt", "oot3d_pgo_10.indirect.txt",
                                     "extra.txt"])

    def test_pairs_add_up_over_files(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            census = Census()
            for path in census_files([pgo_folder(Path(temp))]):
                read_census(path, census)
            self.assertEqual(census.pairs, {(0x100014, 0x200000): 100, (0x100014, 0x200100): 20,
                                            (0x100058, 0x200000): 10, (0x300004, 0x200100): 5})
            self.assertEqual((census.header_calls, census.dropped), (138, 3))

    def test_the_runtime_writers_text(self) -> None:
        # oot3d_native_perf_diagnostics_tests.cpp's TestIndirectCensusText.
        text = ("# oot3d indirect census v1\n# calls=11 pairs=4 dropped=0 threads=1\n"
                "# return target count\n00461788 0021A000 5\n002E4058 00461344 3\n"
                "00100004 00461340 2\n002E4058 00461340 1\n")
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "oot3d_pgo_1.indirect.txt"
            path.write_text(text, encoding="ascii")
            census = Census()
            read_census(path, census)
            self.assertEqual(sum(census.pairs.values()), 11)
            self.assertEqual(census.pairs[(0x00100004, 0x00461340)], 2)

    def test_rejects_other_files_and_short_censuses(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            other = Path(temp) / "a.txt"
            other.write_text("hello\n", encoding="ascii")
            with self.assertRaisesRegex(ValueError, "not an indirect call census"):
                read_census(other, Census())
            short = Path(temp) / "b.txt"
            short.write_text(SESSION_1.rsplit("\n", 2)[0] + "\n", encoding="ascii")
            with self.assertRaisesRegex(ValueError, "2 rows, header says pairs=3"):
                read_census(short, Census())


class SummaryTest(unittest.TestCase):
    def merged(self, temp: Path) -> Census:
        census = Census()
        for path in census_files([pgo_folder(temp)]):
            read_census(path, census)
        return census

    def test_shares_and_coverage(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            result = summarize(self.merged(Path(temp)), None)
        summary = result["summary"]
        self.assertEqual((summary["calls"], summary["sites"], summary["pairs"]), (135, 3, 4))
        self.assertEqual(summary["single_target_sites"], 2)
        self.assertAlmostEqual(summary["single_target_call_share"], 15 / 135)
        self.assertAlmostEqual(summary["top_target_coverage"]["1"], 115 / 135)
        self.assertAlmostEqual(summary["top_target_coverage"]["2"], 1.0)
        self.assertAlmostEqual(summary["top_target_coverage"]["3"], 1.0)
        self.assertEqual([site["return"] for site in result["sites"]],
                         ["0x00100014", "0x00100058", "0x00300004"])
        hottest = result["sites"][0]
        self.assertEqual((hottest["calls"], hottest["target_count"]), (120, 2))
        self.assertEqual([(target["target"], target["calls"]) for target in hottest["targets"]],
                         [("0x00200000", 100), ("0x00200100", 20)])
        self.assertAlmostEqual(hottest["targets"][1]["share"], 20 / 120)
        self.assertNotIn("site", hottest)

    def test_program_names_sites_and_targets(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            program_path = Path(temp) / "aot_program.json"
            program_path.write_text(json.dumps(PROGRAM), encoding="utf-8")
            result = summarize(self.merged(Path(temp)), load_program(program_path))
        blx, computed, unknown = result["sites"]
        self.assertEqual((blx["site"], blx["kind"], blx["function"]),
                         ("0x00100010", "indirect_call", "Caller"))
        self.assertEqual([target["function"] for target in blx["targets"]], ["TargetA", "TargetB"])
        # A computed call returns to its resume edge, not site + 4.
        self.assertEqual((computed["site"], computed["function"]), ("0x00100040", "Caller"))
        # No call site returns there: named by the function holding return - 4.
        self.assertEqual((unknown["site"], unknown["function"]), (None, "Helper"))
        self.assertEqual((result["summary"]["unmapped_sites"], result["summary"]["unmapped_calls"]),
                         (1, 5))


class MainTest(unittest.TestCase):
    def test_writes_json_and_prints_the_summary(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            folder = pgo_folder(root)
            program_path = root / "aot_program.json"
            program_path.write_text(json.dumps(PROGRAM), encoding="utf-8")
            output = root / "census.json"
            stdout = io.StringIO()
            with contextlib.redirect_stdout(stdout):
                code = main([str(folder), "--program", str(program_path),
                             "--output", str(output), "--top", "2"])
            self.assertEqual(code, 0)
            result = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(result["format"], "oot3d_whole_aot_indirect_census_v1")
            self.assertEqual(len(result["sites"]), 3)
            text = stdout.getvalue()
            self.assertIn("2 census file(s): 135 calls at 3 sites, 4 pairs, 3 dropped", text)
            self.assertIn("single-target sites: 2, 11.1% of calls", text)
            self.assertIn("hottest 1/2/3 targets per site cover 85.2% / 100.0% / 100.0%", text)
            self.assertIn("top 2 sites by calls", text)
            self.assertIn("  00100010 Caller: 120 (88.9%), 2 target(s): 00200000 TargetA 83%, "
                          "00200100 TargetB 17%", text)
            self.assertNotIn("00300004", text)

    def test_no_census_is_an_error(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            with self.assertRaisesRegex(SystemExit, "no oot3d_pgo_<N>.indirect.txt"):
                main([temp])


if __name__ == "__main__":
    unittest.main()
