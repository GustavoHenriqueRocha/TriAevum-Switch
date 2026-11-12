from __future__ import annotations

import struct
import tempfile
import unittest
from pathlib import Path

from whole_aot_pgo_merge import (
    DEFAULT_GCOV_TOOL,
    TAG_ARCS,
    TAG_SUMMARY,
    fill_summaries,
    main,
    records,
    session_streams,
)

def gcda(arcs: list[int], runs: int = 0, sum_max: int = 0) -> bytes:
    data = struct.pack("<4I", 0x67636461, 0x4236312A, 1, 2)
    data += struct.pack("<Ii", TAG_SUMMARY, 8) + struct.pack("<II", runs, sum_max)
    data += struct.pack("<Ii", 0x01000000, 12) + struct.pack("<3I", 1, 2, 3)
    data += struct.pack("<Ii", TAG_ARCS, 8 * len(arcs)) + struct.pack(f"<{len(arcs)}q", *arcs)
    data += struct.pack("<Ii", TAG_ARCS, -4)  # four zero counters, no data
    return data


def summary(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    for tag, start, length in records(data):
        if tag == TAG_SUMMARY:
            return struct.unpack_from("<II", data, start)
    raise AssertionError("no summary")


def arcs(path: Path) -> list[int]:
    data = path.read_bytes()
    values: list[int] = []
    for tag, start, length in records(data):
        if tag == TAG_ARCS:
            values += list(struct.unpack_from(f"<{length // 8}q", data, start))
    return values


class FillSummariesTest(unittest.TestCase):
    def test_runs_and_largest_arc_over_every_file(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            first = Path(temp) / "a.gcda"
            second = Path(temp) / "b.gcda"
            first.write_bytes(gcda([5, 70]))
            second.write_bytes(gcda([900, 1]))
            self.assertEqual(fill_summaries([first, second], 3), (900, 2))
            self.assertEqual(summary(first), (3, 900))
            self.assertEqual(summary(second), (3, 900))
            self.assertEqual(arcs(first), [5, 70])  # counters untouched

    def test_missing_summary_is_inserted_after_the_header(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "a.gcda"
            data = gcda([8, 2])
            path.write_bytes(data[:16] + data[32:])  # without the summary record
            self.assertEqual(fill_summaries([path], 2), (8, 1))
            self.assertEqual(summary(path), (2, 8))
            self.assertEqual(arcs(path), [8, 2])

    def test_sum_max_saturates_at_32_bits(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "a.gcda"
            path.write_bytes(gcda([1 << 40]))
            self.assertEqual(fill_summaries([path], 1)[0], 0xFFFFFFFF)


class SessionStreamsTest(unittest.TestCase):
    def test_complete_streams_by_number_and_lone_tmp_skipped(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            folder = Path(temp)
            # The indirect call census (.indirect.txt) sits beside the streams.
            for name in ("oot3d_pgo_10.stream", "oot3d_pgo_2.stream",
                         "oot3d_pgo_2.stream.tmp", "oot3d_pgo_3.stream.tmp",
                         "oot3d_pgo_2.indirect.txt", "oot3d_pgo_3.indirect.txt.tmp",
                         "dumps.txt"):
                (folder / name).write_bytes(b"x")
            streams, skipped = session_streams(folder)
            self.assertEqual([path.name for path in streams],
                             ["oot3d_pgo_2.stream", "oot3d_pgo_10.stream"])
            self.assertEqual([path.name for path in skipped], ["oot3d_pgo_3.stream.tmp"])

    def test_refuses_to_add_to_an_existing_profile(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "pgo").mkdir()
            (root / "pgo" / "oot3d_pgo_1.stream").write_bytes(b"x")
            (root / "out").mkdir()
            (root / "out" / "old.gcda").write_bytes(b"x")
            with self.assertRaises(SystemExit):
                main(["--streams", str(root / "pgo"), "--output", str(root / "out"),
                      "--gcov-tool", "true"])


GCC16_VERSION = 0x4236312A  # "B61*", devkitA64 GCC 16.1


def gcfn(name: str) -> bytes:
    """__gcov_filename_to_gcfn: magic, version, then the string as its length
    with the terminating zero and its bytes (libgcc/libgcov-driver.c)."""
    raw = name.encode() + b"\0"
    return struct.pack("<3I", 0x6763666E, GCC16_VERSION, len(raw)) + raw


def info_to_gcda(ident: int, counts: list[int]) -> bytes:
    """__gcov_info_to_gcda: header, one function, no object summary, and the
    closing zero word (write_one_data)."""
    data = struct.pack("<4I", 0x67636461, GCC16_VERSION, 7, 9)
    data += struct.pack("<2I", 0x01000000, 12) + struct.pack("<3I", ident, 11, 22)
    data += struct.pack("<Ii", TAG_ARCS, 8 * len(counts)) + struct.pack(f"<{len(counts)}q", *counts)
    return data + struct.pack("<I", 0)


@unittest.skipUnless(Path(DEFAULT_GCOV_TOOL).is_file(),
                     "needs devkitA64's gcov-tool (retuned-switch-prep image)")
class EndToEndTest(unittest.TestCase):
    """Streams laid out byte for byte as oot3d_whole_aot_pgo.cpp writes them
    (filename record, then the object's data), merged by devkitA64's gcov-tool."""

    def test_two_sessions_add_up_under_the_object_name(self) -> None:
        name = ("sdmc:/switch/oot3dre/pgo/CMakeFiles#oot3d_native_whole_aot.dir#home#x#"
                "oot3d_whole_aot_shard_000.cpp.gcda")
        other = name.replace("shard_000", "shard_001")
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            streams = root / "pgo"
            streams.mkdir()
            (streams / "oot3d_pgo_1.stream").write_bytes(
                gcfn(name) + info_to_gcda(5, [10, 3]) + gcfn(other) + info_to_gcda(6, [1, 0]))
            (streams / "oot3d_pgo_2.stream").write_bytes(
                gcfn(name) + info_to_gcda(5, [30, 4]) + gcfn(other) + info_to_gcda(6, [2, 0]))
            (streams / "oot3d_pgo_3.stream.tmp").write_bytes(b"cut short")
            output = root / "profile"
            self.assertEqual(main(["--streams", str(streams), "--output", str(output)]), 0)
            files = sorted(path.name for path in output.glob("*.gcda"))
            self.assertEqual(files, [Path(name).name, Path(other).name])
            merged = output / Path(name).name
            self.assertEqual(arcs(merged), [40, 7])
            self.assertEqual(summary(merged), (2, 40))
            self.assertEqual(summary(output / Path(other).name), (2, 40))


if __name__ == "__main__":
    unittest.main()
