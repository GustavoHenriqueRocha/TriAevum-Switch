Português (original): [SWITCH_RESEARCH_2026-10-17.pt-BR.md](SWITCH_RESEARCH_2026-10-17.pt-BR.md)

# Research: techniques from other 3D Switch homebrew projects (2026-10-17)

Main source: **SwitchWakerHD** (Wind Waker HD recompiled PPC→C on the Switch; 48 measured optimization rounds):
https://github.com/centollOS/SwitchWakerHD (docs/switch-port.md, docs/deko3d-plan.md).

## Findings
- Mesa/nouveau ≈ half of the render thread's time; switch-mesa has glthread but EGL never starts it (SwitchWakerHD forced it via st_set_background_context).
- glGetError/glGen* synchronize with glthread; loose uniforms make nvc0 copy the whole block into the FIFO (std140 UBO cut commands to 1/3); **mallinfo locks malloc** (used in perf.txt since 1e2bea5 every 150 frames — evaluate).
- deko3d: fps equal/slightly better than GL, without Mesa's GL thread, ~400 MiB more heap, zero shader stutters (offline DKSH cache). Does not improve the GPU.
- NXVK (https://github.com/PalindromicBreadLoaf/nxvk): NVK on the Switch, Vulkan 1.4 + GL 4.5 via Zink; adopted by Dekopon (Azahar on the Switch, https://github.com/PalindromicBreadLoaf/dekopon). No public benchmark vs switch-mesa 20.1.
- Core 3 unavailable (hbloader NPDM: cores 0–2). Game alone on one core: SwitchWakerHD 20→28 fps.
- Order hot functions contiguously (500 functions = 88% of the time in 4.5 MB) — complementary to PGO.
- newlib malloc with a global lock shared with Mesa: take malloc off the hot path (pools/rings).
- appletSetCpuBoostMode(FastLoad): CPU 1785 but GPU 76.8 MHz → only during loads (Dekopon: CpuBoostScope).
- Don't use clkrst in the app (sleep problem, SwitchWakerHD issue #33); leave it to sys-clk. RAM at 1600 in handheld is an official profile.
- SoH/2S2H on the Switch: SDL3 + GL switch-mesa; interpolation redoes the whole frame (same cost as ours).

## Ranking of opportunities
1. Make the interpolated frame cheaper (reuse the bottom screen, dynamic resolution on the intermediate, drop intermediates that don't fit).
2. Clocks via the safe path (RAM 1600, GPU 614 via sys-clk, FastLoad only during loads).
3. Measure the GPU per pass (GL_TIMESTAMP), count barriers/clears/zcull.
4. NXVK experiment (GL via Zink, then Vulkan).
5. Forced glthread + synchronization audit; deko3d as a long-term project.
Outside the top 5: take malloc off the hot path; order hot functions contiguously.
