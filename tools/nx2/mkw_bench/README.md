# Mario Kart World race benchmark

Repeatable in-race frame times for `nemyu-cli`: a key script enters Grand Prix with the default
choices (Mushroom Cup, Mario Bros. Circuit) and holds accelerate, and the frame statistics come
from the `nx2:vi2` lines the emulator logs every 600 presents.

```
.\bench.ps1 -Tag base -Game "<dump>\exefs\main" -Config "$env:APPDATA\nemyu\config\qt-config.ini"
.\bench.ps1 -Tag half -Game ... -Config (python variant.py <config> half.ini resolution_setup=1)
```

`-EnvPairs "EDEN_NX2_GPU_PROFILE=1"` adds the GPU-thread zone timers to the log;
`EDEN_NX2_SAMPLE=CPUCore_*,GPU,GPUBack,VulkanWorker` with `_START=170 _SECONDS=60 _OUT=<file>`
samples the race (`samples.py <file>` summarizes it). `-Capture <dir>` saves every 120th frame;
`contact.py <dir> <sheet.png>` makes a contact sheet. Outputs go to `out\`. `-Idle` presses
nothing, so the game stays on its title screen with its movie looping. The game picks its movie
at boot: when it is the 640x360 one (about one boot in five), `EDEN_NX2_SYNCPT_POLL_DELAY_US=200`
with `EDEN_NX2_SYNCPT_VALUES_FIRST=1` reproduces the movie player's 2162-0001 abort within a
second, and without the old order the movie plays on. Every run prints the guest aborts in its log.

A run is killed after `-Seconds`, so the emulator never shuts down and saves nothing at exit (the
GUI does). `-QuitAtFrame <n>` ends it at that presented frame instead, the way closing the window
does: the log shows `Key script: quit`, `run.ps1` prints `exited_by_itself=True`, and the shader
cache's `vulkan_used.bin` is written again. `-Seconds` still kills a run whose shutdown hangs. Back
the cache up first when a run should not change it.

The script needs the save data to have the default choices unlocked. `racestats.py` finds the
race load, the longest frame over 3 s in windows 3-8 (usually the 6th, presents 3001-3600; a menu
can also hold one over 3 s), and counts from the third window after it, when the countdown is
over; without one it counts from the 9th. The `game speed` line under it is the race's game time
per real second, from the vblanks each present asked for in its word 12 (the game advances that
many each frame), with how many presents asked for 2+ vblanks and how often word 12 changed; `-`
for a build that does not log them. The `boot` line covers windows 1-2, from the boot to the title screen (the
loading screen), and counts the runtime pipelines and the CPU reads of GPU-written memory (logged
every 64th) reported in that span.

The lines after it are for the loading screen and the menus:
- `preload`: the pipelines built at boot (`Total Pipeline Count`), the cached ones left for when
  they are first needed, the used-pipeline list the boot read, whether frame generation was on,
  and the frame-rate mode the loader applied to the game (`dynamic frame rate on`, `30 fps lock
  on`, `frame-rate mode off`, or `... not applied`).
- `progress`: the log time from present #1 to presents #600 and #1200 and to the title screen
  key press (frame 1500), which shows how far the loading got where the frame average does not.
- `window 1`, `window 2` and `menus` (window 3 up to the one before the race load): frame time,
  longest frame and frames over 100 ms, then what the log says held them. Runtime pipelines with
  how many came from the disk cache, were task/mesh or had their draws skipped while building;
  draws and dispatches that waited for a pipeline build; CPU read traps (their waits and the
  flushes in them); guest writes blocked on the cache locks; presents held over 100 ms; "Long
  GPU-thread operation" lines; and the MiB of zeros left out of unwritten sparse or GPU-only
  blocks and of sparse blocks backed; last, the game speed, as under the race line. A summary
  line counts in the window that holds the middle of the time it covers: the first event 10 s or
  more after the last line logs it, so it can cover much more, and it says how long (older
  builds' runtime pipelines lines do not and count as 10 s).
  The window's ms of runtime builds, build waits, trap waits and blocked writes are also shown per
  10 s of the time its lines cover. A `-` is a line this build does not log.

A boot A/B only compares runs with the same frame generation and a similar preload: a run that
builds 400 pipelines at boot instead of 2000 translates hundreds more while loading.

The game's frame-rate mode (`mkw_frame_rate`, Dynamic by default) changes how much game time each
present covers, while the key script and `racestats.py` count presents. Pin it in every run with
`-EnvPairs "EDEN_NX2_MKW_FPS_MODE=..."`: `off` for a performance A/B against runs from before
the mode existed, which all ran the game as released. Compare the same number of race windows,
and check that both sides' `load in window N` agree. Under Dynamic the race covers up to ~1.4x
more game time per present at 41-45 fps; under `30` the course intro and countdown take about
half the presents, so the countdown can end well before the accelerate key at frame 4250
(`ACCELERATE_FRAME` in make_keys.py): move it earlier, or end the run with `-QuitAtFrame`.

After the GPU load, the bench prints how many race samples nvidia-smi reports held by the power
cap, by temperature or by a hardware slowdown, the P-states and the enforced power limit, then the
power source before and after the run and any change Windows logged meanwhile (Kernel-Power 105,
also saved to `out\<tag>.power.txt`). On battery the laptop caps the GPU and CPU (a mesh shader
A/B once read 31 against 23 ms that way), so rerun any run that ends with the battery warning.

## Measurements (RTX 3050 6 GB Laptop, 16 GB, MSVC build, 2026-11-01)

| Settings | Frame | fps | Over 100 ms |
| --- | --- | --- | --- |
| Current (Low accuracy, 0.75x + FSR 3, BC3, conservative VRAM, VSync off) | 25.6-27.2 ms | 37-39 | 10-17 per run |
| 0.5x | 23.8 ms | 42 | 7 |
| 0.25x | 26.8 ms | 37 | 11 |
| ASTC uncompressed | 27.9 ms | 36 | 8 |
| `EDEN_NX2_READ_TRAPS=0` (8x fewer submits) | 24.5 ms | 41 | 13 |
| Submit every 128 draws instead of 4096 | 24.5 ms | 41 | 6 |
| `EDEN_NX2_GPU_WRITTEN_CMDLISTS=0` | 16.7 ms | 60 | 0, but the race renders black |
| NVN's indexed draw macro drawn indirectly (default since) | 23.1, 24.5, 24.2 ms | 42 | 14, 12, 18 |
| Same builds, `EDEN_NX2_INDIRECT_DRAWS=0` | 28.4, 30.3, 28.7 ms | 34 | 17, 16, 9 |
| GPU accuracy High instead of Low | 41.7 ms | 24 | 7 |
| 216 sparse tile maps (default since) | 24.2 ms | 41 | 5 |
| Same build, `EDEN_NX2_SPARSE_REMAP=0` | 24.5 ms | 41 | 11 |
| 16 MiB VMA blocks, separate buffer and sparse pools (default since) | 24.5 ms | 41 | 15 |

High accuracy showed the same vegetation as Low in the captures, so Switch 2 programs now run
Low unless set per game; `EDEN_NX2_GPU_ACCURACY=high` measures High. With the 216 maps the sparse
blocks backed by GPU writes stay at 0 (2.5 GB by 200 s without them); the 5.8 GB texture pool is
created GPU-only instead, which reserves emulated memory but not host commit. The VMA changes
took ~110 MiB off the device memory in use during the race.

`-Soak -Seconds 1200` (two races): race 1 averages 21-26 ms; race 2, the open route from Mario
Bros. Circuit to Crown City, 28-30 ms. Before the VMA changes, VMA usage in race 2 stayed up to
300 MiB over the 5233 MiB budget (22 of 22 samples), with 1.4-2.7 s stalls as Windows paged
video memory out; with them it goes over in 2-4 of ~20 samples and the longest stall is 0.5-1.1 s.
Host commit stays flat between races (9.9-10.4 GB).

Run-to-run noise is about +-1.5 ms; frames over 100 ms vary more (9-18 per run on one build),
mostly in the first three race windows. Nothing else may run meanwhile: a second emulator
instance skewed a run by several ms. Vulkan timestamps showed the GPU executing command buffers
only ~34% of the time (~9 ms a frame; nvidia-smi's "utilization" reads ~87% regardless), and
nearly all of it while the GPU thread waits in Scheduler::Finish: the culling compute writes the
arguments of NVN's indexed draw macro (5 words: count, instances, first index, vertex offset, base
instance) into the command stream, so the GPU thread downloads them ~8 times a frame
(FlushGpuWrittenCommands), and CPU and GPU never overlap. Maxwell3D::FrontTakeIndirectArguments
now runs those macros as indirect draws that read the arguments from GPU memory.
