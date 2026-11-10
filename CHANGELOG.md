<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- lang: en-GB -->

# Changelog

Nemyu is a free and open-source Nintendo Switch 2 emulator written in C++. It is a fork of
[Eglantine](https://github.com/Djihads80/Eglantine) (`nx2` branch), which is derived from
[Eden](https://git.eden-emu.dev/eden-emu/eden), yuzu and Sudachi. This file lists what Nemyu has
changed since it forked from Eglantine on 2026-11-01 (the first Nemyu commit is `1b5caa173d`, on
top of Eglantine `a65d7bf29a`).

Much of Nemyu's work is ported from Eglantine and from other forks. Ported changes name their
origin in parentheses (fork, author); the commits keep the original authors, and
[docs/UPSTREAM_CREDITS.md](docs/UPSTREAM_CREDITS.md) lists the adaptations in detail.

`EDEN_NX2_*` and the other `EDEN_*` names below are switches: environment variables, or lines of
`nx2_switches.txt` (see [dist/nx2_switches.txt](dist/nx2_switches.txt)). Unless a switch is
described as opt-in, the new behaviour is the default and the switch restores the old one.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/). Nemyu does not follow
semantic versioning strictly while it is at 0.x.

## [Unreleased]

## [0.1.7] - 2026-11-09

The first stable build of 0.1.7. Below are the changes since 0.1.7-experimental.3; the sections of
the experimental builds list the rest of 0.1.7. Most of them ran in Ocarina of Time and Mario Kart
World on the test laptop (RTX 3050 6 GB, i5-13450HX). The every-bind check of Ocarina's uniform
banks, shader reuse, `EDEN_NX2_NAN_MINMAX`, the two new Mario Kart World logs, the Mario Kart
World video memory guard and the shader cache log as the default have not been run in a game yet.
Switches described as opt-in are off by default until they are measured.

### Added

- **Shader Manager** (Tools menu and the game list's context menu): lists each game's cache files
  with what they are (in use, set aside by another cache version, from another build or emulator),
  shows the same game's caches in other emulators' folders, imports a cache of this build's version
  or a Lichii cache (versions 52 to 54) by adding only the pipelines the game lacks, exports and
  deletes caches, and starts Prepare Shaders.
- **Save Manager** (Tools menu and the game list's context menu): saves per profile with their size
  and date, backups, and the same game's saves in other emulators (Eden, yuzu, Citron, Sudachi,
  Ryujinx). It backs up, restores, imports (from another emulator, a zip or a folder), exports as a
  zip and deletes; every import, restore and delete backs up the save it replaces first.
- **Shader warm-up from the game files** (game list: Build Shader Warm-up from Game Files, Delete
  Shader Warm-up): reads the game's own shader archives and, at game start, builds the compute
  shaders the disk cache lacks on low-priority threads, so the driver's cache already holds them
  when they are first dispatched (`EDEN_NX2_WARM_THREADS`, default 4; adapted from Lichii #579).
- **NVIDIA DLSS for Ocarina of Time** ("Use NVIDIA DLSS", off by default): DLSS Super Resolution
  draws the gameplay upscale with motion vectors from the camera and Link's pose. Only builds made
  with NVIDIA Streamline can use it (adapted from Lichii #606 and #456, Rengundom).
- Compiled pipelines are saved per game with `VK_KHR_pipeline_binary` where the driver keeps none
  between sessions (AMD on Windows: Ocarina of Time's boot to the title screen went from 513 s to
  14 s on an RX 6750 XT), in `vulkan_binaries.bin` beside the shader cache
  (`EDEN_NX2_PIPELINE_BINARIES=0` turns it off; from CheezerWheezer's Lichii).
- A host frame pacer holds each frame until just before the display refresh it should be shown at,
  using `VK_KHR_present_id` and `VK_KHR_present_wait`, so uneven frame work no longer shows as
  uneven frame times. It needs FIFO or Mailbox, stays off with frame generation or Turbo, and pauses
  for 10 s after missed present waits (`EDEN_NX2_HOST_PACE=0` only measures; from Fritchii).
- File > Turn On / Turn Off Portable Mode creates (or renames away) the `user` folder, optionally
  copying the current data into it, and restarts Nemyu.
- A crash appends its exception code, address and 32 stack frames to `nemyu_log.txt`
  (`EDEN_NX2_CRASH_LOG=0` leaves them out).
- Per-game setting **Native compute write mark** (Graphics > Advanced): a Switch 2 compute dispatch
  marks only the bytes its stores can reach as GPU-written instead of the whole binding (Auto, the
  default; Full binding restores the old behaviour; from Fritchii and Lichii #816).
- **.nx game archives** (fab918's nxformat): an archive keeps a game with its updates and add-ons in
  one file, either as extracted trees (exefs/ and romfs/, as converted from NUP or a dump folder) or
  as decrypted NCAs (as converted from an NSP or XCI). The game list shows .nx files, registers
  their content and falls back to the file name for an archive without a title. A tree boots like a
  deconstructed ROM directory; the updates and add-ons of a native archive go to the frontend
  content provider, which also covers standalone updates added from a folder and extracted add-ons
  served to the AoC service. The header-only reader is vendored in externals/nxformat (MIT). The
  converter and archive editor are not included (adapted from Lichii #923).
- Opt-in switches, to be measured before any becomes a default: `EDEN_NX2_FAST_DRAW` (draws that
  follow a draw of the same pipeline keep its image views and samplers, Lichii #858;
  `EDEN_NX2_FAST_DRAW_VERIFY=1` counts mismatches), `EDEN_NX2_RT_FRESH` (new render targets start
  empty, Lichii #834), `EDEN_NX2_TC_MINIMUM_MIB`, `EDEN_NX2_READBACK_FENCE_COPY`,
  `EDEN_NX2_READ_REQUEST_EARLY`, `EDEN_NX2_RELEASE_DRAIN_SKIP` and `EDEN_NX2_ACQUIRE_WRITER_WAIT`
  (from Fritchii), `EDEN_NX2_JIT_TRAPPED_READS` (from Orchid), `EDEN_NX2_JIT_ATOMICS_MONITOR`,
  `EDEN_NX2_NAN_MINMAX` (NaN-defined min and max on every driver; slower on NVIDIA).
- More in the log: the DMA read flush and query wait in `EDEN_NX2_GPU_PROFILE`, semaphore acquire
  drains every 5 s, device memory VMA allocated and freed, a warning when a frame runs out of
  descriptor buffer, which vertex attribute variants keep their shaders, the image memory used in
  the last 60 frames, and (`EDEN_NX2_ROMFS_READ_LOG`, on for Mario Kart World) the name of each
  large RomFS file the game reads first.
- More opt-in switches, off until measured:
  - `EDEN_NX2_GPL=1` links a graphics pipeline built in game from four parts instead of compiling it
    whole: its pre-rasterization and fragment shader libraries, taken from a cache that keeps at
    most 512 that no pipeline holds, and its own vertex input and fragment output libraries, then a
    fast link. A state variant of a known shader set then costs the two interface parts and the link
    instead of a driver compile. Boot and background prebuilds stay monolithic, as do task/mesh
    pipelines, transform feedback and a static rasterizer discard, and a failed library or link
    builds the pipeline whole. `=1` keeps it off on AMD's proprietary driver, where pipelines fast
    linked from libraries draw dotted white fringes around blended effects (clouds, particles,
    glows; Lichii #579, justin-wq1337, confirmed on RDNA4 by estevanbs); `=2` enables it there too,
    with every link optimized. `EDEN_NX2_GPL_OPTIMIZE=1` links the same libraries again with link
    time optimization on a low-priority worker and binds that pipeline from the next draw. Every 10
    seconds an `Nx2Perf gpl` log line compares linked and whole builds. Adapted from Lichii #579
    (PantACRO4life), with one descriptor set layout for all parts instead of one set per stage.
  - `EDEN_NX2_PARALLEL_STAGES=N` (0 to 4, default 0) translates the programs of a new pipeline,
    except the last, on N helper threads when the GPU thread must build it itself (adapted from
    Lichii 0c15201320).
  - `EDEN_NX2_BACK_SPLIT=<workers>` (1 to 16, default off) hands each draw's index and vertex stream
    ranges and its image descriptors to a small pool of worker threads. An idle worker polls for
    `EDEN_NX2_BACK_SPLIT_SPIN` microseconds (default 50) and then sleeps; `EDEN_NX2_BACK_SPLIT_JOBS`
    keeps one job and `EDEN_NX2_BACK_SPLIT_VERIFY=1` runs both paths and logs differences. Lichii
    reports 15-25% more FPS in Donkey Kong Bananza; nothing measured it here (adapted from Lichii
    28735f4162).
  - `EDEN_NX2_ASYNC_READBACK=1` lets a guest CPU read of GPU-written buffer memory wait only for its
    own download submission instead of for everything submitted (adapted from Lichii #1028). OpenGL
    keeps the synchronous flush.
  - `EDEN_NX2_READ_AHEAD=2` also downloads every GPU-written range in the 1 MiB block around a
    trapped guest read, for reads scattered over a few large GPU-written regions, as in Mario Kart
    World.
  - `EDEN_NX2_DRAW_LIST_COPIES=1` also keeps host copies of the command lists that draws write, so
    they stop draining the GPU split (adapted from Eglantine #791, sethsethsethsethsethsethseth).
  - `EDEN_NX2_BACK_MACROS=1` runs the macros that read memory on the GPU split's back thread, so the
    front no longer waits for the back before running them. Lichii reports about 8% more FPS in
    Ocarina of Time; nothing measured it here (adapted from Lichii #1056).
  - `EDEN_NX2_TC_JOIN_LAYERS=1` skips the upload of a joined image when copies of its layers
    overwrite it whole (adapted from Lichii #1079).
  - `EDEN_NX2_DLSS_PRESET` also takes the DLSS presets E and F, and setting DLSS up logs the memory
    it allocated (adapted from Lichii #1098).
  - `EDEN_NX2_MEMORY_PRIORITY=1` enables `VK_EXT_memory_priority` (and
    `VK_EXT_pageable_device_local_memory` where present) and gives render targets, buffers and
    staging different priorities, so Windows demotes old textures first when the process goes over
    its video memory budget. It costs one memory allocation per render target.
  - `EDEN_NX2_INDEX_SYNC_SKIP=1` skips the upload scan of an index range that no CPU write has
    reached since the last scan (adapted from Lichii #858).
  - `EDEN_NX2_PB_RESNAPSHOT_IN_TRANSIT=1` (with `EDEN_NX2_PB_RESNAPSHOT=2`, the default) also copies
    again the command lists on their way to a channel when it resumes. It is off because this code
    holds Nemyu's fix for Ocarina of Time's freezes and has not been tested yet.
    `EDEN_NX2_PB_RESNAPSHOT_AFTER=cpu` keeps the copies taken so far after a release the GPU wrote
    (`all`, the default, copies again). Adapted from Lichii 312886a860 and 1ac93b6d72.
  - `EDEN_NX2_JIT_BRANCH_CACHE=1` gives each indirect branch its own one-entry block cache. It costs
    16 bytes per compiled branch and its author measured only 0.997 and 0.994 of the JIT cycles per
    guest instruction (Pokopia world walk).
  - `EDEN_NX2_NATIVE_PROGRAMS=1` records each Switch 2 program the GPU thread lowers in the shader
    cache log, so the next boot lowers the recorded programs on the pipeline workers next to the
    pipeline builds and a first bind takes its program instead of lowering it on the GPU thread. It
    costs boot time and memory (the boot logs how many programs it lowered, in how long and how much
    memory they hold), and its gain is unmeasured: `EDEN_NX2_GPU_PROFILE` logs every 10 s the
    programs lowered on the GPU thread and what they cost (adapted from Lichii #1036).
  - `EDEN_NX2_SELECTED_WRITE_BOUND` bounds a written storage mark heuristically for command list
    words.
- Also in the log: the JIT's compiles every 10 s (`EDEN_NX2_JIT_STATS=1`: blocks, KiB of code and
  milliseconds, and how many were already compiled by another core, since each of the six cores has
  its own code cache), fastmem read faults and the loads they hit, the GPU thread's time in the 10 s
  async pipelines line split into builds, waits for worker builds and queueing, the builds that
  reused shaders, the decoder's name, CPU or GPU and thread count when a movie opens, the movie
  frames dropped, command list copies taken after submit (`EDEN_NX2_SNAPSHOT_TRACE`, which also logs
  channels that waited 50 ms or more on a semaphore), and entries parsed from memory that differ
  from their copy (`EDEN_NX2_LIVE_READ_CHECK`).
- **Update checker:** Nemyu checks its GitLab releases at start-up and from **Help > Check for
  Updates**. A newer release opens a window with its notes and Download, Remind Me Later (one day)
  and Skip This Version. The download is checked against the SHA-256 published with the release and
  a file that does not match is discarded; nothing is installed by itself. Experimental builds are
  offered experimental and normal releases, normal releases only normal ones, and builds without a
  release tag never check (adapted from Lichii #82, #504, #509 and #1101).
- **Switch profiles per game:** named profiles of Advanced Switches, edited beside the global
  switches and picked in a game's Properties (**Switch profile**). A profile applies on top of the
  global switches while the game runs and is undone when it stops; when it changes a switch read
  only at start-up after another game ran, Nemyu offers to restart into the game. `nemyu-cli`
  applies it too.
- **Prepare Shaders from the game's files** (opt-in): `EDEN_NX2_PRECOMPILE_COMPUTE=1` builds the
  compute programs found in the game's files and `EDEN_NX2_PRECOMPILE_GRAPHICS=1` its vertex and
  pixel programs for the render states the cache recorded, only inside Prepare Shaders (adapted from
  Eglantine #814, failaip12).
- `EDEN_NX2_BUILD_PRIORITY=1` (opt-in): background pipeline builds wait, for up to 2 s, while the
  pipeline workers have builds the game needs (after Lichii #1036). The 10 s pipeline log line also
  reports how long those waits overlapped background builds.
- `EDEN_NX2_TC_COMPACT=1` (opt-in): near the video memory budget, textures move out of partly used
  16 MiB memory blocks.
- The **Use NVIDIA DLSS** settings show which NVIDIA files are present and can fetch the missing
  ones from NVIDIA's Streamline release.
- Tests for the update checker, switch profiles, the build priority queue, kept pipeline pointers
  and 3000 randomly damaged shader cache logs (adapted from Lichii #1036's fuzz target).

### Changed

- **Shader cache log:** the Vulkan pipeline cache is now `vulkan_log.bin`, an append-only log of
  checked records (`EDEN_NX2_SHADER_CACHE_V2`, on by default; `=0` goes back to `vulkan.bin` exactly
  as before; adapted from Lichii #1036). A record has a checked header and a hashed payload. Reading
  skips a damaged record alone and goes on at the next intact header, a cut-off tail is left out,
  the latest record of a key wins and tombstones remove records. A writer thread appends in bursts
  and compacts the log at boot once a quarter of it is dead. Each record carries the cache version
  whose layout it has, so a version bump that only changes translation no longer empties the cache,
  and the caches the 32 to 33 bump set aside (`vulkan.v32.bin`) can be imported. OpenGL keeps
  `opengl.bin`.
  - Nobody loses a cache: the first boot of each game takes `vulkan.bin`, `vulkan_mesh.bin`, their
    set-aside copies of same-layout versions and `vulkan_used.bin` into the log, and those files are
    only ever read. If a file holds more than one record of a pipeline (a re-stored one), the latest
    comes in. Later boots take a file in again once its size or time changed (a Shader Manager
    import, a session with the switch off).
  - **A pipeline built with the log on lives only in the log.** Turning the switch off later goes
    back to `vulkan.bin` as it was, without those pipelines; a Shader Manager export with the log on
    writes them out as `vulkan.bin`, `vulkan_mesh.bin` and `vulkan_used.bin`.
  - The Shader Manager (it counts the log's pipelines, imports into the log and exports it), the
    compute warm-up and the game list's Remove see the log. Remove also deletes the set-aside
    `vulkan.vNN.bin` and `vulkan_mesh.vNN.bin` of same-layout versions, so the next boot does not
    take them into a fresh log again.
  - Two emulators running the same game no longer lose records to each other: appends and
    compactions take an exclusive lock on `vulkan_log.bin.lock` (a writer that finds it taken keeps
    its records queued for its next burst), and the Shader Manager keeps the lock file out of Clean
    Up. The boot lets go of the log's bytes once the records are read, which is tens of MB less held
    through the boot builds and Prepare Shaders for a large title.
- **Asynchronous shaders:** Switch 2 titles follow the Asynchronous shaders setting (off by default)
  instead of turning them on while the setting is at its default (`EDEN_NX2_ASYNC_SHADERS=0/1` still
  forces it). With it on, a draw whose pipeline is still building is skipped only when every target
  it draws into is cleared whole every frame, so textures a game draws once are no longer left
  missing (`EDEN_NX2_SKIP_GUARD=0` skips as before; Mario Kart World also lets depth-tested scene
  draws be skipped). Before a skip, a draw waits up to 3 ms and all draws up to 8 ms in 16.7 ms
  (50 ms, and 100 ms in 250 ms, in Ocarina of Time). A pipeline key that differs from a cached one only
  in vertex attributes the shader doesn't read reuses that pipeline (`EDEN_NX2_ATTRIBUTE_ALIAS=0`).
- A new graphics pipeline whose shaders were already translated for another pipeline, with the same
  environment answers and only state those shaders don't read differing, reuses their translation
  and SPIR-V; only the driver compile is left (`EDEN_NX2_SHADER_REUSE=0` translates again; adapted
  from Wael3rd 582c7b1bd3). With asynchronous shaders, the GPU thread no longer spends the skip wait
  on draws the skip guard won't skip, so the pipelines after them start building sooner.
- **The Legend of Zelda: Ocarina of Time** only runs with the built-in Max Resolution and Dynamic
  FPS patches, both on by default. The Occlusion Off patch is gone: the game should run well without
  patches that change what it does. Its movies skip the tensor network, which an input copy
  replaces; the UI storage is copied at each present while it runs a frame ahead, so the dialogue
  box and HUD no longer go missing in half the frames (Lichii #715, #721). Its title defaults now
  also check its uniform banks on every bind, write fence-flagged query reports after the host GPU,
  keep up to 256 MiB of deleted images for reuse, limit the texture collector to 4 ms a frame under
  memory pressure, count VMA's unused block space against the memory budget, and build shared shader
  functions as calls instead of inlining them (shorter driver compiles). It also invalidates
  deferred writes only over the bytes written, not the whole 32-byte blocks they touch
  (`EDEN_NX2_INVALIDATE_BLOCKS=0`; other titles keep whole blocks, and `=1` restores them for
  Ocarina of Time): the rest of a block can hold bytes the GPU wrote that guest memory does not
  have yet, and Fritchii saw glowing geometry in Hyrule Field with whole blocks (Fritchii
  c2d2f50d15).
- **Mario Kart World** submits its draws to the host GPU every 128 instead of every 4096 (as Lichii
  does for every title), so a CPU read of GPU memory waits for less work.
- Every Switch 2 title polls channels waiting on CPU-released semaphores every 50 us instead of each
  millisecond (`EDEN_NX2_PENDING_POLL_US=0` restores the queue wait; Lichii #446).
- The JIT: on x64 hosts, guest acquire loads are plain loads and load-only or store-only barriers
  emit no fence by default (`EDEN_NX2_X64_TSO=0` restores them), and Lichii #717's code generation
  work is on outside Paranoid accuracy: register allocation, smaller code, back-edge halt checks, a
  32-entry return stack buffer, a larger dispatch table, reused host flags, LSE atomics as host
  atomics and pause hints (each with an `EDEN_NX2_JIT_*=0` switch). Blocks that cannot have changed
  the FPCR pop the return stack buffer inline on a guest return, so each return has its own host
  indirect jump (`EDEN_NX2_JIT_INLINE_RET`, on again by default; `=0` jumps to the shared handler),
  and scalar floats stay in XMM registers (`EDEN_NX2_JIT_SCALAR_XMM`, on). Scalar FRINTA and FRINTX
  are emitted inline instead of calling soft-float, with the soft-float's results and FPSR flags
  (`EDEN_NX2_JIT_INLINE_FRINT`, on; blocks with FPCR.FZ, and half precision, keep the call). These
  three switches are on outside Paranoid CPU accuracy. Two IR changes need no switch: a rotate and
  a low mask become a logical shift right, and an add whose source stays live goes into a new register, with a 64-bit
  immediate that fits imm32 encoded in the instruction. The return, scalar float and IR changes are
  adapted from notsapinho's Eglantine, from the series Lichii #717 carried in part; their author
  measured 0.991 and 0.990 (return) and 0.974 and 0.963 (scalar floats) of the JIT cycles per guest
  instruction in a Pokopia world walk, and none was measured on Nemyu.
- The kernel spins briefly before sleeping on contended locks, address waits and idle cores
  (`EDEN_NX2_ARB_SPIN`, `EDEN_NX2_KSPIN`, `EDEN_NX2_CORE_LOCK`, `EDEN_NX2_IDLE_PAUSE`; Lichii #717).
- Less waiting on the GPU threads: command lists the GPU wrote are read from copies instead of
  draining the split, and a channel waiting for such a copy no longer stops the others (from
  zortos293's and Frizti's Lichii, #752); the texture collector writes evicted GPU-written images
  back without waiting for the whole GPU (`EDEN_NX2_TC_GC_ASYNC=0`); a deferred clear no longer ends
  and restarts its render pass (Lichii #699); adjacent pages of a GPU range flush in one call; large
  images with no guest memory are cleared instead of uploaded from zeros
  (`EDEN_NX2_TC_ZERO_CLEAR=0`); a pipeline builder a draw waits for is raised to high priority on
  Windows (`EDEN_NX2_PIPELINE_BOOST=0`).
- Less CPU work per draw and per read: the buffer cache's range sets are sorted arrays instead of
  boost interval sets (Lichii #481), stages with no storage or texture buffers skip those binders,
  pipeline keys compare their shaders first, the ASTC decoder works out each block's partition hash
  once, and NUP zstd members are decoded only as far as reads need (zortos #885).
- Switch 1 titles evict only the pipelines over `EDEN_NX2_PIPELINE_EVICT_ABOVE` (8,000), least
  recently used first, instead of every pipeline unused for 9,000 frames (from Fritchii).
- `EDEN_NX2_PIN_THREADS=1` (still opt-in) places the GPU threads through Windows CPU sets instead of
  a hard affinity mask (Lichii #418).
- `EDEN_NX2_TC_*_MIB` overrides are capped at the device's memory budget (Lichii #692), and
  `EDEN_NX2_SWAP_INTERVAL` does not apply while a video plays.
- Implicit Vulkan layers registered by Overwolf are disabled for Nemyu (Lichii #365).
- With `EDEN_NX2_TENSOR_COOPMAT=1`, AMD GPUs without matrix cores keep the software HMMA path, which
  faulted in AMD's driver (Lichii #657).
- The Data Manager lists its folders with their sizes on the left and labelled actions on the right,
  and opens the Shader and Save Managers. A game's properties show the section icons, and a game
  without box art fetches it while its loading screen is shown.
- The log is flushed at most every 250 ms (and at once for Critical lines) instead of on every Error
  line.
- On NVIDIA the driver's own shader disk cache is sized for Switch 2 games: Nemyu sets
  `__GL_SHADER_DISK_CACHE_SKIP_CLEANUP=1` and `__GL_SHADER_DISK_CACHE_SIZE` to 10 GiB for its own
  process, before Vulkan is loaded and only where the variables are not already set
  (`EDEN_NX2_NV_SHADER_CACHE=0` leaves the size to the driver). Fritchii measured the driver keeping
  about 33,000 of Ocarina of Time's 132,000 pipelines at its default size and silently dropping the
  rest, so places compiled again on the next run (from Fritchii 4012e5271e).
- **Mario Kart World video memory:** once the driver's usage, including the free space inside VMA's
  blocks, comes within 256 MiB of the budget, the texture collector evicts that excess instead of
  letting the driver page video memory out (`EDEN_NX2_TC_BUDGET_GUARD_MIB=256` for Mario Kart World
  only; the switch is off elsewhere, and `EDEN_NX2_TC_SLACK=1`, Ocarina of Time's value, takes
  precedence). On the RTX 3050 its images sat at the collector's critical line from the second race
  on while that slack and the buffer cache kept growing. Not built or run when it was written.
- **Movies:** AV1 is decoded by libdav1d at its own default thread count instead of FFmpeg's 4 frame
  threads (`EDEN_NX2_VDEC_DAV1D_THREADS=<n>` sets a count, 4 brings the old behaviour back; VP9 and
  the other decoders keep their 4). Decoded frames that the movie's clock has already overtaken are
  dropped when an output image comes free, so a game that presents below the movie's frame rate no
  longer falls further behind the sound (`EDEN_NX2_VDEC_PACE=0` turns it off). The clock starts at
  the first frame the game acquires; the end-of-stream frame is never dropped, nor more than 30
  frames in a row, and at full speed nothing is dropped. The decoder now creates only the hardware
  devices it can use, so a movie with no hardware decoder, such as Ocarina of Time's AV1 ones, no
  longer creates and discards each device first (a user log showed 1.7 s between its first movie
  packet and the CPU fallback). Adapted from Lichii #879, #912, #659 and #809.
- The HostTiming thread sleeps in the OS between events on x86 Windows instead of waiting in MWAITX
  or UMWAIT, which Windows counted as running (Lichii measured 99.7-99.8% of a core, and 1-4% with
  the sleep, with no change in fps; `EDEN_NX2_HOSTTIMING_SLEEP=0` waits as before, and
  `EDEN_TIMING_COARSE` does nothing while it is on; Lichii, with the timer and wake event from
  Eglantine #538, pretend7979). Pages of guest memory that hold only zeros leave the working set on
  Windows, which cannot punch holes in the section behind guest memory (Lichii measured about 7 GB
  of them in Ocarina of Time on a 16 GB PC with 0.2-0.6 GB free); a low-priority thread does a pass
  every 5 s while one trims 64 MiB or more, and otherwise once a minute (`EDEN_NX2_TRIM_ZERO=0`
  turns it off; a page used again costs a fault, a slow one if Windows has written it to the page
  file meanwhile).
- A series from Lichii #805 (jpkto) on command lists, uploads and barriers, mostly with a switch
  that is on by default and restores the old behaviour at `=0`:
  - command list pages on GPU-written memory wait only for the words GPU work wrote instead of the
    whole 4 KiB page (`EDEN_NX2_LIST_WORDS`); DMA copies and clears now retire a page's copy, and
    DMA clears no longer treat their word count as a byte count.
  - the split's front publishes to the back once it waits for work (`EDEN_NX2_PUBLISH_AFTER_WAIT`)
    and before it sleeps on suspended channels, and the back runs a guest read's flush after its
    current entry (`EDEN_NX2_BACK_TICK`); an idle back submits and sleeps while the front only polls
    (Lichii #949).
  - a run of in-order uploads is one command with one pair of barriers (`EDEN_NX2_UPLOAD_BATCH`),
    and uploads go ahead of a submission's work unless that work uses their bytes, where they used
    to break the render pass (`EDEN_NX2_UPLOAD_ORDER`).
  - small uniform buffers are compared in guest memory instead of through a scratch copy
    (`EDEN_NX2_UBO_INPLACE`, now on by default; `=0` brings the scratch copy back).
  - a compute dispatch's barrier is recorded in the dispatch's own command, and command chunks align
    their data for every command (no switch).
- Less CPU work per draw, continued: a disabled vertex stream is no longer translated (a third of
  the translations the split's back made in Mario Kart World races; Lichii 681342c9da, from
  Hydro74000) and fence operation lists are vectors instead of deques (Lichii #858).
- Build Shader Warm-up now reads every container the archive walker understands: loose `.bnsh`,
  uncompressed `.bfsha` and `.sarc`, and `.cgsb`, `.shn` and `.sharcb`, raw or with `.zs`, in any
  case. A title with such files gets a larger warm-up file after a rebuild, and the warm-up marker
  makes the next boot build the new programs once (Lichii #812).
- Titles that read the GPU clock in nanoseconds (Ocarina of Time by default) latch it with each
  present, so changing GPU Clocks (Boost or Overclock) mid-frame no longer moves a timestamp
  backward; it takes effect at the next present (Lichii #664).
- The **.nx** reader is nxformat 1.0.1 (fab918): a read can span blocks and directory lookups use
  binary search (Lichii #1078).
- **Ocarina of Time** takes four defaults from nemyu-coco (GPU-written compute grids dispatched from
  GPU memory, the conditional render report read on the back, one write per batch of fence
  downloads, a submit every 128 draws).
- **Xenoblade Chronicles DE, 2 and X (Switch 2 Editions)** use the legacy sector order (Orchid).
  Frames larger than the window are shrunk with an area average under FSR.
- Nemyu no longer reads `nx2_switches.txt` from Eden's data folder.

### Fixed

- **The Legend of Zelda: Ocarina of Time:**
  - It froze at 0 to 1 fps while loading and in the intro: command lists were copied again too late,
    when the game had already refilled the command ring for a later frame, so methods ran from
    unrelated entries and channels waited on releases that were never parsed. A list is now copied
    again only when its channel resumes, as Lichii and Fritchii do (`EDEN_NX2_PB_ACQUIRE_COPY=1`
    copies at every met acquire), and the rest of an entry is dropped at a header no GPU class has,
    since a method address is 12 bits (`EDEN_NX2_KEEP_MISREAD_HEADERS` runs those methods as
    before).
  - It froze at 0 fps about 40 s into some boots, with channels waiting for an acquire-equal whose
    word had already counted past the value. An acquire-equal now counts as met once its word
    counted from below the value to past it since the channel suspended (`EDEN_NX2_ACQUIRE_PASSED`,
    on; `=0` waits for the exact value). A word already past the value when the channel suspended
    counts after 2 s, only for values of 2 and up, so 0 and 1 flags still wait.
  - It froze collecting a blue rupee on an RTX 3050: a command list entry that starts with stray
    words is now read from its first valid method.
  - It, and Donkey Kong Bananza, crashed or hung on an RX 570 (GitLab #7 and #8) when a method was
    split before a called command list.
  - It froze after a while with Balanced or Accurate fences (Lichii #687).
  - White flashes on leaves and walls at GPU accuracy Low (fence-flagged reports now wait for their
    fence, Lichii #337), and one-frame flashes on leaves and Link's tunic (uniform banks checked on
    every bind, from Fritchii and Eglantine).
  - Water flickered every other frame (a texture handle that is a storage buffer address now binds
    no texture, from Fritchii).
  - The red dotted line in the Royal Family's Tomb on AMD (noperspective values are interpolated
    perspective-correct, Lichii #722). `EDEN_NX2_NOPERSP_AS_SMOOTH`, unset, follows the driver: off
    with NVIDIA's proprietary driver, where Fritchii saw the Hyrule Field drawbridge fog turn solid
    white on an RTX 4080 with it on, and on with every other driver; `=0` and `=1` force it. A
    geometry shader kept as a native passthrough (NVIDIA) keeps native interpolation (Lichii #820;
    Fritchii 9b30eec8ca).
  - Near hedges in Castle Town kept only their bare branches: programs can bind 64 storage buffers
    instead of 32, and compute programs translated while their pointers were null are translated
    again once they are set (Lichii #551).
  - Occlusion counts past 256 slots were zeroed; up to 1,024 are kept (Lichii #718). A run of
    exactly one slot more than a subgroup holds (257 slots with 32 lanes, 513 with 64) came out
    too low; its scan now adds the first subgroup's sum (Lichii #718).
  - Corrupt textures in the fairy cave from `.nup` dumps with damaged `.zs` members (Lichii #740).
  - Black blocks and a crosshatch on an RX 6750 XT, ending in a GPU fault: ASTC and unswizzle stores
    past the edge of an image whose size is not a multiple of the block size landed in other images'
    memory on AMD, and each store now checks the image's size (Fritchii f52631cf66, from
    CheezerWheezer's Lichii).
  - Descriptors ended up far past the buffers they bound and the GPU faulted: a deleted buffer is
    now dropped from the bindings of every channel, not only the bound one (Eglantine #794,
    A-Man-With-A-Plan-dev).
- Shader translation threads shared one static set of written clip distances, a data race (Lichii
  #902).
- A channel could deadlock on its own semaphore when the fence thread overwrote a GPU pusher flag
  (Lichii #479, #609).
- Splatoon Raiders' outdoor scenes were washed out: the upscaler stand-in decodes sRGB when the
  game's program does (from Eglantine).
- Orbitals' gameplay was black: geometry shaders with ALD and bindless gathers with offsets are
  translated (Lichii #742).
- An image that does not fit in video memory is left out of the cache instead of ending the process
  (zortos #342, #440).
- A swapchain was destroyed while its last present could still be queued (device loss on MoltenVK
  after a resize).
- AMD: a texture handle past the descriptor pool binds a 2D view instead of crashing the shader
  compile, and ASTC with uneven mip block counts is not re-compressed to BC1/BC3, which showed dark
  seams (Lichii #679).
- Shaders: atomic float min and max are NaN-defined on AMD (Lichii #432), and denormal controls are
  collected from FP32 min and max (#346).
- The JIT's x64 backend: 32-bit SDIV by zero returned the dividend, FRINTA rounded ties to even and
  FRINTX rounded up, SQSHLU did not saturate narrow lanes, and SM4 failed above 2 GiB (Eden #4598).
- A CPU read of a preflushed region saw stale data until its download landed (Ocarina of Time only,
  `EDEN_NX2_PREFLUSH_READ_WAIT`; from Frizti's Lichii).
- `EDEN_NX2_PB_CHECK` from `nx2_switches.txt` only half applied.
- **Mario Kart World:** karts sank below the water from the second water section on until a pause,
  because nothing wrote the 256x256 collision height map the game reads on the CPU back to guest
  memory (that image is now written back through the preemptive download path after its dispatch),
  and water was flat with no waves because the wave program reads a ground height map that is
  missing here (the program's depth factor is now 1.0 only when both inputs are exactly zero).
  `EDEN_NX2_MKW_WATER=0` turns both off, and no other program or title is touched (adapted from
  Lichii #690, from Eglantine #737, snowstormxd).
- JIT (x64): the inline return dispatch crashed Ocarina of Time at startup in testing (exception
  code 0xC0000409). Xbyak's strict operand size check rejected the qword operand of its final
  indirect `jmp`; `EmitTerminal` is noexcept, so the first block with a return called
  `std::terminate`. dynarmic is built with that check off, so its tests passed. The jump now uses an
  unsized operand, and every Xbyak user is built with dynarmic's checks so the emulator and the
  tests generate code under the same ones. `EDEN_NX2_JIT_INLINE_RET` was off for a while during
  testing and is on again (adapted from notsapinho's Eglantine 36a43cfed1). The inline FRINTN,
  FRINTP, FRINTM, FRINTZ and FRINTI set the guest's inexact flag (IXC) whenever the input had a
  fraction; only FRINTX reports it.
- A pipeline cache that was cut off while a record was saved (a crash or a forced close) made the
  next boot delete the whole file, so every pipeline the game had built was built again, and a
  damaged record in the middle made every record after it unreadable. A cached pipeline whose record
  lacks a value a newer translation reads was never stored again, so every later session built it on
  the GPU thread. `EDEN_NX2_CACHE_REPAIR` (on; `=0` deletes the cache as before) cuts the file off
  where the last complete record ends and stores such a pipeline again from the game's build. It
  covers `vulkan.bin`, `vulkan_mesh.bin` and `opengl.bin`; a record with an empty or oversized
  environment count counts as damaged either way.
- Pokopia's furniture placement guides were never drawn: the jump table clamp fold only matched UP0,
  and the guides use UP1 and UP2, so the indirect branch could not be tracked and the pipeline
  failed to build (Eglantine #781, kyabetsu4). A post-process compute that used a half-precision
  gather with fewer than four taps never dispatched ("Native QMD failed: native shader dispatch
  required") and the 3D scene stayed black; the Ampere lowering now packs only the taps the mask
  enables (Lichii #610, from Frizti).
- Native compute store extents are no longer too small where the analysis could not bound them: the
  whole binding is marked there, as before the extent analysis, so the CPU no longer reads stale
  bytes (Lichii #816). A join upload is skipped only for whole, unscaled, uncompressed copies, so
  edge texels no longer keep undefined data (Lichii #817).
- A guest CPU write into GPU-written memory could lose its bytes to the GPU's older ones: a download
  now publishes its landing and a write waits for one under way, and the writes a download keeps
  stay pending until they are applied (Lichii #805). Read traps are forgotten over a texture read's
  whole flush area, so a later GPU write through another device alias is trapped (Lichii #264). A
  reordered upload is ordered after an earlier in-order copy of the same bytes, and GPU work that
  read or wrote a buffer without a mark (indirect draws, obtained buffers, conditional rendering) is
  marked, so an upload no longer goes ahead of it (Lichii #805). Same-value instance stores no
  longer let a compute dispatch run before the draw it should follow (Lichii #488).
- An image that does not fit in memory, once every placement failed, was given an unbacked sparse
  image that AMD's drivers do not survive drawing into (an 8 GB RX 6650 XT in Ocarina of Time ended
  there). A texture cache image small enough to back is now left out until the collector has freed
  memory (`EDEN_NX2_TC_UNBACKED_FALLBACK=1` brings the old fallback back for every image; Lichii
  #880, from Eglantine #200, ZnOw01). A staging buffer that cannot be made frees every idle one and
  tries once more before the emulator ends (Fritchii saw it in Ocarina's town square after half an
  hour; Fritchii 464fbb45bb). A pipeline record with every stage (VertexA and VertexB, both
  tessellation stages, geometry and fragment) overflowed a vector of five and ended the emulator at
  every boot until the cache was deleted (Lichii #815).
- Windows: when the fastmem backing allocation failed the session ran without fastmem, which Lichii
  measured as 20 to 25% fewer fps in Ocarina of Time; it now commits the same size once (growing a
  system-managed page file), waits 50 ms and tries again, up to five times (Lichii #645, from
  Frizti). The manifest's `longPathAware` element sat under an unbound prefix, so Windows ignored
  it; paths past MAX_PATH also need `LongPathsEnabled` in the registry (Lichii #576, from Frizti).
  Linux: cold guest memory is marked with `MADV_COLD` instead of `MADV_PAGEOUT`, which wrote pages
  to swap at once and, in Lichii's reports, stopped the whole emulator for 3 to 15 s every few
  minutes on a machine with swap on a SATA SSD (Lichii #756).
- Malformed files fail to load instead of crashing or exhausting memory: PFS0 and HFS0 tables and
  names, RomFS tables (walked without recursion, loops rejected), XCI images without a secure
  partition, NCA FS headers, layer parameters and buffered tables, and NSP packages without a
  program loader; `ReadBytes` sizes its buffer by what the file holds (Lichii, branch
  `capy/filesys-harden-pfs0-romfs`). Two bugs that valid content can reach are fixed with it: every
  AES-XTS section opened as a null storage, and a short read past the end of the data in an
  integrity-verified layer wrote past the caller's buffer.
- `IDeliveryCacheFileService::Open` refused a file name with a period, so opening a delivery cache
  file with an extension failed (Lichii #561). `aud:u` command 0 returns the audio input manager and
  `IAudioRenderer` command 14 (SDK 23 and later) is accepted, so a Switch 1 title on a new SDK no
  longer gets an unimplemented-function error when auto-stub is off (Lichii #600, #497). SDL motion
  sensors that stop reporting (another program such as Steam Input changed their mode, or after a
  reconnect) are restarted after 1 s of silence, backing off to every 8 s
  (`EDEN_NX2_MOTION_RESTART`, on; the Qt frontend only; Lichii #710, #712).
- A load with no loader found crashed in the kernel's destructor instead of reporting the error
  (Eglantine #750, mw2mw3dirtshowdown-del). An upload into an MSAA image that did not start at the
  origin of its level was read from the corner of the scratch image (Eglantine #784,
  JuliusBairaktaris). `Log2Ceil(0)` returned a huge exponent, so a zero-size upload picked a staging
  size class far out of range (Lichii #918, from Frizti). A mapping of the last GPU page of the
  address space indexed one past the first page table level (Lichii c9df50563e). A manager made
  where a destroyed one stood could keep using its translations, as when a game is stopped and
  another booted (Lichii #805).
- `EDEN_NX2_CMDPAGE_DIAG`, `EDEN_NX2_DOWNLOAD_TRACE`, `EDEN_NX2_DRAW_RUNS`, `EDEN_NX2_3D_METHODS`
  and `EDEN_NX2_SEM_WATCH` did nothing when set in `nx2_switches.txt` (they were read before it
  loaded); the Advanced Switches page no longer locks `EDEN_NX2_3D_METHODS` and `EDEN_NX2_PB_CHECK`
  as environment-only (Lichii #714).
- An evicted graphics pipeline translated again kept no storage buffer pointers for slots that were
  null or unmapped at that moment, and read zeros through them until a restart (Lichii #1143,
  zortos293).
- A texture upload or copy is synchronized with every shader stage that uses it, and vertex buffers
  a draw keeps bound are not overwritten by a later upload in the same submission (Orchid,
  MythosOfs).
- **Custom RTC** did nothing or snapped back to the current time, and a past date turned into a far
  future one (Eglantine #808, Amnesia1000).
- Buffer ranges the GPU rewrote after a fence download's copy stay dirty (nemyu-coco).
- IMAD.MOV with a negated constant-buffer addend is lowered correctly, and Splatoon Raiders keeps
  its own image sizes at 1x under a resolution scale.
- A texture layer with no guest memory behind it is zero-filled instead of read out of bounds, a
  sparse block that is not one page of the image is rejected, and a queue no longer leaks popped
  values (Orchid).

## [0.1.7-experimental.3] - 2026-11-07

### Fixed

- The window froze ("not responding") when Prepare Shaders finished: stopping waited for the game
  to exit, but the game had been kept from starting. It now starts for a moment and is stopped
  right away.

## [0.1.7-experimental.2] - 2026-11-07

Changes since 0.1.7-experimental.1. Experimental builds are numbered until 0.1.7 is stable.

### Added

- **Prepare Shaders** in a game's context menu (and `nemyu-cli --prepare-shaders <game>`) builds
  every pipeline in the game's cache behind the loading screen, ignoring the used list, and stops
  before the game runs. Pipelines memory can't hold are built and released, which still fills the
  driver's own cache, so later boots and places already visited compile quickly.

### Fixed

- Stopping a game with ASTC recompression (BC1 or BC3) crashed: a texture still being compressed
  wrote into its decode context after the texture cache had freed it.

## [0.1.7-experimental.1] - 2026-11-07

Changes since 0.1.6 (34 commits). This release is dedicated to The Legend of Zelda: Ocarina of
Time's shader and pipeline compile stalls, with general fixes from other forks. It builds and passes
the unit tests, but the asynchronous pipelines, the compute "fast first" builds, the batched texture
collector and the stricter full-clear check have not been run in a game yet.

### Added

- New graphics pipelines are translated and built off the GPU thread when asynchronous shaders are
  on: what translation reads is copied on the GPU thread, a worker builds the pipeline, and the draw
  waits within the native budget (3 ms a draw, 8 ms a frame) before it is skipped. Finished builds
  are stored in the disk cache.
- New compute shaders met in game are built without driver optimizations first, then built
  optimized on a background thread and swapped in (`EDEN_NX2_COMPUTE_FAST_FIRST`: unset = AMD's
  proprietary driver in Switch 2 titles, `1` = any driver, `0` = never;
  `EDEN_NX2_COMPUTE_FAST_FIRST_MIN_KIB`, default 64).
- The log classifies every new graphics pipeline key as new shaders, a state variant or cached but
  not built, and every 10 s names the key fields that differed most (`EDEN_NX2_PIPELINE_DIFF=1` logs
  each variant).
- A device loss logs what `VK_EXT_device_fault` reports: the driver's description, the faulting
  addresses and the vendor records (from Zortos' Lichii #512).

### Changed

- Asynchronous shaders are on by default for Switch 2 titles while the setting is at its default
  (`EDEN_NX2_ASYNC_SHADERS=0/1` forces it).
- A pipeline cache of another Nemyu version is set aside as `vulkan.v<version>.bin` (three kept per
  game) instead of deleted, and restored when that version runs again (`EDEN_NX2_CACHE_DISCARD_OLD=1`
  deletes it).
- Cached pipelines survive driver updates: a record saved by another version of the same driver is
  loaded as this one (`EDEN_NX2_KEEP_CACHE_DRIVER_VERSION=1` restores the old behaviour; adapted from
  Spikol02's Nyamu).
- Pipelines prebuilt from the disk cache are no longer evicted before their first use (Lichii #550).
- The alpha test reference is rounded to 1/255 in the pipeline key, so near-identical keys share a
  pipeline (zortos293's Lichii, upstream #544).
- AMD GCN4 and older build at most 2 pipelines at once in Switch 2 titles
  (`EDEN_NX2_GCN_PIPELINE_WORKERS`, `0` removes the cap).
- The texture collector downloads the images it evicts behind one GPU wait per 64 MiB instead of
  one each (`EDEN_NX2_TC_GC_BATCH=0` restores the old behaviour).
- A frame pacing target is ignored while a video is decoding, so Ocarina of Time's intro movie runs
  at its own rate (`EDEN_NX2_PACING_VIDEO=0` keeps the target).
- Repeated asserts and log floods are rate-limited: the first 8, then each power of two, with the
  count (`EDEN_LOG_UNLIMITED=1` logs everything).
- Less GPU-thread overhead, from Orchid: `IsRegionGpuModified` stops at the first dirty query,
  `TickWork` skips its mutex when nothing is requested, DMA copies are waited for only where a fence
  download follows, and dirty CPU writes are gathered without a lock when there are none. Zeroed
  page-table pages stay committed (Lichii #485) and page walks are reused for shorter ranges (#580).

### Fixed

- A clear drops only the pending CPU upload of the target it fully overwrites, instead of every
  bound target's (`EDEN_NX2_CLEAR_KEEPS_UPLOAD=1` never drops one; adapted from Summit's Droplet).
- Decoded video frames are written only into an image that still exists (from Droplet), and
  `nx2_gsv` marks end of stream only on a picture from the last input (from Orchid).
- The JIT retries committing code space on Windows instead of emitting into an uncommitted page,
  which crashed at the system commit limit (Kaj, Lichii #310).
- Shaders: TFB varyings that start inside a generic output vector are captured (from Spikol02's
  Nyamu), and `TXD`/`ATOMG` registers are shadowed (Lichii #588).
- Vulkan: descriptor payload storage grows when a frame exhausts it (Lichii #495), framebuffers stay
  within the device limits and the stream buffer is padded (#427), the staging pool rechecks region
  0 after a wrap (from Orchid), and a rejected compute program is rethrown once per dispatch (#197).
- KeplerMemory and Fermi2D ignore methods past their registers instead of writing out of range.
- Open Save Data Location opens the folder the game actually wrote, which Ocarina of Time needs.

## [0.1.6] - 2026-11-07

Changes since 0.1.5 (88 commits). They were validated on one machine (an RTX 3050 laptop with 6
performance and 4 efficiency cores); the AMD and Intel fixes could not be tested there.

### Added

- `EDEN_NX2_INLINE_CNTPCT=1` computes guest counter reads in the JIT, and `EDEN_NX2_X64_TSO=1`
  emits plain acquire loads and no fence for load-only or store-only barriers; both opt-in (from
  Selevet Eglantine performance build 8, 999sian).
- The log names each compute shader at its first dispatch and every GPU wait of 500 ms or more;
  `EDEN_NX2_SKIP_COMPUTE=<hash>,...` drops dispatches of listed compute shaders (from Selevet
  Eglantine).
- **The Legend of Zelda: Ocarina of Time:** a built-in **Occlusion Off** patch, off by default and
  listed in Add-Ons (cLohan's mod, via sharkiix and Selevet's performance build 9).
- `EDEN_NX2_DMA_SNAPSHOT` (opt-in): macro DMA reads of pages already drained skip the next drain
  while nothing sent since may write them; `=2` checks every skipped read (adapted from Frizti's
  Lichii, with Evil's EvilOOT fix).
- `EDEN_NX2_PIN_THREADS=1` (opt-in, Windows) pins the Switch 2 GPU thread and the split's back to
  their own fast cores (adapted from Frizti's Lichii). It is off by default because it made Mario
  Kart World stall for seconds on the test laptop.
- Opt-in: a present queue with release fences (`EDEN_NX2_PRESENT_QUEUE=1`, from Eglantine PR
  #695), `EDEN_NX2_PIPELINE_CLEAN`, `EDEN_NX2_UBO_INPLACE`, `EDEN_NX2_NUP_CACHE_MIB`,
  fence-flagged reports written after the host GPU, unoptimised graphics pipelines and a pipeline
  compile log on AMD; the switches of chuppatricky's AMD build are listed in `nx2_switches.txt` and
  the Advanced Switches page.
- Texture descriptors that read as format 0 with an address are logged (the first 16), to find the
  stale table reads behind Ocarina of Time's GPU desync.

### Changed

- **The Legend of Zelda: Ocarina of Time:** the built-in Max Resolution and Dynamic FPS patches are
  on by default, and Dynamic FPS also applies when its hook or code area differs on a dump of the
  matching version, with a warning (from Selevet Eglantine). Eight sessions of used pipelines are
  kept (`EDEN_NX2_USED_SESSIONS`, from Eglantine and Lichii #161).
- A guest CPU write trap that finds the buffer or texture cache lock held is handed to the GPU
  thread instead of waiting (`EDEN_NX2_CPU_WRITE_DEFER=0` waits). In Mario Kart World races,
  frames over 100 ms went from 14 and 5 to 4 and 0 over two races.
- GPU thread work cut: empty direct draws are skipped by default (`EDEN_NX2_SKIP_EMPTY_DRAWS=0`)
  and unchanged vertex ranges are not scanned again (`EDEN_NX2_VERTEX_SYNC_SKIP`) (from heydemoura's
  EglantinePS5); `MustFlushRegion` answers without the buffer cache lock for pages the GPU never
  wrote (Lichii #413, zortos293); the split's front no longer sends instance-loop writes the back
  ignores (from Frizti's Lichii, tibzejoker #178); range subtractions that touch nothing are skipped
  (from Frizti); the quad index buffer grows without waiting for the GPU (Lichii #414, Hydro74000);
  the texture collector stops after 300 us a frame while memory is fine
  (`EDEN_NX2_TC_GC_BUDGET_US`, from Selevet); plain 2D block-linear textures are unswizzled a GOB
  row at a time (from Orchid); new ASTC data is scanned in place, and uploads a join copy overwrites
  are skipped.
- Cached background pipelines are built newest first (from Frizti), and the used pipeline list is
  saved every 15 s on the serialization thread instead of the GPU thread.
- Large cards fill 5/8 of their memory budget before the texture collector runs.
- The host timer thread runs at the same priority as the guest CPU and GPU threads, and the PAUSE
  between close counter reads is off on CPUs without SMT (from Selevet Eglantine, 999sian).
- The Turing macro RAM holds 0x8000 words.
- Shader cache versions were bumped; the first launch of each game rebuilds its caches.

### Fixed

- **The Legend of Zelda: Ocarina of Time:** trees keep their near leaves (loaded pointers that
  compete with a paired constant buffer pointer are bound, Lichii #410); terrain blotches from a
  deferred clear landing after a blit (Lichii #185); distant tessellated rocks (`SHF` register form
  shadowed); lowered `SHF.R.U64` funnel shifts no longer shift by 63 (from zortos293's Lichii);
  compute pointer-table pools are bound up to 256 MiB, and bindless handle words are read from the
  host-aligned storage base.
- **Donkey Kong Bananza:** no longer freezes at the first banana: QMDs in its shifted layout are
  accepted (from Anka, adapted from moceure).
- AMD and Intel Arc: storage float atomic adds are emitted natively, which fixes the Ocarina of
  Time hang in its auto-exposure shaders (`EDEN_NX2_NATIVE_FLOAT_ATOMICS=0`, from Selevet).
  AMD keeps `robustBufferAccess2` (Hyrule Field's sky turned white), waits once per occlusion
  query resolve and uses a pipeline barrier for wait-for-idle; Intel Arc fixes for Mario Kart
  World.
- Shaders: the `IADD32` negative overflow boundary (from Orchid, exploitizm), shared u8/u16 store
  CAS loops, more uniform-operand forms shadowed (from Selevet), the shuffle fallback for relative
  shuffles (from zortos293's Lichii), plain 2D gathers from a second texture buffer (from
  MateusAugustoBrito via Frizti), task programs' mesh workgroup count zeroed before they run,
  float Sampled Type for depth-compare images, compute writes ordered before later draws, and
  colour targets the fragment shader never writes are masked.
- Core: a lost wake when cancelling worker tasks (Lichii #290), virtual address ranges that wrap
  past the address space end are rejected (from zortos293's Lichii), and the TSC deadline of Windows
  Event waits saturates.
- The GUI says the decryption keys are missing when a game fails to load without them.
- The Linux build no longer breaks on Xlib's `Success` macro.

## [0.1.5] - 2026-11-06

Changes since 0.1.4 (388 commits). Most of them were validated on one machine (an RTX 3050 laptop);
the AMD fixes could not be tested there. The Yoshi and the Mysterious Book demo still hangs its GPU a few
seconds after boot.

### Added

**Games**

- **The Legend of Zelda: Ocarina of Time (Switch 2):**
  - Storage buffers behind dynamic and loaded pointers are bound (every mapped slot of a pointer
    array, chosen at run time), the vegetation shaders' pointer loads are resolved, and
    pointer-table pools are bound to their mapping up to 64 MiB, so the grass task and mesh
    programs find their instance records (from Lichii, Jonas and Duck912).
  - Multiplies by 0 and 1 are folded so the sky ambient compute binds its buffer (from Lichii,
    Duck912).
  - Mesh programs that output points or lines are translated (from Satsuma, SkiierCo).
  - The remaining compute forms after its culling pass are lowered: indexed `ULDC.U8/U16`, `ULEA`
    with a carry in `UP2` and up, `F2IP.U8` with two sources, `I2F.U8` of a uniform byte (from
    Lichii); `EDEN_NX2_LOWER_EXTENDED=0` rejects them again (adapted from Eglantine, kraibse).
  - Wide right shifts (`SHF.R.S64`, `SHF.R.U64` forms) and layered `SURED` are lowered (from
    Selevet, sylvieistaken).
  - A stand-in replaces its gameplay upscaler network by default: the network's matrix multiplies
    are rejected and a plain upscale squares the packed RGB9E5 frame; only the ten network
    programs are affected. `EDEN_NX2_OOT_NETWORK=1` emulates the network again (from Lichii,
    Felipe Lima).
  - Nanosecond report timestamps for Ocarina of Time only, which raise its dynamic resolution
    (from Lichii #84, as in Satsuma); `EDEN_NX2_GPU_TIMESTAMP=ns|ticks` forces a mode.
  - All native storage buffers are mapped from their mapping for this game only, and its 64 KiB
    native uniform banks are re-uploaded once per frame by default (from Lichii; kraibse,
    Franseven).
  - Outdoor haze is thinned by default as a temporary workaround while the cause is open
    (`EDEN_NX2_OOT_VISUAL_WORKAROUND=0` turns it off; scales `EDEN_NX2_OOT_FOG_SCALE`,
    `EDEN_NX2_OOT_EXPOSURE_SCALE`, `EDEN_NX2_OOT_BLOOM_EXP_SCALE` follow Selevet) (from
    chuppatricky's Lichii build, from zortos293).
  - Built-in **Max Resolution** and **Dynamic FPS** exefs patches, both off by default, shown in
    the game's Add-Ons (from Lichii, Frizti and zortos293; Selevet).
  - Fixed the hang at the next rupee pickup after changing the GPU accuracy mid-game: the macros
    go back to the engines when the GPU split stops (from Eglantine, kraibse).
- **Welcome Tour** boots: nx2:gsv 1121 gets a session (from Eglantine PR #630); decoder images the
  client maps for the GPU (gsv 426) are mapped (`EDEN_NX2_DECODER_IMAGE_MAP=0` leaves them
  unmapped) and written block-linear as RGBA (from Eglantine PR #166, Jonas); Switch 2 device
  ranges are shared between a process's address spaces (from Eglantine, Duck912;
  `EDEN_NX2_PRIVATE_DEVICE_RANGES=1` restores one range per address space). Its rendering is
  still broken.
- **Yoshi and the Mysterious Book (demo):** the split-method heuristic runs for the demo by
  default, held CPU writes are applied before GPU data is flushed for the demo
  (`EDEN_NX2_FLUSH_HELD_WRITES`, adapted from rdenno), split macro parameters are parked with
  their method, GPU-written code is flushed when native shaders are captured (from Eglantine,
  Sssnowyyy), decoded video frames refresh their GPU copy and an unmapped texture buffer no longer
  aborts (from Eglantine, rdenno). Diagnostics: `EDEN_NX2_SYNC_SUBMIT=1` waits for each Switch 2
  submission (rdenno); `EDEN_NX2_MAP_IN_ORDER=1` maps GPU memory before later command lists are
  read (from Orchid, Sssnowyyy), opt-in. The demo is not yet known to be fixed.
- **Fire Emblem: Fortune's Weave:**
  - Its DLSS evaluation (13 dispatches) is recognised (`EDEN_NX2_GAME_DLSS=1` follows the layout
    in any title), and **Use the game's own DLSS** (`game_dlss_network`, on by default, per game)
    chooses between the translated network and a cheaper plain upscale, with a status line (from
    Eglantine, Duck912).
  - **RCAS sharpness** for the DLSS stand-in (vendored MIT `ffx_fsr1.h`).
  - **Use NVIDIA DLSS in place of a game's DLSS** (`nvidia_dlss`, experimental, off by default,
    GeForce RTX only), drawn through NVIDIA Streamline. It is only built with the
    `ENABLE_NVIDIA_DLSS` CMake option, and the Streamline and DLSS files are not shipped (from
    Eglantine, Duck912).
  - `VSMS_MAPPING` is implemented, so NVN no longer tears down at boot (from Eglantine, duck912),
    and presents honour RGB10A2 images (from Eglantine, duck912).
- **Pokémon Legends: Z-A and other Switch 2 Editions of Switch 1 games:** upgrade packs in loose
  dumps (`dlc/<id>/<version>/romfs`) now load as add-on content (from Orchid, MythosOfs); a Switch
  2 Edition update's Switch 2 program is kept out of the Switch 1 update;
  `EDEN_NX2_COMPAT_PLATFORM=1` makes `nn::pl::IsRunningOnOunce` answer true (from Satsuma,
  SkiierCo), opt-in.
- **Pokémon Pokopia:** Joy-Con 2 mouse mode works: the PC mouse acts as player 1's right Joy-Con 2
  (Nmouse, hid 3139/3131) (archie). `ExtendSaveData`/`GetSaveDataSize` read the user id at its
  aligned offset (rdenno).
- **Donkey Kong Bananza:** DLC in NUP containers is served as add-on content (from Eglantine,
  Takoroka).
- **Cyberpunk 2077:** `REDUX.AND`, strong GPU `LDG`, register-handle `SURED` and 8/16-bit global
  loads (from Satsuma, SkiierCo).
- **FINAL FANTASY RESONANCE:** 32-bit global `ATOM.CAS` (from Lichii, Shrxmy), and `nemyu-cli`
  hands a game's `ExecuteProgram` restart to the launcher instead of hanging (from Lichii).
- **The Legend of Zelda: Tears of the Kingdom Switch 2 Edition:** movie and menu textures are no
  longer scrambled: descriptors with native TIC bit 88 are decoded, written back and DMA-copied in
  the legacy Tegra sector order, and images of the two orders no longer alias
  (`EDEN_NX2_LEGACY_SECTOR_LAYOUT`, on by default for this game only). Its remaining shader forms
  are lowered: `DFMA`, `DADD` and `DMUL` with register or constant-buffer operands, `F2F` between
  F32 and F64, `SGXT.U32` with a register bit count, and programs that end at their closing
  self-branch with only predicated exits (its cutscene video network) (from Eglantine PR #554,
  Alleexxi).
- **Star Fox:** no longer hangs on "Launching..." and then crashes: its window textures get the
  4 KiB alignment the SDK expects (this game only), and the upscale stand-in accepts its 1600x900
  RGBA16F to 1920x1080 RGB10A2 pair when its tensor upscaler is rejected.
  `EDEN_NX2_STARFOX_FPS=30..60` patches the tested build's frame loop to that rate; it is opt-in and only tested at 60 (from
  Eglantine PR #532, pretend7979; also covers the closed #556 by Mcm190).
- **Super Mario Bros. Wonder Switch 2 Edition** (named "wonder ns2e" in the original commit):
  `INx2FontService` (Alleexxi).
- **Fast Fusion:** G8R24 depth textures sample as D24S8 (from Eglantine, vibecodekun).

**Services & applets**

- Amiibo: Switch 2 extended application area commands 300-303 (the extended area itself is not
  read yet), v3 amiibo dumps (2048 bytes, NTAG I2C plus), and **Load/Remove Amiibo** keeps an
  amiibo on the reader until the game scans for it.
- The controller applet accepts GameCube controllers and reads arguments of any size safely.
- Opt-in `EDEN_NX2_REPORT_VRR=1` tells Switch 2 games the display supports VRR (from Selevet).
- Audio: hwopus command 100 (`OpenHardwareAudioCodecClient`) returns a client that decodes Opus
  packets with libopus instead of a missing interface, and sysmix streams (16-bit or float PCM,
  48 kHz, 1, 2 or 6 channels) are played through the output sink instead of being drained
  silently, which should give Nintendo Switch Sports its menu-movie audio (from Eglantine PR #532,
  pretend7979).
- Switch 2 services documented on Switchbrew (from Eglantine PR #611, Takoroka):
  - `chat:u` returns its objects and events (Mario Kart World calls them at boot), `GetRoomInfo`
    answers as outside a GameChat room, and the layout customization object hands back its event;
    events that never fire share one `StubReplyEvent`.
  - hid 220 `IsOunceVibrationDeviceMounted` answers whether the device is mounted; the scene id
    (138/139) and the Nmouse pointer ranges are read back per application; 213 logs its first
    request.
  - `IApplicationProxy` 40 returns `IPerformanceFunctions`, which keeps the title's CPU overclock
    request on the applet and also reports it enabled when the CPU clock setting is Boost or
    Overclock.
  - `IApplicationFunctions` 230 `IsTensorRtSupported` answers false, so titles do not turn on
    tensor upscaling that runs in software here; `EDEN_NX2_TENSORRT_SUPPORTED=1` answers true as a
    Switch 2 does (kept false by default in Nemyu).
- Each emulation session gets its own Switch 2 macro context, so a second game in the same GUI
  process no longer inherits the first one's macros.

**Input**

- DualSense HD rumble: with **Enable DualSense HD Haptics** on and a single DualSense connected
  over USB, HD rumble plays through its voice coil actuators instead of being collapsed into motor
  strengths; other setups fall back to standard rumble. The setting is off by default here (on
  upstream), because on one Linux PipeWire setup the signal came out of the controller's speaker
  (from Eglantine PR #624, Alex Arentz; synthesis after RenHDrum, MIT).

**Per-title switch defaults**

- `src/common/nx2_title_defaults.h` gives switches per-title defaults that follow the running
  program, also when the GUI or the home menu starts another game. Current rows:
  `EDEN_NX2_GPU_TIMESTAMP`, `EDEN_NX2_NATIVE_SSBO_MAPPED` and `EDEN_NX2_NATIVE_UBO_REUPLOAD` for
  Ocarina of Time, `EDEN_NX2_NATIVE_SSBO_MAPPED` for Mario Kart World. `EDEN_NX2_TITLE_DEFAULTS=0`
  ignores the table. Documented in NX2_NOTES.md.

**File formats (NUP, NSP, XCI)**

- NUP packer: **Convert to NUP** in the GUI and `--pack-nup` in the CLI, deterministic and
  resumable, with a metadata dialog (name, icon, version), a progress window and overrides
  (Jonas, kraibse).
- NUP extraction and metadata editing, including localized titles, with an icon search
  (switchiconshowdown.com, or SteamGridDB with a user API key) (SavageCore, kraibse).
- Update-only NUP containers boot against a companion base NUP in the same folder (a55uka); the
  base is found by peeking at each candidate's title instead of opening every container.
- A NUP's update and add-ons are listed in the game's Add-Ons, its display version shows in the
  game list tooltip (a55uka), the NUP tools are also in the **Tools** menu, and the NUP context
  menu entries are reordered (kraibse).
- LayeredFS mods apply to NUP titles (Dr. Sergio).
- Switch 2 programs in NSP and XCI files are detected before boot, so they get the 12 GiB layout
  and the other Switch 2 defaults (from Lichii, cira0100 and Paulius).
- The display version of loose dumps is also read from `GameDataList.Product.NNN` (kraibse).
- A NUP's en-us title is preferred over its default title (from Eglantine PR #660, Kabosune).

**UI**

- **Advanced Switches** page (Configure > Switch 2): a searchable, filterable table of every switch
  the emulator reads, with a tooltip each, Import/Export and Reset All; ticked switches are saved
  to the install's `nx2_switches.txt` (Brazilian Portuguese translation included).
- Game properties use Configure's layout: a list of entries with a dot where the game overrides a
  setting, a header with the game's cover, version, title ID, format and size, an Add-Ons tab that
  says what each add-on is (update, DLC, mod, cheats, built-in patch), and **Reset to Global** per
  tab.
- Monochrome line icons (Tabler, MIT) drawn in the theme's text colour in the main menus, the
  Configure side list and the game list context menus.
- **Pipeline preload at boot** (Graphics > Advanced): **Used pipelines** (default) or **All cached
  pipelines**, which builds every cached pipeline while at least 8 GiB of commit memory is left.
- Frame generation **Back Off When the Game Slows** (on by default); turning it off keeps
  generating through scene-to-scene frame rate swings (from Eglantine issue #395, soypilled).
- The performance overlay shows the emulator's RAM use and the machine's free and total RAM on
  Windows.
- The last eight logs are kept in `log/history`.
- With a resolution scale set, the status bar shows "Scale: Nx (frame at 1x)" when the last Switch
  2 frame shown was not rescaled: images written by compute shaders are scaled back down, and
  Switch 2 titles do much of their frame in compute. The Internal resolution help says the same,
  and NX2_NOTES.md describes what scaling does for these titles.

**Graphics & GPU**

- Remaining cached pipelines are built in the background on low-priority threads for Switch 2
  titles (`EDEN_NX2_BACKGROUND_PIPELINES=0` off, `=1` every title), and task and mesh pipelines
  are now stored in the disk cache (adapted from Frizti).
- HMMA can run on `VK_KHR_cooperative_matrix` with `EDEN_NX2_TENSOR_COOPMAT=1`, opt-in.
- Opt-in experiments from other forks: `EDEN_NX2_INLINE_DEFER=2` and `EDEN_NX2_HELD_WRITE_BATCH=1`
  (from eden-fefw, xDarkWindx); `EDEN_NX2_FENCE_FLUSH_US=<us>` batches fence submissions on the
  split's back (from Eglantine PR #494, rdenno); `EDEN_NX2_SELECTED_WRITE_BOUND`,
  `EDEN_NX2_BUFFER_SKIP`, `EDEN_NX2_INLINE_INVALIDATE` and `EDEN_NX2_DYNAMIC_SKIP` with
  `EDEN_NX2_FAST_DRAW_VERIFY` (from Lichii, zortos293; used by Selevet's builds);
  `EDEN_NX2_FLUSH_DRAWS`, `EDEN_NX2_CMD_BATCH`, `EDEN_NX2_HIGH_PRIORITY` (from Selevet);
  `EDEN_NX2_SKIP_EMPTY_DRAWS` (from Lichii); `EDEN_NX2_VDEC_EOS_DRAIN` (from Lichii PR #22, Radu
  Ursache); `EDEN_NX2_DRIVER_AMD=1` takes the AMD driver's paths on any card (notsapinho);
  `EDEN_NX2_INTEL_CONDITIONAL_BARRIERS` keeps barriers on Intel's Windows driver.
- Nx2Perf switches (`EDEN_NX2_PERF_OPT`, `_PERF_OFF`, `_PERF_ON`, `_PERF_VERIFY`) gate the
  GPU-thread optimizations ported from Lichii; `fragment_barrier`, `async_pipelines` and
  `split_overlap` (also Selevet's `EDEN_NX2_DMA_RANGES`) are opt-in (from Lichii, zortos293; 999sian
  PR #161; Selevet). `split_overlap` is marked unsafe in `nx2_switches.txt` and the Advanced
  Switches page, after Lichii removed the same narrowed MME DMA read wait in its PR #184.
- More opt-in Nx2Perf names: `front_sleep` lets the split's front sleep on an atomic during long
  waits for the back instead of yielding in a loop (from Lichii PR #184), and `forward_reads` lets
  a front macro read take its words from a constant buffer upload still queued for the back (from
  Frizti), both through `EDEN_NX2_PERF_ON`.
- `EDEN_NX2_PREFLUSH=0` (opt-in) makes CPU reads of GPU-written buffer data flush on demand instead
  of marking the pages Preflushable for the rest of the session, downloading earlier read ranges in
  the same wait (from Lichii PR #184).
- With `EDEN_NX2_ACQUIRE_QUEUED`, native compute semaphore releases are noted too, so an acquire
  they meet no longer drains the split (from Frizti).
- Diagnostics: `EDEN_NX2_TABLE_POINTER_TRACE`, `EDEN_NX2_PENDING_WRITE_TRACE`,
  `EDEN_NX2_FLASH_WATCH` (from Lichii, Frizti), `EDEN_NX2_DOWNLOAD_TRACE`,
  `EDEN_NX2_QUERY_DMA_TRACE`, `EDEN_NX2_DRAW_RUNS`, `EDEN_NX2_CMDPAGE_DIAG` (from Lichii,
  zortos293), `EDEN_NX2_VDEC_TRACE`, and every split-front wait site is named in
  `EDEN_NX2_GPU_PROFILE` (from Satsuma).

**CPU & core**

- dynarmic A64: `SQRSHL`/`UQRSHL`, scalar `SQRSHRN`/`SQRSHRUN`/`UQRSHRN`, `SQDMLAL`/`SQDMLSL` and
  scalar `SQDMULL` (LucyferDev).
- CPU switches from Selevet, all off by default: `EDEN_CPU_ACCURATE_FLOAT`, `EDEN_CPU_FUSED_FMA`,
  `EDEN_CPU_GLOBAL_MONITOR`, `EDEN_CPU_FASTMEM_CHECK`, and `EDEN_TIMING_COARSE` for the timing
  thread.
- The host clock calibration is logged at boot (Gavin Schiller).

**Build & tools**

- Optional tuned Windows builds: `build-pgo.ps1` with MSVC LTCG, clang ThinLTO and clang PGO
  (generate, merge, use) in separate build directories; training builds write the PGO profile
  every 30 s.

### Changed

- **Settings reorganised:** settings are placed from a layout table and relabelled consistently;
  expert groups (CPU internals, frame generation tuning, GPU synchronization, pipeline internals,
  ASTC decoding, presentation, Graphics > Fixes, Advanced Switches) stay hidden until **Show
  advanced settings** is ticked, unless one of their settings was changed. Storage folders are
  gathered on **System > Storage**, the game list cache, Discord and box art options moved to
  **General > Interface**, and **Disable web applet** moved to **System > Applets**.
- The controller applet and input settings use the Switch 2 drawings and names (Joy-Con 2 Pair,
  Joy-Con 2 (L)/(R), TV mode, Handheld mode), with larger player boxes and the Joy-Con 2 buttons
  placed as on the hardware.
- Audio backend and device lists show readable names (Auto, Cubeb, SDL3, None).
- The low memory warning says it is about system RAM and the page file, not VRAM.
- Screenshots are saved to the configured folder without a dialog by default (CloudP4bbs).
- Switch 2 pipelines are no longer evicted after 9,000 unused frames while memory allows
  (`EDEN_NX2_EVICT_PIPELINES=1` always evicts).
- Native storage buffers: written compute bindings are sized from their mapping (fixes Donkey
  Kong Bananza's first cave switch plate), Ocarina of Time maps all of them, and Mario Kart World
  keeps descriptor sizes (21-22 ms a race frame against 28-31 ms). `EDEN_NX2_NATIVE_SSBO_MAPPED`
  forces a mode.
- Idle cores spin 100 us before blocking in Switch 2 titles on hosts with 8 or more threads
  (`EDEN_NX2_IDLE_SPIN_US`) (from Lichii).
- Audio renderer voices are no longer dropped for Switch 2 titles (`EDEN_NX2_VOICE_DROP=1`).
- Storage reads are not paced to 40 MB/s for Switch 2 titles (`EDEN_NX2_READ_THROTTLE=<MB/s>`)
  (notsapinho).
- The guest RAM of a Switch 2 game is released when it stops (`EDEN_NX2_RELEASE_GUEST_RAM=0`)
  (from Eglantine, SmileyTatsu).
- With a system-managed page file, guest memory commits are retried while Windows grows it
  (from Selevet).
- Selevet's switch meanings are followed: `EDEN_NX2_ACQUIRE_QUEUED=1` covers every acquire (the
  old gequal-only level is `=gequal`) and also SEM_EXECUTE acquires, and `EDEN_NX2_ASYNC_SKIP` is
  read for native pipeline waits.
- `EDEN_NX2_PBFLUSH=1` now flushes only parameter blocks on GPU-written pages; the old behaviour is
  `=2` (from Satsuma).
- Guarded uniform arithmetic is accepted again after the Lichii frontend dropped it, checked after
  emission (`EDEN_NX2_UNIFORM_GUARDS=0`).
- `EDEN_NX2_DEVICE_RANGE_CHECK` (release destroyed memory's device ranges, from Satsuma) and
  applying held writes before a read flush (`EDEN_NX2_READ_FLUSH_HELD_WRITES=1`, from eden-fefw)
  are opt-in until compared.
- **Use NVIDIA DLSS in place of a game's DLSS** says in its help that it only acts in games that
  run their own DLSS (Fortune's Weave so far) and does not raise the resolution of others, and the
  log says once per session when 1800 frames passed without a DLSS evaluation to replace.
- Switch 2 service commands carry Switchbrew's names: hid, the audio sessions
  (`nn::audio::devmgr` and `nn::audio::sysmix`), the AS ioctl `NVGPU_AS_IOCTL_GET_SYNC_RO_MAP`, and
  the gsv UserSession, GpuSession, KUserWorkerSession, SyncptSession and video decoder commands.
  Renames only (from Eglantine PR #611, Takoroka).
- Shader cache versions were bumped again; the first launch of each game rebuilds its caches.
- The README was rewritten to present Nemyu as a general Switch 2 emulator, with compatibility
  levels, measured performance and requirements.

### Fixed

**AMD and Intel**

- Maxwell min, max and saturate use NaN-ignoring instructions on AMD (black screens in Ocarina of
  Time) (from sharkiix's Lichii version).
- AMD's Windows driver crashes compiling some pipelines in parallel in Switch 2 titles, so they
  are serialized there (`EDEN_NX2_AMD_PARALLEL_PIPELINES=1` keeps them parallel), and its position
  input issue is worked around (from PokeDinner).
- AMD graphics shaders avoid dynamic subgroup shuffles, which crashed `amdvlk64.dll` in
  `vkCreateGraphicsPipelines` (Eglantine issue #618) (from leadaenima's Lichii and Orchid;
  `EDEN_NX2_AMD_SHUFFLE_LOOP`).
- Nested epilogues counted before demotes are reordered, depth comparison dropped from samplers
  used by plain samples, and blend state refreshed at every native draw without dynamic blending
  (Ocarina of Time's forest fog on AMD) (from sharkiix's Lichii version).
- Wide-warp emulation is skipped in compute built with 32-lane subgroups (sharkiix).
- Storage and shared CAS loops (atomic INC/DEC, F32 add, emulated 8/16-bit stores) compare against
  the value the failed exchange returned instead of reloading the word, which could spin until the
  device was lost on AMD (an RX 9060 XT in Ocarina of Time) (from sharkiix's Lichii version).
- 8- and 16-byte storage accesses are split into words when the device's storage buffer alignment
  is smaller than the access (4 bytes on AMD's drivers and RADV): the vector index could land one
  or more words off, which gave Ocarina of Time green light and a cyan sky on AMD. Global accesses
  from sharkiix's Lichii version; the same fix for accesses the storage buffer pass rebases is
  Nemyu's. NVIDIA and Intel shaders are unchanged.
- Geometry shaders stay within `maxGeometryOutputComponents` on hosts without NV pass-through
  (from Orchid, MythosOfs; `EDEN_NX2_GS_PRUNE_OUTPUTS=0`).
- Conditional barriers are no longer stripped on Mesa ANV (Mario Kart World's froxel lighting)
  (Andreas Heider).

**Graphics & GPU**

- Mario Kart World's black screen at the start of a Grand Prix: decoder release fences complete
  on Stop and Finalize (from Eglantine, Javier, and PokeDinner, Franseven), and completed fences
  no longer advance the syncpoint again.
- Movies decoded on the host GPU (Mario Kart World's pre-race movie, Hyrule Warriors' cutscenes)
  no longer show green (Jonas); the scaler's colour details are set for NV12 too (from Orchid);
  a failing hardware decoder falls back to the CPU decoder (from Lichii PR #161, 999sian); decoder
  frame threads are no longer disabled by the low-delay flag (from Eglantine PR #488, ymatrix1).
- Phis of every IR type are emitted (four Donkey Kong Bananza world shaders) (from Eglantine,
  CloudP4bbs).
- Query results are flushed before split-front MME DMA reads (Mario Tennis Fever's hidden
  characters) (from Orchid, MythosOfs), and the split's back is waited for once per such read.
- A split instance loop continues at its own instance index (from Eglantine, Sssnowyyy).
- A pipeline that a draw or dispatch waits on is built first (`EDEN_NX2_BUILD_WAITED_PIPELINES=0`)
  and every waiter is woken when a graphics pipeline finishes (from Lichii, zortos293); draws
  recorded after a pipeline that failed to build are dropped (from Lichii PR #142, Felipe Lima).
- Alpha-to-coverage is recorded again when the bound pipeline changes it (`EDEN_NX2_A2C_REFRESH=0`).
- The occlusion counter is paused, not reset, at each submission (from Lichii PR #121, Kaj), and
  occlusion queries begin only inside a render pass (notsapinho); query copy destinations stay
  disjoint and the dynamic vertex stride is declared with dynamic vertex input (from Orchid).
- Large images: an image's guest size and `TryFindBase`'s address delta are 64-bit (Mario Kart
  World's 5.8 GB terrain array) and images of 4 GiB or more stay off the whole-image upload
  (from Eglantine, Sssnowyyy and A-Man-With-A-Plan).
- Texture handles are read as header offsets only for Switch 2 (from Eglantine, Sssnowyyy); live
  descriptor reads are bounded (from Orchid, MythosOfs); depth-compare descriptors stay float
  (from Orchid); ETC2/EAC formats added to `TexturePixelFormat` (notsapinho).
- Constant buffer words past the buffer end are dropped for Switch 2 titles (from Eglantine PR
  #412, ZnOw01).
- CPU-GPU coherency: `FlushCaching` runs before `DrawTexture`, stale buffer ids no longer reach the
  LRU and page table, GPU data is not downloaded over newer CPU writes, each buffer's readback is
  clamped to its own range, multi-range segments are rebuilt every pass, scattered DMA ranges are
  copied page by page, and the usage tracker handles short and unaligned ranges (from eden-fefw,
  Eglantine, Orchid, Lichii and Satsuma; Duck912, MythosOfs, godpow, zortos293, SkiierCo).
- Rejected native programs are looked at again when their bytes change (`EDEN_NX2_REJECT_RECHECK=0`)
  (from Orchid).
- Fences are released again while an acquire waits at high GPU accuracy.
- Composite is serialized between the GPU thread and syncpoint actions, which now run outside the
  syncpoint guard (from Orchid).
- An undecodable render target format logs and falls back instead of guessing a pixel size, and
  the software blit refuses undecodable formats (Paulo Rossatto).
- The shader recompiler is told when a program is native, so the lowered native forms are only
  taken for Switch 2 programs.
- Invalidated shader metadata is reclaimed only once renderer-side bindings are cleared (from
  Orchid, MythosOfs, adapted from ZnOw01's Primrose), and the uniform cache hit history is shifted
  with `copy_backward` instead of an overlapping forward copy (from Orchid, from
  zortos293/Eglantine).
- A one-page usage range checks only the queried blocks (from Eglantine PR #462, failaip12), and a
  clear deferred to a render pass load op starts no occlusion query (from Orchid, from
  zortos293/Eglantine).
- Unwritten Turing MME registers read as zero in the interpreter (Christian).
- Teardown: the renderer is destroyed before the GPU channels and scheduler, and the state tracker
  is detached before the final submit (Orchid; SmileyTatsu).
- The pipeline cache starts empty when the driver rejects the on-disk cache, and a bad saved
  Vulkan device index falls back to a real GPU (Erick; MythosOfs).
- A pipeline whose build throws no longer ends the process: a graphics build that fails for any
  reason wakes the draws waiting for it (from Lichii PR #184), graphics and compute pipeline
  creation catch any `std::exception` and skip the pipeline, and loading the pipeline cache stops
  at a bad entry and keeps the file (from sharkiix's Lichii version; seen in Ocarina of Time
  cutscenes).

**Services, applets and input**

- The HOME menu applet no longer crashes the `am:EventObserver` thread when emulation is stopped
  while a winding applet (MyPage after the profile selector) is still around; a winding applet is
  not relaunched after exit was requested, and a pruned applet stays alive until its lock is
  released.
- The profile selector returns the user that was selected, also with arrow keys and controllers.
- Software keyboard: no longer moves the mouse pointer, honours a game's disabled cancel, and trims
  long pastes at once; overlay dialogs handle controller input on the GUI thread; both texts of a
  custom error are filled correctly.
- Amiibo settings applet says why it closes without an amiibo and lists the right game names.
- Audio: managers are registered by owner and every output manager is called (Yuri Olive), AudioOut
  sessions are inserted under the manager mutex, long-lived managers are shared with `aud:u` and the
  process handle is checked before a renderer session id is taken (Sssnowyyy), and renderer systems
  are added under the list lock (failaip12).
- Mii `BuildRandom` reads age, gender and race as 32-bit words (rdenno).
- hid skips unassigned applet slots, and SDL's joystick lock is taken before the joystick map lock
  (failaip12).
- Switch 1 error applets opened by a Switch 2 title (Kirby Air Riders) no longer take the Switch 2
  channel state (Jonas); `ClearEvent` with a non-event handle is logged (from Selevet).
- Switch 2 GPU memory: map and unmap work, and a replaced backing's release, are queued after the
  registry's lock; every semaphore region is kept, not only the first 32 (jojo-chaechae's fix,
  Eglantine issue #392); 216 sparse request range checks are overflow-safe (Sssnowyyy); a write to
  a sparse block that got no memory is dropped instead of asserting (godpow).

**CPU, kernel and file system**

- Data races and undefined behaviour found with UBSan in timing, perf stats, file logging, Opus
  headers and `VectorVfsFile::Read`; the pause end time is published before the timer resumes; the
  exit lock and NVDEC flags are atomic (failaip12).
- A fiber no longer holds a weak reference across a switch (Sssnowyyy); GPU dirty memory gathers are
  serialized (from Lichii PR #138, Shrxmy).
- x64 calls only use rel32 within a signed 32-bit distance (from Orchid, Jonas); page table block
  ids no longer wrap after 65535 mappings (from Eglantine, Franz); the 64-bit `WaitIfEqual` read
  rejects wrapping addresses (from Eglantine PR #613); dynarmic initializes `committed_size` before
  the constant pool (from Orchid).
- Each kernel memory manager sizes its reference counts to its own region (exploitizm).
- `OpenDataStorageWithProgramIndex` falls back to the loaded RomFS for NUP and loose dumps
  (Walnutoast830); LayeredFS/ExeFS patch dirs are guarded against missing mod roots (SavageCore);
  a failed ZBIC decompression fails the NSO load (janvitos); mixed-case loose RomFS directories are
  found on Linux (snowstormxd).
- NUP robustness: large zstd members are decompressed once, tables with wrapping bounds, cyclic
  directories or directories listed by two parents are rejected, a contentless ExeFS counts as
  absent, `main` and `main.npdm` are validated on the same ExeFS, the platform is detected from the
  update ExeFS, the source dump name stands in for a missing title, and editing a NUP no longer
  leaves the GUI stuck or deletes non-empty directories (Sssnowyyy, sagaopc, a55uka, Aidan
  Kinzett, Mcm190, MythosOfs, SavageCore, kraibse).
- `nemyu-cli` registers a booted XCI/NSP's contents, so its control data is found.
- Per-game values are kept when the global config is read again (999sian); nested config arrays
  assert against the innermost array (Dr. Sergio).
- The game list stops directory walks when asked and reapplies its search after a repopulate
  (rdenno); the performance overlay can be dragged on Wayland (Matt Artist).

### Performance

- GPU thread (mostly from Lichii's Ocarina of Time work, zortos293): `MaxContinuousRange` answered
  from a run cache keyed by small page and walked only as far as asked (`EDEN_NX2_RUN_CACHE`),
  storage buffer bindings kept until the page table changes (Matt Artist), native uniform banks
  uploaded without marking them (Matt Artist), unchanged words skipped in `ForEachModifiedRange`
  (`EDEN_NX2_SKIP_CLEAN_WORDS=0`), repeated index binds and refilled usage pages skipped, cheaper
  Maxwell3D method dispatch on the split back, sorted page runs taken as they are, large odd-pitch
  layers unswizzled half on a texture worker, and the upload of a new render target that a clear
  overwrites skipped (`EDEN_NX2_CLEAR_SKIP_UPLOAD=0`, from Lichii PR #141, Kajcsi).
- `EDEN_NX2_TC_MIN_AGE` applies only while memory allows (sharkiix).
- Shaders: HMMA A operands broadcast within the quad with each K8 group unrolled (sharkiix); global
  atomic `SMin`/`UMin`/`SMax` and the remaining 32-bit global atomics routed to storage buffers
  (sharkiix; from Eglantine, kraibse); per-tap gathers with run-time offsets issue one gather per
  tap and unproven offsets are read at run time (`EDEN_NX2_PTP_RUNTIME_OFFSETS=0`); `RED` and
  `SURED` data registers are shadowed; immediate `UIADD3` carries into any `UPn` (Satsuma); the
  storage buffer pass keeps If scopes as a tree (Kirby Air Riders compute, from Selevet).
- MME DMA words are read in one block and image downloads wait for the GPU once per batch (from
  Eglantine, Yuri Olive); descriptor reallocations eliminated and buffer synchronization
  fast-guarded (Yuri Olive).
- Kepler compute no longer collects inline uploads the native path never reads (from Orchid).
- Shader builds read each texture descriptor once per handle, and without the flushing read that
  took the texture and buffer cache locks and could wait for the GPU, which stalled translations
  for seconds next to a busy GPU thread.
- Intrusive container member offsets are cached on MSVC (1.4% of emulated CPU time) (notsapinho).

### Tests

- New and updated tests: native loaded pointer bindings, layered SURED and NUP re-editing,
  batched page updates in the memory tracker mock, the cemuhook test server bound to loopback,
  Maxwell SHR translator IR (MythosOfs), clamped SHR count masking, VectorVfsFile end-of-file
  reads (failaip12), zero-duration event signals (MythosOfs), usage tracker masks, and the MSVC
  `FindBestUpdatePackage` comparison (Daniel Cabral). With the memory tracker mock, cemuhook and
  SURED/NUP test fixes (in part from Orchid batch 8), the full test suite passes.
- Tears of the Kingdom texture, lowering and capture tests (from Eglantine PR #554), and a test
  that the CAS loops carry the exchange result.

### Build & tools

- `NOMINMAX` is defined for Windows clang-cl builds (from Lichii), and collected range lengths
  match the tracker's size type so macOS builds compile (from Lichii #193).
- Non-Windows event wait deadlines saturate instead of overflowing (MythosOfs).

### Docs

- The README was refreshed and then rewritten (see Changed); NX2_NOTES.md describes the per-title
  switch defaults; `dist/nx2_switches.txt` and the Advanced Switches page list every new switch,
  including the changed meaning of `EDEN_NX2_PBFLUSH=1`; `docs/UPSTREAM_CREDITS.md` credits each
  batch of ports.

### Removed

- The controller applet's PNG icons, replaced by the drawn previews.
- Two changes were made and reverted on this branch: measuring the TSC only on CPUs without
  CPUID leaf 0x15 (it moved Skylake to Comet Lake CPUs to the slow OS clock), and a single page
  descriptor read that duplicated an existing fast path.

## [0.1.4] - 2026-11-05

Published on GitLab (219 commits since 0.1.3). Shader caches are rebuilt on the first launch
(cache version 28), so the first minutes of each game stutter while they compile. Known issues at
release: Ocarina of Time's fog and later areas are unverified, and Pokémon Pokopia, Welcome Tour and
the Yoshi demo were not fixed in this release.

### Added

**Games**

- **The Legend of Zelda: Ocarina of Time (Switch 2)** boots and renders its intro and first
  scenes: the 64-bit `WaitIfEqual` address arbiter wait (from Eglantine PR #567, Zortos), AV1
  movies, H.264 packet conversion, and its instruction forms, depth gathers and per-tap offsets
  (adapted from Lichii `fix/oot-bloom` and `fix/oot-fog`, Zortos). Fog and later areas are not
  verified.
- **Mario Kart World frame rate** setting (Graphics > Advanced, per game): **Dynamic** (default)
  patches the game's frame-rate mode at launch so it keeps normal speed down to 30 fps, **Lock
  30 FPS** forces its 30 fps mode, **Off** runs it as released. `EDEN_NX2_MKW_FPS_MODE` overrides
  it. Dynamic is not applied when the guest clock does not follow real time (reverse engineering
  and patch by ZendNevs, through jojo-chaechae's fork and PokeDinner).
- **Fire Emblem: Fortune's Weave:** AV1 movies (from Eglantine, Takoroka, with an FFmpeg 9.0.2
  build that has libdav1d on Windows and Linux x86-64), its native software keyboard layout
  (0x160012) (from Eglantine, Duck912), and the sky cube (`FSEL` operand modifiers, from Eglantine,
  Duck912).
- **Pokémon Pokopia:** software keyboard revision 0x140010 decoded natively (from PokeDinner,
  Franseven) and `F2I` of absolute or negated constants (its shadow pass).
- **Hyrule Warriors: Age of Imprisonment:** `F2I` from signed or high F16 halves, `TLD`/`TLD4` with
  half-precision results (gameplay was black) and a guard against garbage vertex or index counts
  (from Eglantine, rdenno).
- **Mario Kart World photo mode** in docked mode: `HSETP2` register form (from Eglantine,
  Takoroka).
- **Splatoon Raiders:** `TEX .F16` results packed in the right order (the scene was yellow) (from
  PokeDinner, Franseven).
- Other titles: Nintendo Switch Sports reports a typical six-axis sensor when the IC information
  is empty (from Satsuma, SkiierCo); Mario Tennis Fever's X8Z24 depth (from Eglantine, rdenno);
  four-word QMD releases (Cyberpunk 2077) (from Satsuma, SkiierCo); compute forms of the Yoshi
  demo, The Duskbloods and Star Fox (from Lichii, Zortos).

**Shaders**

- Lichii's Ampere frontend (program capture, geometry exit, register pressure, trap guards) and
  its shader recompiler changes are adopted and merged with Nemyu's (Zortos and Eglantine
  contributors). HMMA lowers to a compact software matrix multiply.
- Many more native forms are lowered, each with a switch that rejects it again: `F2I` byte, ceil,
  16-bit and signed/high-half forms (`EDEN_NX2_F2I_EXTENDED`, `EDEN_NX2_F2I_FORMS`),
  `IMAD.WIDE/HI/X` (`EDEN_NX2_IMAD_WIDE_FORMS`), `LEA.HI` forms (`EDEN_NX2_LEA_FORMS`), `IADD3` with three sources or two carries
  (`EDEN_NX2_IADD3_CARRY_FORMS`), `RED` with an offset, `MUFU` immediate, `ULOP3` form 0, signed
  `SGXT`, `FSET.BF` uniform, `FMNMX.NAN`, `IMNMX` negative and full-width immediates,
  `BAR.SYNC.DEFER_BLOCKING`, `VOTEU` high destinations, register-handle `SULD`/`SUST`, `USHF`
  with an immediate high word, `ULDC.U8/U16`, `TLD4.PTP` with proven offsets, `UPOPC` inversion,
  `SUATOM`, `SHF.L.HI`, `BRA .DIV/.CONV`, full-mask 1D/buffer `SUST.P`, any cache hint on `LDG`
  and local spills, and guarded uniform arithmetic (from Eglantine, Duck912, rdenno and
  vibecodekun; PokeDinner, Franseven, KaitonGxx and BXYMartin; Lichii, Zortos; Orchid, MythosOfs;
  Eden-NS2).
- Native pass-through geometry programs are lowered by default (other geometry stays behind
  `EDEN_NX2_EXPERIMENTAL_GEOMETRY=1`), and a lowered pass-through counts as a regular geometry
  program on hosts without NV pass-through (from nowhere02; PokeDinner PR #19, MamamaNanana).
- HMMA is emulated by default in F32 for programs with at most 64 HMMA (`EDEN_NX2_TENSOR`,
  `EDEN_NX2_TENSOR_LIMIT`) (from PokeDinner, Franseven).
- `TMML` returns 8.8 fixed point in Maxwell order, `F2I` F32 to U64 saturates, `I2F` U64 and U32 RZ
  round exactly, and Maxwell `PRMT` is translated in its default mode (from PokeDinner, Franseven,
  via Eden-NS2).
- SPIR-V: capabilities for PrimitiveId and draw parameters, the narrow storage CAS helper on every
  fallback path, builtins no longer inferred from pass-through masks, routed global `UMax32`
  atomics, and NVN slots separated from loaded pointers (from Lichii, Eglantine, nowhere02 and
  kipavy).
- The texture pass dedups by handle program, resets depth and rolls back slots (from Eglantine).
- `ISBEWR` data registers are shadowed (from Lichii PR #100, Zarigis); `H0_NH1` mixed-sign half
  sources fixed (Jonas); clamped `SHR` fills from the base (from Lichii, Zortos).

**Services**

- `nx2_switches.txt` is read at startup next to the executable, in the data folder and in a
  portable `user` folder, so switches work for GUI launches (from PokeDinner, Franseven).
- Auto-stubbed commands reply with zeros (`EDEN_NX2_AUTO_STUB_ZERO=0`) (from Eglantine, Takoroka);
  vi2 client state, notifications and layer buffer bits, and gsv 115 (Donkey Kong Bananza) (from
  PokeDinner and Eglantine).
- `nfp` `InitializeEx` (300) for Switch 2 amiibo titles (`EDEN_NX2_NFP_INIT_EX=0`) (from
  PokeDinner); ISelfController 2400 and `SetGraphicsScaleChangedNotificationEnabled` (1050);
  `IApplicationFunctions` 320 reports the applet's screenshot and recording permissions;
  `nvhost-tsg-gpu` subcontext ioctls are stubbed (`EDEN_NX2_TSG_STUB_ALL=1` restores success for
  all); `GetBuiltInFontData` parses `{u8 type, u8 flag}` (from Eglantine, Aidan Kinzett); stream
  play `IUserShimScopedObject` 300 behind `EDEN_NX2_STREAMPLAY_300=1`, opt-in (from Lichii).
- The GPU channel error notifier is cleared when it is installed (from PokeDinner, from Eden-NS2).
- Switch 2 movies decode with frame threads (`EDEN_NX2_VDEC_FRAME_THREADS=0`) (from PokeDinner,
  BXYMartin), and unnamed H.264 streams are detected (from PokeDinner, rdenno).

**UI**

- **Mesh shaders (Switch 2)** checkbox in Graphics > Advanced (on by default, per game, changeable
  while a game runs); the `EDEN_NX2_NO_MESH` variables still force the skip.
- Every setting has a rich tooltip with an explanation and, where useful, a tip.
- New Nemyu logo, Windows icon, platform icons, and Switch 2 artwork for the controller previews,
  controller icons and game placeholder; the Discord presence image URLs are versioned so the new
  logo shows.
- Japanese translation refined and completed (EmilyTsum).

**Diagnostics and tools**

- `EDEN_NX2_GPU_PROFILE` counts the split front's waits per site, forced fence releases and
  acquires met in memory, with new zones for deferred calls, sync requests, frame ticks, DMA and
  2D blits; draws that wait for a pipeline build, long GPU-thread operations, held frames, vi2
  present intervals and game speed, and the origin of misread command words are logged.
- `nemyu-cli`: key script code 0 ends the run cleanly; `-d/--debug` and `-i/--input-profile` take
  their argument (from PokeDinner, BXYMartin).
- `tools/fork-scan.sh` lists commits from the forks Nemyu follows that it does not have yet (idea
  from Orchid, MythosOfs).
- Local build scripts for Windows and Linux that repeat the CI's setup.
- Mario Kart World bench: loading-screen counters, game speed, native log path, resolved paths and
  portable build logs.

**Opt-in experiments** (off by default)

- `EDEN_NX2_ACQUIRE_PEEK=1` and `EDEN_NX2_ACQUIRE_QUEUED=1|2` skip split drains for acquires already
  met or met by queued releases (from Selevet and Orchid); `EDEN_NX2_BACK_SPIN=<pauses>` and
  `EDEN_NX2_GPU_WAIT_SPIN_US=<us>` tune spinning (from Orchid).
- `EDEN_NX2_READ_AHEAD=1` (sequential CPU read flush-ahead), `EDEN_NX2_GPU_ONLY_ZERO_SKIP=1`,
  `EDEN_NX2_PACE_HOST_SLEEP=0` (guest-kernel pacing, from Eglantine, Duck912) and
  `EDEN_NX2_GSV_HOST_THREAD=1` (graphics system sessions on a host thread, from Orchid) were added
  as defaults and then kept opt-in after two fatal exits in early testing.
- `EDEN_NX2_FRAMES_AHEAD=auto` waits for the current frame (from Eglantine, Sssnowyyy);
  `EDEN_NX2_SPLIT_METHODS=auto|park` parks split methods across called command buffers;
  `EDEN_NX2_PBFLUSH=1` flushes small GPU-written parameter blocks (from Lichii);
  `EDEN_NX2_NATIVE_UBO_REUPLOAD=1` re-uploads native 64 KiB uniform banks (from PokeDinner);
  `EDEN_NX2_NATIVE_SSBO_MAPPED=1` sizes native compute storage buffers from their mapping (from
  Eglantine, Duck912).

### Changed

- **Settings regrouped into eight categories:** General (General, Interface), Graphics (Graphics,
  Advanced, Extensions), Controls (Players, Advanced, Hotkeys), System (System, Profiles,
  Filesystem, Applets), CPU, Audio, Network (Network, Web) and Debug. Pages scroll, and Configure
  opens at a size that fits the Controls page.
- The FSR options are named after what they run: "FSR 1", "FSR 1, slightly sharper" and the
  "Built-in optical flow" frame generation (each keeps "(was FSR 3)"); "Window Adapting Filter" is
  now "Upscaling Filter"; 12 GB DRAM loses "(Unsafe)"; anisotropy "Default" is "Game Default".
- Settings rows that do nothing in this build are hidden ("Use asynchronous GPU emulation", "Check
  for updates", "Enable Gamemode" on Windows, "Dump Optical Flow").
- The low virtual memory warning no longer holds the boot: a status bar note plus a non-modal box
  once per session with "Don't show this again", at 12 GiB (`EDEN_NX2_LOW_COMMIT_WARNING_18G=1`
  restores 18 GiB).
- Mario Kart World runs about twice as fast in races: native compute storage buffers use
  descriptor sizes again and fence downloads copy only the Preflushable pages
  (`EDEN_NX2_EXACT_PREFLUSH=0`) (from Lichii PR #104, Zarigis): race frames went from 57-73 ms to
  about 21 ms on an RTX 3050 laptop with frame generation off.
- Donkey Kong Bananza waits for each of its frames (from Eglantine #506, Duck912), and command lists
  that waited behind a semaphore are copied again before they run (`EDEN_NX2_PB_RESNAPSHOT`;
  Eglantine `9fea7d8379`, rdenno), aimed at its first-cave freezes. The freeze could not be
  reproduced by the scripted test, so it is not confirmed fixed.
- Donkey Kong Bananza's native terrain reset runs on the CPU by default, before `PrepareDraw` and
  bounded (`EDEN_NX2_RESETFIX=0`), restoring the missing cave terrain (from PokeDinner and Lichii).
- Native graphics pipelines wait up to 3 ms each (8 ms per ~16 ms window) before a draw is skipped
  (`EDEN_NX2_NATIVE_SYNC_PIPELINES=budget|full|0`, `EDEN_NX2_NATIVE_SYNC_BUDGET_US`) (from
  PokeDinner; budget idea from ZendNevs).
- nx2:vi2 paces presents by their swap interval once a game keeps it for 120 presents, so
  Fortune's Weave no longer runs at double speed; Mario Kart World keeps one vblank per present
  (`EDEN_NX2_SWAP_INTERVAL`) (from Eglantine, Duck912; guard from Selevet).
- Guest CPU writes to GPU-written memory are applied before each command list
  (`EDEN_NX2_HELD_WRITES=0`) (from PokeDinner, Franseven, from Eden-NS2).
- Cache flushes and sync fences run on the GPU split's back thread (adapted from Lichii, Zortos).
- With frame generation under FIFO VSync, the swapchain presents with Mailbox
  (`EDEN_NX2_FG_KEEP_FIFO=1`); presents use a queue of their own instead of the submit lock
  (`EDEN_NX2_PRESENT_SHARED_QUEUE=1`); each frame acquires with its own semaphore and a timeout
  (`EDEN_NX2_PRESENT_LEGACY_ACQUIRE=1`) (in part from zortos293/Eglantine).
- The stream buffer is 128 MiB on every GPU (`EDEN_NX2_STREAM_BUFFER_256=1`), an integrated GPU's
  budget is capped at its memory share (`EDEN_NX2_IGPU_HEAP_BUDGET=1`), and idle staging buffers are
  freed with large ones rounded in quarter steps (`EDEN_NX2_STAGING_LEGACY=1`) (from Eglantine,
  Takoroka, via Selevet and Orchid).
- ASTC decodes on the GPU no longer wait or submit (`EDEN_NX2_ASTC_FLUSH=0|1`) (from Orchid).
- The applet capture of Switch 2 frames is drawn when an applet reads it, with three resource sets
  (`EDEN_NX2_APPLET_CAPTURE_EVERY_FRAME=1`, `EDEN_NX2_CAPTURE_SETS`) (from Orchid and PokeDinner).
- The used-pipeline list survives short sessions (`EDEN_NX2_USED_LIST_MERGE=0`).
- The guest subgroup size is only forced for warp-level compute (Intel SIMD32) (from Eglantine,
  999sian and Yuri Olive).
- The README header is centred on GitLab and its status, requirements and credits were refreshed;
  `docs/UPSTREAM_CREDITS.md` lists the adapted fork changes.

### Fixed

- Donkey Kong Bananza: up to 18 storage buffers per stage no longer crash the driver (the buffer
  cache follows the shader's limit of 32) (from Eglantine PR #301, nowhere02); the pushbuffer
  desync at 157 s (a method split across GPFIFO entries) is handled by parking only when the next
  entry matches; Generic31Y-W are generic attributes and undefined outputs are dropped.
- Syncpoints: a slot's value is published before its event is signalled
  (`EDEN_NX2_SYNCPT_PUBLISH_IN_ACTION=0`), slot events are created signalled
  (`EDEN_NX2_SYNCPT_SIGNAL_NEW_SLOTS=0`), and syncpoint actions are deregistered by id.
- Timed `Event` waits on Windows end only on the signal or the deadline
  (`EDEN_NX2_EVENT_WAIT_LEGACY=1`) (from zortos293/Eglantine); expired weak timing events no longer
  spin forever (`EDEN_NX2_TIMING_DROP_EXPIRED=0`) (from Orchid); accelerated texture uploads read
  only the image's guest bytes (`EDEN_NX2_UPLOAD_READ_BOUNDS=0`).
- Audio: in/out managers are unregistered before they are destroyed and stay alive while sessions
  are open (from Eglantine, Dee1), and libopus state is kept on the host
  (`EDEN_NX2_OPUS_GUEST_STATE=1`) (from Eglantine, k-start).
- hid ignores input before the npad applet resource exists; six-axis sensor values have defaults
  (`EDEN_NX2_SIXAXIS_LEGACY=1`); the Polish language code is correct (from PokeDinner and Lichii).
- Video: readiness notifications stop during teardown (`EDEN_NX2_VIDEO_READY_GATE=0`) and an NV12
  output pitch narrower than the frame is rejected (from Lichii, Zortos).
- Vulkan: the minimum framebuffer layer count is used with clears clipped to it, task and mesh
  stages get push constants, null-image buffer views are skipped, storage views are cached per
  format, ASTC view formats only list A8B8G8R8 when uncompressed, descriptors are validated with
  `TextureViewHeader`, tessellation pipelines the driver would refuse are skipped, query results are
  flushed before native MME DMA reads (`EDEN_NX2_DMA_READ_QUERY_FLUSH=0`), depth-stencil reinterpret
  copies copy one aspect (from Lichii PR #89, cr1ckt; an NVIDIA device loss), a failed GPU wait is
  logged before the process ends (from Eglantine PR #361, Sssnowyyy), and RADV is treated like the
  other AMD drivers for unbound sparse images (`EDEN_NX2_SPARSE_UNBACKED`) (kraibse).
- Core: the original ASID is restored after cross-ASID device memory marks, `GetPointer` is guarded
  against non-physical addresses, unmapped device pages are walked one at a time (from Lichii and
  PokeDinner), `HintCold` is clamped to tracked pages (from Selevet), and a guest backing failure
  logs `GetLastError`.
- Settings: the profile list's order and its lock while a game runs, wrong defaults and typos, the
  gamecard path is saved and XCI filenames stay intact, the touchscreen defaults, the masked web
  token, a confirmation before deleting an input profile, per-game Properties no longer change
  global settings, the View menu overlay toggle stays in sync, and every room dialog treats the
  "Nemyu" nickname as a placeholder.
- The game list no longer terminates on "Access is denied" during a scan, and stays disabled during
  the startup scan (from Eglantine, rdenno).
- `IsRescaled` no longer dereferences the missing parent of a null or buffer view
  (`EDEN_NX2_NULL_VIEW_RESCALE_LEGACY=1`), and the bound texture buffer is checked on the
  `TEX.ARRAY.DC` and `TEXS` paths (from Lichii, zortos293).
- The `maxwell-spirv` tool builds again (from Lichii), and the combined build compiles with MSVC's
  shadow warnings treated as errors.

### Performance

- Ported from Eglantine build-158 and other forks: native macro parameters without a second FIFO
  copy, register-write history opt-in in release builds (`EDEN_NX2_3D_HISTORY=1`), bitmap scans that
  skip empty words, unchanged small uniform buffers reused within a command buffer
  (`EDEN_NX2_UBOREUSE=0`, gabrielskaminski), and batched shader cache invalidations (snowstormxd).
- GPU thread: resolved texture ids kept next to the descriptor tables, GPU-written pages marked a
  bitmap word at a time, layout sizes answered from the last kind map run
  (`EDEN_NX2_LAYOUT_CACHE=0`), page tracking without heap allocations, deferred invalidations
  rounded to whole blocks (`EDEN_NX2_INVALIDATE_BLOCKS=0`) and deduplicated
  (`EDEN_NX2_INVALIDATE_DEDUP=0`), the upload barrier that only ended the render pass skipped
  (`EDEN_NX2_UPLOAD_BARRIER=1`, from Eglantine PR #361), fence download ranges merged
  (`EDEN_NX2_FENCE_RANGE_MERGE=0`), dynamic states set for the pipeline `PrepareDraw` bound
  (`EDEN_NX2_DYNAMIC_STATE_LOOKUP=1`), GPU split fields on their own cache lines, the GPU thread
  woken by new submissions while channels wait (`EDEN_NX2_PENDING_WAKE=0`), `UsageTracker::Reset`
  skipped when nothing was tracked, and invalid index logs moved to trace level (from
  zortos293/Eglantine, Orchid and Eglantine PR #374).
- Kernel and timing: host service threads woken after the scheduler lock is released
  (`EDEN_NX2_DUMMY_WAKE_UNLOCKED=0`), the timing thread woken only for a new earliest event
  (`EDEN_NX2_TIMING_WAKE_EARLIEST=0`), the scheduler lock word on its own cache line, a pause between
  context guard tries (`EDEN_NX2_CONTEXT_GUARD_PAUSE=0`), and static x64 emit handler tables (from
  Orchid, MythosOfs).
- Shader recompiler: unused SSA identities dropped, failed storage buffer searches pruned (593 ms
  to 75 ms on a large compute shader), scalar constant ids remembered, visited blocks found by
  lookup, predicate and flag definitions kept in blocks, and phis of sealed blocks created only when
  needed (from PokeDinner, Wael3rd).
- NCZ/NSZ/XCZ block reads are faster and pre-decrypted NCZ skips the second AES pass (from
  PokeDinner, PavelBARABANOV).

### Tests

- CoreTiming tests for expired events and shared state reset between cases; Ampere lowering tests;
  `descriptor_table_growth`, H.264 converter and ISBEWR tests come with their ports.

### Removed

- The modal "Outdated Game Format" warning on the first boot of an extracted dump, which was wrong
  for Switch 2 games (Eglantine `426f120ee8`, snowstormxd, removes it too).

## [0.1.3] - 2026-11-03

15 commits since 0.1.2.

### Changed

- nx2:gpu 214 regions without the sparse flag (render targets) are backed as they are written
  instead of in full at creation, adapted to Mario Kart World's region re-creation (from Eglantine
  PR #269, Takoroka); `EDEN_NX2_GPU_ONLY_LAZY=0` restores full backing.
- CI publishes Windows and Linux releases from tags.

### Fixed

- Mario Kart World's 2162-0001 abort while its title movie loops: the armed syncpoint waits are
  read before the values they answer (`EDEN_NX2_SYNCPT_VALUES_FIRST=1` restores the old order;
  `EDEN_NX2_SYNCPT_POLL_DELAY_US` is a test knob), and a slot is cleared under the lock its old
  signal holds.
- A crash after about 4.5 minutes of Mario Kart World: `FindBuffer` is bounded by its page table
  (from Eglantine PR #260, Whovian64).
- Device memory is rebuilt on any memory layout change, so Switch 2 titles get their full 12 GiB
  after a smaller configured layout (from Selevet, sylvieistaken).
- Guards for empty range queries (from Eglantine PR #310, néo Luigi), uniform buffers without a
  device address (ymatrix1) and out-of-range macro bind positions (ZnOw01); descriptor table limits
  are clamped.

### Performance

- Descriptor tables are sized by the next power of two of their limit instead of 524,289 entries
  (from Eglantine PR #277, nowhere02), and the descriptor update ring is no longer zero-filled
  (about 120 MiB) (from Eglantine PR #260).
- Gathered multi-range buffers are released with their sources (from Eglantine PR #276,
  nowhere02).
- GPU split ring positions are aligned to 128 bytes (from Eglantine PR #124, Yuri Olive).

### Build & tools

- Mario Kart World bench: records the power source, throttling reasons and power limits, detects
  the race load more reliably, restores its environment variables after each run, and adds an idle
  mode and a guest abort count.

## [0.1.2] - 2026-11-02

22 commits since 0.1.1, mostly finishing the rename.

### Changed

- Logs are `nemyu_log.txt` and `nemyu_gpu.log`, crash dumps `nemyu-crash-<pid>.dmp`; the startup
  lines, crash report, SDL, cubeb and Nsight Aftermath names say Nemyu, and load errors point to
  Nemyu's handbook and issue tracker instead of Eden's channels.
- The default device name, web username, first user profile and stub keyboard text are "Nemyu";
  a saved "Eden" nickname is treated as a placeholder.
- The About dialog, Help links and NSIS installer point to Nemyu's GitLab pages.
- The standalone room server is `nemyu-room`; the room protocol is unchanged.
- The news feed no longer shows Eden's releases as console news, and the update checker no longer
  offers Eden builds.
- Linux files install under the app id `io.gitlab.nemyu_emulator.nemyu` (udev rule
  `72-nemyu-input.rules`).
- dynarmic's AVX-512 paths are off unless `EDEN_NX2_AVX512=1`, after crashes on Zen 4 CPUs in Mario
  Kart World (from Selevet, sylvieistaken).
- Automatic release pipelines are disabled in CI.
- Docs and dev tools name `nemyu-cli`.

### Fixed

- An exit hotkey saved by Eden or Eglantine is recovered, also after 0.1.0 or 0.1.1 saved the
  default.
- AVX-512 `VectorCountLeadingZeros16` returned wrong values in lanes 4-7 (from Selevet, from
  zortos293/Eglantine).
- A 1.3 s freeze in Donkey Kong Bananza: the upscale fallback's debug readback and trace only run
  with `EDEN_NX2_SAMPLE_AT` or `EDEN_NX2_TRACE_AFTER_UPSCALE` (from Selevet).
- Hangs and crashes at shutdown: GPU sync waits end once the GPU threads stop (from fork-alarok),
  and the GPU channels are destroyed after the renderer (closing Mario Kart World mid-race).
- Garbage GPU commands: inline uploads over 64 MiB are dropped, an oversized native vertex stream is
  clamped, addresses past the device space are rejected, oversized render targets are refused, and
  a missing swapchain is recreated (from Eglantine PR #329, failaip12).
- The Windows fallback guest memory is released once, with a test (from Eglantine PR #344,
  QcCodingBear).
- Non-core threads no longer write past the per-core read areas (from Eglantine PR #334,
  snowstormxd).
- The Mario Kart World bench never reads a stale log.

## [0.1.1] - 2026-11-02

99 commits since 0.1.0, mostly ports from Eglantine (Takoroka unless noted).

### Added

- **NUP containers** (Unencrypted (Archival) Package) load, with the aligned NUP v1 record layout
  (from Eglantine, a55uka).
- **Task and mesh shaders:** Switch 2 task and mesh programs (Mario Kart World) are translated and
  drawn with `VK_EXT_mesh_shader`, with `ISBERD`/`ISBEWR` and `UPOPC` lowered for them; draws are
  skipped before their pipeline is built when the extension is missing. `EDEN_NX2_NO_MESH` and
  `EDEN_NX2_NO_MESH_FILE` skip them; these pipelines are kept out of the disk cache. This replaces
  0.1.0's `ISBERD`/`ISBEWR` stopgap.
- Shader lowering for Kirby Air Riders, Nintendo Switch Sports and Donkey Kong Bananza: `MUFU` with a
  uniform source, more `S2R` special registers, 32-bit `MATCH.ALL` (also into `P6`), `TXQ` on any
  buffer and `TXQ.B` with three or four outputs, `TLD.SCR.LL`, `SULD.P.3D`, every `ATOMS` operation,
  `REDUX.OR/XOR/MIN`, 64/128-bit and register-addressed local spills, `LEA` with a uniform addend or
  carry out, `CSMTEST`, bindless `TEX`/`TEX.B` (with `.DC`, `.LZ` and explicit LOD), `TLD.B`,
  `TMML` and `TMML.B`, `TLD4` with a uniform handle or texel offset, `TLD.SCR` with an offset,
  `IMAD.WIDE.U32` and `UIMAD.WIDE.U32` with immediates, `SR_VARIABLE_RATE`, `UPRMT`, `I2F.U8` from any
  byte, `FRND` from a constant buffer, `FSETP` with a uniform operand, `USHF` above UR15, `IADD3.X`
  with a constant buffer, `ISETP.EX`, uniform registers above UR27, and the jump table clamp with
  instructions in between. Texture handles holding a header byte offset are read (Switch Sports
  scenery).
- Switch 2 services: built-in fonts in their own shared memory (Kirby Air Riders), Switch 2 NRR
  hash lists (Kirby Air Riders, Minecraft, Mario Tennis Fever), audio renderer revision 16, an
  empty archive for missing Switch 2 system data (`EDEN_NX2_EMPTY_SYSTEM_DATA=0`), real
  `auddmg:u`/`audsmx:u` audio stream objects (Switch Sports' menu movie), IAudioDevice notification
  events, video decoder commands of later SDKs and gsv 912, graphics device ids for gpu 41/42,
  `nvhost-prof-gpu` and the 2D/3D/compute/I2M/ZBC GPU flags (Fire Emblem's CUDA driver), hid 213/220,
  bsd 36, `ssl:s` contexts and three ssl replies (Minecraft), and reply data for AM 320/321,
  `GetCurrentGraphicsScale` and nsd 15/21.
- vi2 229 crops a present to the area the game drew (Kirby Air Riders docked), and the upscale
  images are found outside title 4's slots.
- Pokémon Pokopia's name entry: Switch 2 library applet ids map to the applet in their low byte, and
  software keyboard config 0x140010 uses UTF-8 (from Eglantine, rdenno).
- `EDEN_NX2_SVC_TRACE` names the handles a `WaitSynchronization` waits on, and NX2_NOTES.md
  documents the task and mesh programs and their support.

### Changed

- The build tools build into `build/`; the MSVC build is the only one shipped.

### Fixed

- Permanent freezes in Switch 2 titles: GMMU maps run on the GPU thread, with Nemyu's tracking of
  queued map ranges so command lists are not read through a stale mapping and a lock of their own
  for the range tables; the GPU work of nx2:gpu 214, 215 and 216 is queued after the region lock
  is released.
- A crash in `WriteBlockImpl` on Mario Kart World's title screen (big pages without host memory).
- Breakpoints and single steps running stale code (dynarmic's cached code page).
- A failing shader dump no longer ends the process (Nintendo Switch Sports Resort), nor does an
  unknown system archive id.
- An audio crash when a circular buffer sink is cleaned up (Mario Tennis Fever).
- RDNA GPUs lost `VK_EXT_sampler_filter_minmax` because they were taken for GCN4.
- **Create Shortcut** to a loose dump no longer crashes.
- Wide local loads into their own address register are rejected.
- `eden-cli` sets its window title from the main thread on macOS (Sssnowyyy); CMake no longer fails
  in a git worktree (Yuri Olive).

### Performance

- Memory use: unwritten sparse blocks get no memory for zero writes, guest fiber stacks are not
  zero-filled (about 750 MiB in Mario Kart World), the Vulkan pipeline cache is left to NVIDIA's and
  AMD's drivers, shader modules are dropped once their pipeline is built, a loose RomFS is built
  once and only when read (about 0.6 GB), real files keep only their path (160 MB to 48 MB) and the
  path cache forgets closed files, RomFS build nodes no longer own each other, the texture collector
  empties the emptiest memory blocks first, sparse images are cold-hinted too, deleted allocation and
  framebuffer slots are freed, and freed sparse vector regions are forgotten.

## [0.1.0] - 2026-11-02

The first Nemyu release: 81 commits from the fork of Eglantine (`a65d7bf29a`) to the tag, including
the rename.

### Added

**Identity and distribution**

- The emulator is renamed to Nemyu: names, icons, executables (`nemyu`, `nemyu-cli`), docs and
  translation sources.
- Nemyu keeps its data in its own folder (`%APPDATA%\nemyu` and the matching XDG folders). On the
  first run it copies Eden's `eden` folder once, under a lock, and leaves it untouched.
- The About dialog and the Linux desktop and AppStream files describe Nemyu by itself; the README
  keeps the credits to Eglantine, Eden, yuzu and Sudachi. The PGO profile is renamed
  `nemyu.profdata`.
- The repository tooling moved from Forgejo to GitLab (issue and merge request templates under
  `.gitlab/`, the `.github` folder removed), the docs point to GitLab, and the README and
  CONTRIBUTING.md were rewritten.
- Releases: a script publishes the MSVC build as a GitLab release (with Qt, the Visual C++ runtime
  and the licence texts), and a CI job builds Windows releases on GitLab tags, with a pinned
  PortableGit for `patch.exe` and sccache/CPM caching.

**UI**

- Box art for games without an icon (loose Switch 2 dumps), downloaded once and shown in the game
  list as soon as it arrives, with a placeholder meanwhile; the source URL can be changed.
- Discord Rich Presence, on by default, with the game's cover.
- A painted loading screen with the cover, title, version and one progress bar with the shader count
  and an estimate, a sweep until the first frame, and a matching closing screen.
- Switch 2 dumps without control data are named from the user's `titles.json` or a bundled title
  list built from Switchbrew's; NACPs with compressed titles are read.
- A warning when Windows cannot commit enough memory for a Switch 2 title, with how to enlarge the
  page file.

**Graphics and GPU**

- NVN's indexed indirect draws are drawn from GPU memory, so the GPU thread no longer downloads the
  command list several times a frame: Mario Kart World races went from about 29 to 24 ms a frame
  on an RTX 3050 laptop (`EDEN_NX2_INDIRECT_DRAWS=0`).
- nx2:gpu 216 maps and unmaps sparse texture tiles, with `NVGPU_GPU_FLAGS_SUPPORT_REMAP` reported
  (`EDEN_NX2_SPARSE_REMAP=0`): Mario Kart World no longer leaks about 2.5 GB of sparse blocks. A
  mapped tile counts as written (the static world was unlit), and large sparse 2D images are backed
  per sparse block, except on AMD (from Eglantine, Takoroka; guards from kraibse).
- FSR 3 frame generation works: the toggle was bound to the wrong setting, history is kept per
  frame slot, generated frames are paced over the real frame interval (`EDEN_FRAME_GEN_NO_PACING`),
  it can be changed while a game runs, and the status bar shows the presented frame rate. Frame
  generation no longer caps the game's own frame rate.
- Command lists run from a copy taken at submit (`EDEN_NX2_PB_SNAPSHOT=0`), and native programs
  are captured and lowered once (from Eglantine, kraibse, from Wael3rd/Eglantine-MKW).
- Texture handles read from storage buffers are computed for each draw (Switch Sports' scenery
  lighting), and R2UR's unreachable select is dropped (from Eglantine, Takoroka).
- Shader lowering: `SURED.ADD` on 2D surfaces and narrow `LDC` sizes (Mario Kart World), `LDC.IS`
  (Kirby Air Riders, from Eglantine, Takoroka), the `TXD` and `TXQ` variants Splatoon Raiders uses
  (from Eglantine, kraibse), and a stopgap for `ISBERD`/`ISBEWR` (from Eglantine, Paulo Rossatto and
  néo Luigi), replaced in 0.1.1.

**Tools**

- A Mario Kart World race benchmark (`tools/nx2/mkw_bench`) with a soak mode.
- `nemyu-cli` prints the current exception and the stack on abort with `EDEN_NX2_CRASH_DUMP`.

### Changed

- Switch 2 titles run at GPU accuracy Low by default (Mario Kart World: 24.5 against 41.7 ms a
  frame); a per-game value is kept, `EDEN_NX2_GPU_ACCURACY=high|low` forces it, and the status bar
  button toggles the accuracy in effect.
- Reactive flushing is off for Switch 2 titles by default (`EDEN_NX2_REACTIVE_FLUSHING=0|1`).
- Present also waits for the presented frame when the GPU is behind, which stopped Splatoon Raiders
  crashes on slow GPUs (from Eglantine, Takoroka).
- On hybrid CPUs, threads that want performance cores are kept on them (`EDEN_CPU_SETS=0`).
- Dynamic vertex input driver version checks are fixed for NVIDIA and Intel; Switch 2 titles keep it
  off there (`EDEN_NX2_VIDS=1`).
- Log lines go to `OutputDebugString` only when a debugger listens (`EDEN_LOG_DEBUG_OUTPUT=1`).

### Fixed

- Leaks and stale references in the Switch 2 services: sessions are owned by the client's handle
  (`EDEN_NX2_KEEP_SESSIONS=1`), closed sessions' events are not signalled, destroyed GPU regions go
  back to the kernel, vi2 reuses its events, the present wait keeps waiting while the GPU makes
  progress, and the GPU split frees its ring at shutdown.
- Out-of-range syncpoints and varying counts from a desynced command stream no longer crash.
- A crash at Mario Kart World's race intro on AMD Windows drivers (division by a zero
  `imageGranularity`, issues #111 and #172) (from Eglantine, Paulo Rossatto).
- Re-creating a GPU-only region releases the previous backing, counts it once and forgets its 216
  tiles (memory grew to over 6.5 GiB) (from Eglantine, Paulo Rossatto).
- Large sparse images are backed normally on AMD (from Eglantine, kraibse).
- The crash on exit in #181: device memory outlives the kernel, and the Qt frontend destroys the
  system before `main` returns.
- A maximized window stays maximized when a game boots.
- Mario Kart World's movies: every compressed frame a video client submits is kept (the course
  intro showed striped blocks and the menu loop ended early).
- The handle slot map is thread-local (GCC 15 build).

### Performance

- Windows directory listing with `FindFirstFileExW`: Mario Kart World's 206k-file RomFS lists in
  about 0.4 s instead of 30 s in MinGW builds.
- Console log lines are written in one call (about 1.5 ms to 20 us per line in MinGW builds, which
  also removed boot desyncs).
- Guest memory is committed on demand on Windows (`EDEN_NX2_COMMIT_ALL=1`), and dynarmic's x64 code
  cache is reserved and committed as code is emitted (from Eglantine, Takoroka, replacing Nemyu's
  first version); dynarmic invalidates blocks through per-page buckets.
- VRAM on 6 GB GPUs: budget-relative buffer collection (from 512 MiB), a pool of ASTC decode
  images, evictions credited with their real size, a 5 GiB critical texture threshold on every GPU,
  and out-of-range images getting a null view instead of ending the process (`EDEN_NX2_TC_SLACK=1`
  is an opt-in variant).
- Texture cache (from Eglantine, Takoroka): images deleted without walking every channel's tables
  (362 to 3.7 us each), view entries found without a scan, deleted images not counted until
  destroyed and freed once the GPU is past them, a table-driven ASTC opaque scan (224 to 37 us per
  texture), and cold hints for uploaded texture and buffer memory and for sparse and GPU-only
  backing so the OS pages them out first.
- Software 2D blits touch only the rows they cover, and blit scratch buffers over 2 MiB are freed
  (from Eglantine, Takoroka).
- Vulkan memory: 16 MiB VMA blocks, with separate pools for sparse pages and device-local buffers
  (from Eglantine, Takoroka).
- Pipelines: only those the last two sessions used are built at boot (`vulkan_used.bin`), at most
  3,000 without a list, and pipelines unused for 9,000 frames are destroyed
  (`EDEN_NX2_PRELOAD_ALL_PIPELINES=1` builds all) (from Eglantine, Takoroka).

[Unreleased]: https://gitlab.com/nemyu-emulator/nemyu/-/compare/nemyu-v0.1.7...nemyu
[0.1.7]: https://gitlab.com/nemyu-emulator/nemyu/-/compare/nemyu-v0.1.7-experimental.3...nemyu-v0.1.7
[0.1.7-experimental.3]: https://gitlab.com/nemyu-emulator/nemyu/-/compare/nemyu-v0.1.7-experimental.2...nemyu-v0.1.7-experimental.3
[0.1.7-experimental.2]: https://gitlab.com/nemyu-emulator/nemyu/-/compare/nemyu-v0.1.7-experimental.1...nemyu-v0.1.7-experimental.2
[0.1.7-experimental.1]: https://gitlab.com/nemyu-emulator/nemyu/-/compare/nemyu-v0.1.6...nemyu-v0.1.7-experimental.1
[0.1.6]: https://gitlab.com/nemyu-emulator/nemyu/-/compare/nemyu-v0.1.5...nemyu-v0.1.6
[0.1.5]: https://gitlab.com/nemyu-emulator/nemyu/-/compare/nemyu-v0.1.4...nemyu-v0.1.5
[0.1.4]: https://gitlab.com/nemyu-emulator/nemyu/-/compare/nemyu-v0.1.3...nemyu-v0.1.4
[0.1.3]: https://gitlab.com/nemyu-emulator/nemyu/-/compare/nemyu-v0.1.2...nemyu-v0.1.3
[0.1.2]: https://gitlab.com/nemyu-emulator/nemyu/-/compare/nemyu-v0.1.1...nemyu-v0.1.2
[0.1.1]: https://gitlab.com/nemyu-emulator/nemyu/-/compare/nemyu-v0.1.0...nemyu-v0.1.1
[0.1.0]: https://gitlab.com/nemyu-emulator/nemyu/-/releases/nemyu-v0.1.0
