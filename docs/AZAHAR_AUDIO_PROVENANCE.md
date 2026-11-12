# Azahar audio provenance

## Identity

- Upstream: `https://github.com/azahar-emu/azahar.git`
- Upstream revision: `beb5681ee7f85586501b16b083a961b707092cd7`
- TriAevum import commit: `c3313fb20796522f43c177fbd071b069384bc2aa`
- Upstream license for imported files: GPL-2.0-or-later

The local Azahar checkout used during development identifies that revision as
its HEAD immediately before the TriAevum import. Git object comparison between
the two repositories proves that 15 of the 19 imported `audio_core` and
`common` files are byte-identical blobs at the stated revisions.

## Adapted files

Four imported paths intentionally do not have identical blob IDs:

- `audio_core/hle/source.cpp` and `source.h` replace Azahar's concrete
  `MemorySystem` dependency with a bounded physical-memory resolver callback;
- `common/assert.h` and `common/logging/log.h` are reduced compatibility shims
  for the isolated audio build.

The five tiny `boost/serialization` headers are local include shims and are not
represented as copied Azahar implementation. Later TriAevum commits add
save-state validation and runtime integration; Git history preserves those
changes separately from the donor baseline.

## Switch port changes

The Switch port (GustavoHenriqueRocha/TriAevum-Switch and its English fork)
changes these GPL-2.0-or-later files further; each carries a notice:

- 2026-10-17, `4282aae7`: optional per-source interpolation (cubic and sinc)
  in `audio_core/interpolate.cpp`, `interpolate.h` and `hle/source.cpp`;
- 2026-10-17, `8ee3f632`: cheaper HLE DSP mixing, bit-for-bit identical, in
  `audio_core/codec.cpp`, `codec.h`, `hle/mixers.cpp`, `hle/mixers.h`,
  `hle/source.cpp`, `interpolate.cpp` and `sample_queue.h`;
- 2026-11-05, `f8b96aa2`: mixing in place, ADPCM decodes shared between
  sources and two outputs per step in `audio_core/interpolate.cpp`,
  `hle/source.cpp`, `hle/source.h` and `sample_queue.h`, with the new
  TriAevum file `audio_core/adpcm_decode_cache.h`.

`sample_queue.h` and `adpcm_decode_cache.h` are TriAevum storage code (their
headers say so); the audio algorithms they serve remain Azahar's.

## Reproduction

With the Azahar repository available locally, compare the trees with:

```text
git -C <azahar> ls-tree -r beb5681e src/audio_core src/common
git ls-tree -r c3313fb20 tools/oot3d/third_party/azahar_audio
```

Matching Git blob IDs establish exact file identity independently of checkout
line endings. The four adapted paths must instead be reviewed as diffs against
the stated upstream revision.
