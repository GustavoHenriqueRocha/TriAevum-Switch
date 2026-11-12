Português (original): [SWITCH_RESEARCH_XBOX360_2026-10-17.pt-BR.md](SWITCH_RESEARCH_XBOX360_2026-10-17.pt-BR.md)

# Research: Xbox 360/PC ports to Switch homebrew (2026-10-17)

Main reference: **StevensND/nfsmw-nx** (NFS Most Wanted 2005 X360, static PPC→C++ recompilation via ReXGlue, Vulkan renderer on NVK/mesa-switch Mesa 26; docs/ folder with measurements). Others: Kaays/nfscarbon-nx, UnleashedRecomp-NX, MarathonRecomp-NX, autorun (Wine+FEX/Box64).

## Applicable lessons
- **Official clock**: homebrew in handheld starts with GPU 307.2 MHz; `apmSetPerformanceConfiguration(Normal, 0x92220008)` → GPU 460.8 / RAM 1331 (commercial games' config; check for ~1.5 s; 0x92220007 = RAM 1600). TriAevum doesn't call apm (current user uses sys-clk).
- **Threads on Horizon**: only priority 0x3B time-slices; at others, a busy thread blocks those of equal/lower priority on the same core. Pinning the render thread to an exclusive mask worsened their worst case. A57 without LSE: per-draw atomics are expensive (83 thousand/frame = 1.1 ms); timing per draw ≈ ¼ of the render thread → sample 1/128. Eliminate busy waiting (Sleep(0)).
- **CPU per draw** (~16 µs on console): state in a per-draw marker, only changed constants, uploads without duplicates on a copy thread, texture hash checking made rarer (66→4.2 µs/draw), logs off the render thread, key structs without padding, local variables in hot loops.
- **GPU** (L2 256 KB, non-tiler): ZCULL (~2 ms), sky last (~3 ms), resolve without copy, shaders without branches; FP16 useless.
- **Driver**: creating GPU memory is slow; reuse texture objects of the same size/format in the LRU; NvMap handle limit (~4000 with 8 MB transfermem, error 0x235C).
- **Pacing**: unstable 60 is worse than locked 30 if p90 of the step frame > ~15.5 ms; check for a limiter that sleeps a whole period when running late.
- **Build**: weak aliases prevent inlining; guest registers as locals (save/restore ≈ 14% of the main thread); PGO on Horizon: -fno-profile-values and -fprofile-correction.
- **Audio**: pump at a fixed rate (187.5 Hz), ~53 ms queue, priority 0x2B.

## Top 5 for us
1. apm 0x92220008 in handheld (for those who don't use sys-clk).
2. Audit of threads/priorities/busy waiting/atomics on the render thread.
3. nfsmw-style per-draw cost cut.
4. Cheap GPU: draw order, ZCULL, resolve without copy.
5. Test mesa-switch GL (Mesa 26) in place of switch-mesa 20.1; Vulkan/NVK long term.
