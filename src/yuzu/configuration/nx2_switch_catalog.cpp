// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <QCoreApplication>
#include "yuzu/configuration/nx2_switch_catalog.h"

namespace Nx2SwitchCatalog {
namespace {

constexpr Choice ACQUIRE_QUEUED_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: off")},
    {"gequal", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "gequal: acquire-gequal")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: every acquire (Selevet)")},
};

constexpr Choice ACQUIRE_WRITER_WAIT_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: off")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: wait for its writers, poll drains every 1 ms")},
    {"2", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2: also suspend without the drain")},
};

constexpr Choice AMD_SHUFFLE_LOOP_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: keep the shuffles")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: loop on every driver")},
};

constexpr Choice AMD_SAFE_COMPILE_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: never")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: any AMD Windows driver version")},
};

constexpr Choice COMPUTE_FAST_FIRST_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: never")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: any GPU driver")},
};

constexpr Choice ASYNC_SKIP_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: always wait")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: skip while building")},
};

constexpr Choice BACKGROUND_PIPELINES_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: off")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: every title, every system")},
};

constexpr Choice BACK_SPLIT_JOBS_CHOICES[] = {
    {"geometry,images", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "geometry,images: both jobs")},
    {"geometry", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "geometry: index and vertex ranges only")},
    {"images", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "images: image descriptors only")},
};

constexpr Choice GPU_ACCURACY_CHOICES[] = {
    {"high", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "High")},
    {"low", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Low")},
};

constexpr Choice RT_FRESH_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: upload new render targets as before")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: targets nothing overlaps start empty")},
    {"2", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2: overlapped targets start empty too")},
};

constexpr Choice GPU_TIMESTAMP_CHOICES[] = {
    {"ns", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "ns: nanoseconds for every title")},
    {"ticks", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "ticks: GPU ticks for every title")},
};

constexpr Choice PB_RESNAPSHOT_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: read memory as before")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: copy the current list")},
    {"2", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2: copy it and the lists queued behind it")},
};

constexpr Choice PB_RESNAPSHOT_AFTER_CHOICES[] = {
    {"all", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "all: after every release")},
    {"cpu", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "cpu: only after releases the game wrote")},
};

constexpr Choice SPLIT_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: one GPU thread")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: engine methods on a second thread")},
    {"2", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2: macros on the first thread too")},
};

constexpr Choice SPLIT_METHODS_CHOICES[] = {
    {"continue", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "continue: go on in that entry")},
    {"park", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "park: wait for the rest")},
    {"auto", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "auto: experimental heuristic")},
};

constexpr Choice HELD_WRITE_BATCH_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: off")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: batch and join writes")},
    {"2", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2: batch without joining")},
};

constexpr Choice NATIVE_UBO_REUPLOAD_CHOICES[] = {
    {"2", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2: check on every bind, upload changed pages")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: upload once per frame")},
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: off")},
};

constexpr Choice NATIVE_SSBO_MAPPED_CHOICES[] = {
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: every binding")},
    {"w", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "w: written compute bindings")},
    {"r", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "r: read compute bindings")},
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: none")},
};

constexpr Choice ASTC_FLUSH_CHOICES[] = {
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: submit after each decode")},
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: wait for the GPU to go idle")},
};

constexpr Choice SPARSE_UNBACKED_CHOICES[] = {
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: on every driver")},
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: on none")},
};

constexpr Choice EXPERIMENTAL_GEOMETRY_CHOICES[] = {
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: every geometry program")},
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: none")},
};

constexpr Choice NATIVE_SYNC_PIPELINES_CHOICES[] = {
    {"budget", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "budget: wait briefly")},
    {"full", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "full: always wait")},
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: skip while building")},
};

constexpr Choice SKIP_GUARD_CHOICES[] = {
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: only targets cleared every frame")},
    {"depth", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "depth: also depth-tested scene draws")},
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: any draw")},
};

constexpr Choice CPU_SETS_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: never place threads")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: also with few P-cores")},
};

constexpr Choice FRAMES_AHEAD_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: wait for this frame")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: wait for the previous frame")},
    {"auto", QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "auto: this frame, then the previous one if a channel stalls")},
};

constexpr Choice MKW_FPS_MODE_CHOICES[] = {
    {"off", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "off: as released")},
    {"dynamic", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "dynamic: interval from the frame time")},
    {"30", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "30: locked to 30 fps")},
};

constexpr Choice INLINE_DEFER_CHOICES[] = {
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: defer every cache")},
    {"2", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2: tell the texture cache at once")},
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: invalidate at each write")},
};

constexpr Choice PBFLUSH_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: off")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: blocks on GPU-written pages")},
    {"2", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2: every block")},
};

constexpr Choice RUN_CACHE_CHOICES[] = {
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: per page")},
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: per address")},
    {"2", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2: per page, checked")},
};

constexpr Choice DLSS_PRESET_CHOICES[] = {
    {"default", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "default: NVIDIA's choice")},
    {"E", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "E: CNN model (deprecated)")},
    {"F", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "F: CNN model (deprecated)")},
    {"J", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "J")},
    {"K", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "K: the default")},
    {"L", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "L")},
    {"M", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "M")},
};

constexpr Choice GPL_CHOICES[] = {
    {"0", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: off")},
    {"1", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: on, except on AMD's proprietary driver")},
    {"2", QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                            "2: on, AMD's proprietary driver too (optimized links there)")},
};

constexpr Choice FEEDBACK_COPIES_CHOICES[] = {
    {"coalesce", QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "coalesce: once per texture barrier and target set")},
    {"exact", QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "exact: before every draw")},
};

// Sorted by category, then name. Texts say what changing the switch from its default
// brings: gains is what it can fix or speed up, downsides what it can cost or break.
constexpr Entry ENTRIES[] = {
    {
        .name = "EDEN_NX2_ACQUIRE_PASSED",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A semaphore acquire-equal whose word counted past its value while the channel waited "
            "goes on, as the console's polling channel would have seen the value. A word already "
            "past it when the channel stopped counts after 2 s. Off waits for the exact value."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Off only to compare: on, it ends the 0 fps hangs where channels wait for a value the "
            "word already passed (Ocarina of Time, first boot)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Off, such a hang lasts until the game is restarted."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_ACQUIRE_PEEK",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the split, an acquire-gequal checks the releases still queued for the second "
            "thread instead of draining it first."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fewer drains of the second thread."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mario Kart World ran no faster with it on a GPU-bound machine."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_ACQUIRE_QUEUED",
        .category = Category::GpuCommands,
        .type = ValueType::Choice,
        .choices = ACQUIRE_QUEUED_CHOICES,
        .suggested = "gequal",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the split, a semaphore acquire that a release still queued for the second "
            "thread will meet goes on without waiting for that thread. Without macros on the "
            "first thread it stops at gequal. 1 is what Selevet's 1 does; 2 is the same as 1."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer waits for the second thread (about 2,400 a second in Mario Kart World)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Experimental. At 1, an equal or mask acquire can go on too early."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_ACQUIRE_WRITER_WAIT",
        .category = Category::GpuCommands,
        .type = ValueType::Choice,
        .choices = ACQUIRE_WRITER_WAIT_CHOICES,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the split, an acquire-gequal not met at once waits only until the second "
            "thread ran the queued compute, copies and last release to its semaphore, not "
            "everything and its submit, then reads the semaphore: met, it goes on; else it "
            "drains as before. A suspended one resumes when a release meets it, without a drain, "
            "and drains for writes the first thread does not see only every 16th check or 1 ms. "
            "2 also suspends an acquire unmet at its writers without the drain. Not with Balanced "
            "or Accurate fences."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer and shorter waits for the second thread: Ocarina of Time checks a waiting "
            "acquire about 12 times, and Fritchii measured 380 ms a second of drains for those "
            "checks alone. The log counts them as \"Acquire drains\"."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Experimental, not measured yet. A channel waiting on a write the first thread does "
            "not see (a copy or a shader store) resumes up to 1 ms later."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_AMD_QUERY_WAIT_LAST",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value =
            QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On (AMD's Windows driver and AMDVLK only)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When occlusion query results are copied, waits only for the last query of each "
            "range instead of for every query."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "About 5 ms less GPU time a frame in Ocarina of Time (36 to 51 FPS on AMDVLK)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "If a query finishes after the last one, its count is stale: try 0 for flickering "
            "objects on AMD."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles on AMD GPUs"),
    },
    {
        .name = "EDEN_NX2_BACK_MACROS",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the GPU split and its macros on the first GPU thread (the default), GPU macros "
            "that read memory into their parameters and write only plain state registers run on "
            "the second GPU thread instead, which holds the memory they read: the first thread "
            "goes on without waiting for it to catch up, and waits only when it needs a register "
            "such a macro wrote. The log names each macro that reads memory and the thread it runs "
            "on (Lichii #1056)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Ocarina of Time's render enable macro reads an occlusion report ~25,000 times a "
            "second in Hyrule Field, and each read made the first GPU thread wait for the second: "
            "Lichii measured about 8% more FPS there. Not measured in Nemyu yet. While "
            "EDEN_NX2_DEFER_CONDITIONAL is on (the default in Ocarina of Time) that macro takes "
            "its path instead; set it to 0 to compare. With EDEN_NX2_GPU_PROFILE=1 the second "
            "thread's line counts the macros it ran, and the first thread's drains count its "
            "waits ('back register', 'back code')."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Experimental. A macro that writes a register the check of its code missed leaves the "
            "first thread with stale copies until it notices (logged as an error; the macro then "
            "stays on the first thread). Run once with EDEN_NX2_SPLIT_VERIFY=1 to check it."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "The Legend of Zelda: Ocarina of Time, all Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_BACK_SPIN",
        .category = Category::GpuCommands,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 100000000,
        .suggested = "2048",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "8192 pauses"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How many CPU pause instructions the second GPU thread spins for while it waits for "
            "work, before it sleeps."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lower values save power on power-limited CPUs (laptops); higher ones wake it "
            "sooner."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 adds wake-up latency to every batch; large values keep a core busy."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_BACK_SPLIT",
        .category = Category::GpuCommands,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 16,
        .suggested = "2",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off (0 workers)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Starts this many worker threads that take two pieces of each draw off the thread "
            "that draws: the index and vertex buffer ranges, and the image descriptors of a full "
            "draw. The drawing thread does the rest of the draw meanwhile and runs a piece itself "
            "when no worker has started it. EDEN_NX2_GPU_PROFILE=1 logs a 'Back split per "
            "second' line."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less time per draw on the GPU thread in draw-heavy scenes. Lichii reports +15-25% "
            "FPS in Donkey Kong Bananza; not measured in Nemyu yet."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Experimental. Each worker keeps a core busy while draws come (see "
            "EDEN_NX2_BACK_SPLIT_SPIN), which can slow the emulated CPU on CPUs with few cores. "
            "Run once with EDEN_NX2_BACK_SPLIT_VERIFY=1 to check it."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_BACK_SPLIT_JOBS",
        .category = Category::GpuCommands,
        .type = ValueType::Choice,
        .choices = BACK_SPLIT_JOBS_CHOICES,
        .suggested = "geometry",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Both"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With EDEN_NX2_BACK_SPLIT, keeps only one of its two jobs."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Tells the jobs' effects apart, or keeps the one that helps."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Less work moved off the GPU thread."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_BACK_SPLIT_SPIN",
        .category = Category::GpuCommands,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 10000,
        .suggested = "20",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "50 microseconds"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With EDEN_NX2_BACK_SPLIT, how many microseconds a worker keeps polling after its "
            "last job before it sleeps until the next draw wakes it."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lower values free the workers' cores sooner once draws stop (laptops, CPUs with few "
            "cores)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Too low and the workers sleep between draws, so the drawing thread runs the jobs "
            "itself and pays for the wake-ups."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_BACK_TICK",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The second GPU thread runs the flush a guest read waits for after the entry it is "
            "on, instead of after its next draw or behind every command list the first GPU "
            "thread still holds. Compute dispatches also run waiting flushes, as draws do."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shorter guest waits on reads of GPU-written memory: in Ocarina of Time's Hyrule "
            "Field they spent ~1.2 s of every 10 s queued before the flush started."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 waits for the next draw or the queued tick, as before. A flush runs between two "
            "entries of the second thread, earlier than before."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_CMD_BATCH",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When a command list the GPU wrote has to be copied back, the other GPU-written "
            "command lists that needed it before and are out of date again are copied in the same "
            "wait for the GPU (Selevet's switch, on there)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer GPU waits in titles whose compute or draws write their own command lists "
            "(Donkey Kong Bananza, Mario Kart World)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Each wait copies more; lists copied ahead may be copied again if the GPU rewrites "
            "them first."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_DEFER_CONDITIONAL",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off (on for Ocarina of Time)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the GPU split and its macros on the first GPU thread, NVN's conditional render "
            "macro (render or skip what follows from an occlusion query's result) has the second "
            "GPU thread read the query result and set the render enable mode, instead of the "
            "first thread waiting for it to catch up. Taken before EDEN_NX2_BACK_MACROS, which "
            "keeps the other macros that read memory. 1 turns it on for every title, 0 off, "
            "Ocarina of Time included (nemyu-coco)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "nemyu-coco measured Ocarina of Time in Link's house at 26.8 instead of 28.7 ms a "
            "frame together with EDEN_NX2_INDIRECT_DISPATCH (i7-12650HX, RTX 4050 Laptop)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Recognises the macro by its code. A macro that reads the render enable mode after it "
            "still waits for the second thread. Check with EDEN_NX2_SPLIT_VERIFY=1: no 'Split "
            "front register' warnings."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "The Legend of Zelda: Ocarina of Time, all Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_DMA_RANGES",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When a GPU macro on the split's first thread reads memory no GPU work or query has "
            "written, it waits only for the queued work that can write it (uploads to the same "
            "range, copies, compute, puller work) instead of everything queued (Selevet's name "
            "for EDEN_NX2_PERF_ON=split_overlap; EDEN_NX2_PERF_OFF=split_overlap or "
            "EDEN_NX2_PERF_OPT=0 still turn it off, and EDEN_NX2_PERF_VERIFY=1 checks each read "
            "against a full drain)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less time with the first GPU thread waiting for the second: Lichii measured 100-245 "
            "ms of such waits per second in Donkey Kong Bananza."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Unsafe, for experiments only: Lichii removed its version of this wait (PR #184) "
            "after finding queued work it lets a read pass. Here too, draws are not waited for "
            "(their work may still copy to memory on the CPU or write a page for the first "
            "time), uploads count only at the address they were sent to (not other mappings of "
            "the same memory), images the GPU drew are not checked, and a semaphore release "
            "writes memory only at its fence. A read may get old values."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_DMA_READ_QUERY_FLUSH",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When a GPU macro reads occlusion query results, writes the finished ones to memory "
            "first instead of at the next fence."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 brought back hidden characters in Mario Tennis Fever."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Mario Tennis Fever"),
    },
    {
        .name = "EDEN_NX2_DMA_SNAPSHOT",
        .category = Category::GpuCommands,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 10,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the GPU split, a GPU macro's memory read that waited for the split's back "
            "remembers its pages; later reads of them skip the wait while nothing sent since may "
            "write them and no occlusion report there is pending. 1 on, 2 on and every skipped "
            "wait checked against a waited read (logs differences)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Ocarina of Time reads an occlusion report per object, ~35,000 times a second: Frizti "
            "measured Lake Hylia at 46.5 -> 52.3 FPS, with 0 differences in ~235,000 checked "
            "reads (Evil's EvilOOT fix)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A memory write the split doesn't note would be read stale: objects culled or drawn "
            "wrongly. 2 shows whether it happens."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_DRAW_COUNT_GUARD",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Skips draws whose first vertex or count is past 16 million: garbage left by a "
            "rejected compute program."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 brings back the crash on entering new areas in Hyrule Warriors: Age of "
            "Imprisonment, and out-of-bounds reads."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Hyrule Warriors: Age of Imprisonment"),
    },
    {
        .name = "EDEN_NX2_DRAW_LIST_COPIES",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Command list pages that draws write get copies too, recorded at the next dispatch or "
            "DMA copy, as after dispatches and copies. A command list read from them then waits "
            "for the 3D work sent before it and reads the copy, instead of draining the GPU split "
            "and flushing the caches."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Donkey Kong Bananza's culling draws fill ~46 command lists a frame; with each read "
            "flushed it ran at 20 to 30 fps on an Arc A770 (Eglantine #791). Not measured in "
            "Nemyu yet."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Every command list read waits for all 3D work sent before it. A list read before the "
            "next dispatch or copy still takes the flush."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Donkey Kong Bananza"),
    },
    {
        .name = "EDEN_NX2_FAST_DRAW",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A draw that follows a draw of the same pipeline with only draw parameters, vertex "
            "and index buffers, constant buffer binds or constant buffer data changed in between "
            "keeps that draw's pipeline, image views and samplers, and binds its buffers again. "
            "Any other change resolves the next draw in full."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less GPU thread time per draw in scenes that repeat one pipeline. Lichii has it "
            "always on after 0 verify mismatches in Donkey Kong Bananza and Mario Kart World; not "
            "measured in Nemyu yet."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Experimental. A change it misses would show as a wrong or stale texture until the "
            "next frame; EDEN_NX2_FAST_DRAW_VERIFY=1 counts them."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_FENCE_BATCH",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Releases together every fence the host GPU has already passed, taking each cache "
            "lock once per burst."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 handles one fence at a time: more lock spinning (7% instead of 5.5% of the GPU "
            "thread)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_FENCE_DELAYED_SKIP",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A delayed fence (GPU accuracy High, or Balanced/Accurate fences) with nothing to "
            "download, while no earlier fence is still being released, runs its callback at once "
            "on the GPU thread, as an empty fence below High does. Fences that write a "
            "fence-flagged query report are never taken this way."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Such a fence would be stubbed and run its callback at once on the fencing thread "
            "anyway. Taking it here saves a fence object, the fencing lock and a wake of the "
            "fencing thread, thousands of times a second (Orchid's change for Mario Tennis "
            "Fever)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The callback runs on the GPU thread instead of the fencing thread. A missed earlier "
            "fence would show as a callback that ran too early (sync errors or hangs)."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "All titles with GPU accuracy High or Balanced/Accurate fences"),
    },
    {
        .name = "EDEN_NX2_FENCE_DEDUP",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off (on for Ocarina of Time)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When the fencing thread releases several fences together (EDEN_NX2_FENCE_BATCH), "
            "buffer data more than one of them downloaded is written to guest memory once, from "
            "the newest copy, instead of once per fence. 1 turns it on for every title, 0 off, "
            "Ocarina of Time included (nemyu-coco)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less time with the buffer cache locked by the fencing thread, which the GPU thread "
            "waits for: in Ocarina of Time the same pages went back to guest memory ~50 times a "
            "frame (~775 MB/s outdoors). nemyu-coco measured a frame standing outdoors at 34.9 "
            "instead of 38.1 ms together with writing outside the lock, which Nemyu does not do "
            "(i7-12650HX, RTX 4050 Laptop)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None expected: the released fences' guest operations run only after all their "
            "downloads are written. A wrong result would show as stale or flickering data."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "The Legend of Zelda: Ocarina of Time, all Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_FENCE_REPORTS_AFTER_GPU",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off (on for Ocarina of Time)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Writes a fence-flagged GPU report once the host GPU finished the work before it, "
            "instead of when the fence is signaled. 1 turns it on for every title, 0 off, "
            "Ocarina of Time included."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Meant against white flashes on Ocarina of Time's water foliage and flickering tree "
            "leaves (Matt Artist's Lichii build), and CPU reads no longer wait for those "
            "reports. Lichii and Lichii-Superbuild turn it on for every Switch 2 title."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Experimental: more fences wait for the GPU and more work is submitted, which can "
            "lower the frame rate."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_FLUSH_DRAWS",
        .category = Category::GpuCommands,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 4096,
        .suggested = "128",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Every 4096 draws; 128 in Mario Kart World and Ocarina of Time"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How many draws a Switch 2 title records before they are submitted to the host GPU "
            "(Selevet's switch, 128 there)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The host GPU starts on a frame while it is still being recorded: Selevet measured "
            "26.1 ms frames at 128 against 29.7 ms at 4096 in Mario Kart World (RTX 3050), and "
            "nemyu-coco 38 against 43 ms outdoors in Ocarina of Time's Kokiri Forest (RTX 4050 "
            "Laptop)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "More submissions cost driver time; very small values (32) were slower again."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_FLUSH_HELD_WRITES",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On for the Yoshi demo only"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Applies the held guest CPU writes before any flush of GPU data (CPU reads, native "
            "shader reads, compute uploads), so the download can't put the GPU's older copy over "
            "them. 1 for every title, 0 for none."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps programs the CPU wrote over GPU-written memory from reading as zeros."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Applies the writes earlier than the next command list; not compared on other titles."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Yoshi and the Mysterious Book demo"),
    },
    {
        .name = "EDEN_NX2_GPU_ACCURACY",
        .category = Category::GpuCommands,
        .type = ValueType::Choice,
        .choices = GPU_ACCURACY_CHOICES,
        .suggested = "high",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Low on Switch 2 titles while the GPU accuracy setting is at its default"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Forces the GPU accuracy of Switch 2 titles, over the setting."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "High may fix a rendering bug that Low causes."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "High cost Mario Kart World ~17 ms a frame (41.7 against 24.5 ms) with no visible "
            "difference."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_GPU_TIMESTAMP",
        .category = Category::GpuCommands,
        .type = ValueType::Choice,
        .choices = GPU_TIMESTAMP_CHOICES,
        .suggested = "ns",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Nanoseconds for Ocarina of Time, GPU ticks for other titles"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The unit of the timestamps in Switch 2 GPU reports."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Nanoseconds raise Ocarina of Time's dynamic resolution."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mario Kart World paces by them: 52.6 ms a frame and 60% speed with nanoseconds, "
            "against 27.1 ms and 99% with ticks."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Ocarina of Time, Mario Kart World"),
    },
    {
        .name = "EDEN_NX2_REPORT_SUBMIT_TIME",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With nanosecond GPU timestamps (EDEN_NX2_GPU_TIMESTAMP), a GPU report shows the time "
            "the game submitted the command list being run, never later than now and never behind "
            "an earlier report, instead of the time the host GPU thread got to it."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Games time their GPU work with these reports. The host's command processing is no "
            "measure of it: Orchid saw Wonder take its frames for GPU-bound and switch its light "
            "pass every other frame (flickering scenery)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "GPU work looks almost free to the game. Ocarina of Time raises its dynamic "
            "resolution on nanosecond timestamps, so it may raise it further and run slower on a "
            "small GPU. Titles on GPU ticks are unchanged."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Ocarina of Time (the only title on nanoseconds by default)"),
    },
    {
        .name = "EDEN_NX2_GPU_WRITTEN_CMDLISTS",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Reads the command lists that lie on GPU-written pages through the caches, so their "
            "draws use this frame's commands."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 is faster (16.7 against 24.5 ms a frame in one Mario Kart World test)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 makes trees vanish, and races rendered black."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Mario Kart World"),
    },
    {
        .name = "EDEN_NX2_INDIRECT_DISPATCH",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off (on for Ocarina of Time)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the GPU split and its macros on the first GPU thread, a compute dispatch whose "
            "grid size the GPU wrote into the command list (NVN's grid size macro) reads it from "
            "GPU memory, instead of the first thread waiting for the GPU to finish the dispatch "
            "that wrote it. 1 turns it on for every title, 0 off, Ocarina of Time included "
            "(nemyu-coco)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer GPU-thread waits on culling dispatches: nemyu-coco measured Ocarina of Time in "
            "Link's house at 26.8 instead of 28.7 ms a frame together with "
            "EDEN_NX2_DEFER_CONDITIONAL (i7-12650HX, RTX 4050 Laptop), on a build that drained "
            "for these lists; Nemyu reads most of them from copies, so the gain may be smaller."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Recognises the macro by its code: a game whose grid size macro does more than keep "
            "the size would dispatch the wrong grid. The log names the macro the first times "
            "(\"Compute macro at ... takes GPU-written arguments\")."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "The Legend of Zelda: Ocarina of Time, all Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_INDIRECT_DRAWS",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Draws the indirect draws of Switch 2 draw macros straight from GPU memory, instead "
            "of reading their arguments on the CPU and running the macro."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 waits for all queued GPU work about 8 times a frame, so CPU and GPU stop "
            "overlapping (Mario Kart World took ~28-30 ms a frame)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_INLINE_DEFER",
        .category = Category::GpuCommands,
        .type = ValueType::Choice,
        .choices = INLINE_DEFER_CHOICES,
        .suggested = "2",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Inline GPU memory writes are applied at once, but invalidate the other caches merged "
            "at the next cache flush instead of one write at a time. At 2 the texture cache is "
            "still told at once, so a CPU read or a present before that flush can't undo them or "
            "show the old image."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "2 can fix images that briefly show their old contents after inline writes."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 invalidates every cache for each of thousands of small writes between draws; 2 "
            "takes a texture cache lock per write."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_INLINE_INVALIDATE",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The texture, query and shader caches only hear about inline writes on pages they "
            "held something on when last asked; the buffer cache hears about all of them."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less GPU thread time per draw (up to about 2% in Donkey Kong Bananza, per Lichii)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Experimental. A miss would show as a stale texture, shader or occlusion result; "
            "Selevet lists it among the switches to remove if objects flicker."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_INVALIDATE_BLOCKS",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value =
            QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On (off for Ocarina of Time)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Invalidation ranges cover whole 32-byte blocks; the old rounding could end a range "
            "short of the write."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Writes past the start of a block are not left out of the invalidation."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The rest of a block may hold GPU-written bytes guest memory doesn't have yet, which "
            "the invalidation reloads stale: Fritchii v5.1 took it back for glowing geometry in "
            "Ocarina of Time's Hyrule Field. 0 can leave stale data after a write instead."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_INVALIDATE_DEDUP",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Skips an invalidation range that lies inside one of the last few already waiting."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 keeps the duplicates: more invalidation work."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_KEEP_MISREAD_HEADERS",
        .category = Category::GpuCommands,
        .type = ValueType::Presence,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Runs a command header that names a method at 0x1000 or above, which no GPU class "
            "has, instead of dropping the rest of its entry."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A misread word can run as a method thousands of words long that swallows the next "
            "entries, semaphore releases among them: channels then wait for good (0 fps)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_KEEP_REWRITTEN",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When a fence's buffer downloads reach guest memory, ranges the GPU wrote again after "
            "the download was copied stay marked as GPU data, so the next fence downloads them "
            "and readers flush them (nemyu-coco)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 forgets those writes as before: readers can get older bytes, such as an older "
            "GPU-written command list (nemyu-coco: crashes entering Ocarina of Time's Kokiri "
            "Forest, ~3% faster there from the downloads it skips)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_LIST_WORDS",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A command list on a GPU-written page reads from the page's GPU copy only the words "
            "compute or copy work wrote, and the others from guest memory, once the page was "
            "flushed: a list beside those words waits for no copy."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer waits and drains for lists the CPU writes next to GPU-written ones. Upstream "
            "measured 93 to 156 ms a second of such waits in Ocarina of Time."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A GPU write the buffer cache doesn't see would leave a list with old words; 0 reads "
            "whole copies as before, for comparisons."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Ocarina of Time, Mario Kart World"),
    },
    {
        .name = "EDEN_NX2_MACRO_COPY",
        .category = Category::GpuCommands,
        .type = ValueType::Presence,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Always passes macro parameters through a growing list instead of a stack copy."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None; for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "About 2% of the GPU thread."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_MME_JIT",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Runs Switch 2 GPU macros compiled to x86-64 code instead of through the macro memo "
            "and the interpreter (x86-64 CPUs only)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 rules the compiler out when a macro bug is suspected."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 makes each call ~18% slower (174 against 148 ns) and also takes the macros off "
            "the split's first thread."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_MME_MEMO",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Replays the writes of repeated macro calls instead of interpreting them again (when "
            "the compiler is off, and for compute macros)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 rules the memo out when a macro bug is suspected."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 interprets every call: a slower GPU thread."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PAGE_FILTER",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "GPU writes skip the texture, query and shader caches when none of their pages ever "
            "held an image, query or shader there. 0 visits them every time, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less GPU thread time per draw: Ocarina of Time does about 88,000 of these a second "
            "(52 to 54 fps in Kokiri Forest on an RTX 4090, per Lichii)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None expected; 0 is for comparisons. A miss would show as a stale texture, shader "
            "or occlusion result."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PARK_SPLIT_METHODS",
        .category = Category::GpuCommands,
        .type = ValueType::Presence,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Same as EDEN_NX2_SPLIT_METHODS=park, and wins over it."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "See EDEN_NX2_SPLIT_METHODS."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "See EDEN_NX2_SPLIT_METHODS."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PASS_BARRIER_FRAGMENT",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The barrier at the end of a render pass (Vulkan) holds back only the fragment and "
            "compute stages of the work after it, so the next pass's vertex work can start while "
            "this one's last fragments are written."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less GPU idle time between passes in games that end many small ones (Splatoon "
            "Raiders' pier ran ~21,000 render passes a second). Not measured on its own."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Wrong if a vertex, tessellation or geometry shader reads an attachment the previous "
            "pass just wrote: flicker or stale geometry there."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Splatoon Raiders"),
    },
    {
        .name = "EDEN_NX2_PBFLUSH",
        .category = Category::GpuCommands,
        .type = ValueType::Choice,
        .choices = PBFLUSH_CHOICES,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Flushes small GPU-written parameter blocks (64 words or fewer) of macro and compute "
            "calls before reading them from command lists. With the split, 1 waits for the second "
            "thread only for blocks on pages GPU work has written; 2 waits for every block."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Parameters the GPU has just written are read current."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Extra flushes on the GPU thread; 2 also waits for the second thread at every block."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PB_ACQUIRE_COPY",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Copies the rest of a command list from memory again at every met semaphore acquire, "
            "as before, instead of only when its channel resumes from a suspension."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "None expected; it brings back the old behaviour, for "
                                   "comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The copy is read at parse time, when the game may have refilled the command ring "
            "for a later frame: misread commands, then channels waiting for good (0 fps)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PB_RESNAPSHOT",
        .category = Category::GpuCommands,
        .type = ValueType::Choice,
        .choices = PB_RESNAPSHOT_CHOICES,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "After a channel waited on a semaphore, copies its command lists from memory again, "
            "since the game may have rewritten them in the meantime."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None expected; lower values are for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lower values brought back freezes from stale commands (Donkey Kong Bananza's first "
            "cave, Hyrule Warriors fights)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PB_RESNAPSHOT_AFTER",
        .category = Category::GpuCommands,
        .type = ValueType::Choice,
        .choices = PB_RESNAPSHOT_AFTER_CHOICES,
        .suggested = "cpu",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "all"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Which semaphore releases make a resumed channel copy its command lists from memory "
            "again. A release the emulated GPU wrote comes when the GPU thread gets to it, maybe "
            "long after the game wrote the commands; cpu keeps the copies taken so far then."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With cpu, no copies of memory the game already refilled for a later frame (Lichii: "
            "Ocarina of Time's start of gameplay ran clean 9 times of 9, misread commands every "
            "time before)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With cpu, a game that writes commands after submitting them and lets GPU work release "
            "them would run the old words."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles; for Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_PB_RESNAPSHOT_IN_TRANSIT",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With EDEN_NX2_PB_RESNAPSHOT=2, a channel that resumes from a semaphore wait also "
            "copies again the command lists the game submitted to it that the GPU thread has not "
            "queued yet, instead of when the GPU thread queues them."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer misread commands and freezes when the GPU thread runs far behind the game "
            "(the start of Ocarina of Time gameplay while pipelines build)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A list on its way through several waits is copied at each. Not tested in Nemyu yet."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PB_SNAPSHOT",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Copies Switch 2 command lists when the game submits them, instead of reading them "
            "from memory when they run (unless a GPU map is still queued for them)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With 0, the end of a frame can run with the next frame's commands and show an image "
            "three frames old."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PENDING_POLL_US",
        .category = Category::GpuCommands,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 1000,
        .suggested = "50",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "50"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "While GPU channels wait on semaphores the CPU releases, check them this often, in "
            "microseconds, for up to 2 s; 0 waits about a millisecond between checks."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A frame no longer waits out a millisecond per semaphore the CPU releases (Lichii: "
            "Ocarina of Time ~11% faster in Kokiri village)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "More CPU wakeups on the GPU thread while channels wait."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PENDING_WAKE",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "While GPU channels wait on semaphores, the GPU thread waits on its command queue "
            "instead of sleeping a fixed millisecond between checks."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 adds up to a millisecond to each such wait."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PERF_OFF",
        .category = Category::GpuCommands,
        .type = ValueType::Text,
        .suggested = "method,cb_batch",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Comma-separated list of the GPU thread optimizations from Lichii to turn off: method "
            "(faster 3D method dispatch on the second thread), cb_batch (uniform words uploaded "
            "in one write), index_rebind (repeated index buffer binds skipped), usage_tracker "
            "(buffer usage pages not filled again), sorted_page_batches (sorted page runs not "
            "sorted again), unswizzle_workers (large textures with an odd row size unswizzled half "
            "on a texture thread), ssbo_bindings (storage buffer bindings kept until the memory "
            "map changes), native_bank_upload (native 64 KiB uniform banks uploaded again without "
            "unprotecting their pages). Lichii's names for its other optimizations are accepted "
            "and ignored."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None expected: they give the same results. For finding one at fault."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Each one off costs some GPU thread time."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PERF_ON",
        .category = Category::GpuCommands,
        .type = ValueType::Text,
        .suggested = "fragment_barrier",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "async_pipelines for Ocarina of Time (EDEN_NX2_PERF_OFF=async_pipelines turns it off), "
            "none for other titles"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Comma-separated list of the GPU thread optimizations from Lichii that are off by "
            "default to turn on. fragment_barrier: a fragment barrier ends the render pass only "
            "at the next draw that reads one of the render targets, or at any draw once the pass "
            "stored to memory. async_pipelines: a new pipeline of a Switch 2 title has its shader "
            "code generated by the pipeline build (a builder thread, or the thread that records "
            "the draw) instead of the GPU thread. split_overlap: as EDEN_NX2_DMA_RANGES=1, "
            "unsafe (see there). forward_reads: a GPU macro on the split's first thread that "
            "reads the words of a uniform upload it sent itself, still queued and the last "
            "memory write sent, takes them from the upload instead of waiting for the second "
            "thread to catch up. front_sleep: the split's first thread sleeps until the second "
            "one moves on in a long wait, instead of yielding (on by default in Lichii)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer render passes and less GPU work (Ocarina of Time ended about 5,900 a second at "
            "its fragment barriers, per Lichii). Shorter GPU thread stalls when new pipelines "
            "are built. Fewer waits between the two GPU threads (forward_reads: Frizti counted "
            "9,600 a second in Ocarina's forest)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Experimental. fragment_barrier: a draw that reads earlier output some other way "
            "would see it old (flicker or stale effects). async_pipelines: with asynchronous "
            "shaders on, more draws may be skipped while their pipelines build. forward_reads: "
            "a draw queued after the upload that writes the same memory is not waited for. "
            "front_sleep: a wake-up costs some latency after each long wait."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "The Legend of Zelda: Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_PERF_OPT",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 turns off every GPU thread optimization from Lichii (see EDEN_NX2_PERF_OFF and "
            "EDEN_NX2_PERF_ON), including any EDEN_NX2_PERF_ON names."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 costs GPU thread time."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PUBLISH_AFTER_WAIT",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the GPU thread split, the first thread hands its commands to the second once it "
            "holds 8 words while the second has run out of work, instead of after 256."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The second thread waits less in the middle of a long command list: upstream measured "
            "49 of its 131 ms a second of waiting in Ocarina of Time on words the first held."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None expected; 0 brings back the old publishing, for comparisons."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_QUERY_RESULT_WAIT",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog", "On for Ocarina of Time, off for other titles"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When a GPU macro reads occlusion query results the host GPU has not finished yet, "
            "waits for them (at most 200 ms) instead of reading the old value. 1 turns it on for "
            "every title, 0 off, Ocarina of Time included."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Stops foliage and terrain flashing overexposed for a few frames when the camera "
            "moves (Ocarina of Time)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The GPU thread waits for the host GPU at those reads; one tester saw more audio "
            "cutouts."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_RELEASE_DRAIN_SKIP",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the split, a semaphore acquire still unmet after draining the second thread "
            "releases the fences there and waits for it again. On, it skips that wait when the "
            "idle second thread found nothing to release; the GPU cache is still invalidated in "
            "order. Not with Balanced or Accurate fences."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Two fewer waits for the second thread per acquire still unmet after the drain "
            "(Ocarina of Time: about 50-230 a second); the log counts them as \"Fence release "
            "skip\"."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Experimental, not measured yet. The second thread checks the fence manager each "
            "time it goes idle."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_RESETFIX",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Does the stores of Donkey Kong Bananza's terrain reset program on the CPU: the "
            "recompiler loses them, so every rock and dirt piece kept zero copies and wasn't "
            "drawn."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 brings back the missing terrain."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Donkey Kong Bananza"),
    },
    {
        .name = "EDEN_NX2_RESETFIX_EARLY",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Does the terrain reset of EDEN_NX2_RESETFIX before the pipeline lookup instead of "
            "inside draw setup."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 uses the older, slower checks."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Donkey Kong Bananza"),
    },
    {
        .name = "EDEN_NX2_SKIP_EMPTY_DRAWS",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Skips direct draws with no vertices, or no instances outside task/mesh pipelines, "
            "before their pipeline, buffer and descriptor setup. Draws with transform feedback "
            "on still go through."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "About 2% more FPS at Donkey Kong Bananza's Ingot Isle (15.0 to 15.35 upstream)."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                       "0 prepares these draws like any other, as before."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Donkey Kong Bananza"),
    },
    {
        .name = "EDEN_NX2_SKIP_SAME",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Drops 3D state register writes that keep the value the register already holds "
            "(about 60% of them in Mario Kart World)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 makes macro calls and draw setup a little slower."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_SPLIT",
        .category = Category::GpuCommands,
        .type = ValueType::Choice,
        .choices = SPLIT_CHOICES,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2 on Switch 2 titles"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Splits the GPU thread of Switch 2 titles in two: the first parses command lists "
            "(and, at 2, runs the 3D and compute macros), the second runs the engine methods and "
            "records draws."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 or 1 rule the split out when a GPU bug might come from it."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "At 2, Mario Kart World drew ~1.46x more per second with ~17% shorter frames; 0 "
            "gives that up and 1 part of it."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_SPLIT_METHODS",
        .category = Category::GpuCommands,
        .type = ValueType::Choice,
        .choices = SPLIT_METHODS_CHOICES,
        .suggested = "park",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "continue (auto for the Yoshi demo)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "What to do when a GPU method's data goes on in a command list entry that starts "
            "elsewhere: continue there, park it until the rest arrives, or let a heuristic "
            "decide. Setting it applies to every title, the Yoshi demo included."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "park or auto may fix misread commands in a title that splits methods that way."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Both are experimental and can stall the command stream."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_SPLIT_SKIP",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the GPU thread split, state writes that keep a register's value are not sent "
            "to the second thread."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 sends ~55% more 3D state writes through the ring."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_TITLE_DEFAULTS",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Gives switches the defaults some titles have instead of the global ones (Ocarina of "
            "Time: GPU_TIMESTAMP=ns, NATIVE_SSBO_MAPPED=1, NATIVE_UBO_REUPLOAD=2, "
            "COMPUTE_TABLE_256=1, USED_SESSIONS=8; Mario Kart "
            "World: NATIVE_SSBO_MAPPED=0, AMD_PARALLEL_PIPELINES=1; Tears of the Kingdom: "
            "LEGACY_SECTOR_LAYOUT=1). A switch you set still wins."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; 0 runs a title with the global defaults, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 brings back what those defaults avoid: in Ocarina of Time a lower dynamic "
            "resolution, draws reading past their bindings and flashes; Mario Kart World took "
            "28-31 ms a frame instead of 21-22."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "Ocarina of Time, Mario Kart World, Tears of the Kingdom"),
    },
    {
        .name = "EDEN_NX2_ZPASS_SUBMIT_RESET",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Resets the occlusion sample counter at each GPU submission instead of pausing it."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Drops the samples counted between a game's counter reset and its report when a "
            "submission lands in between."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_ASID_RESTORE_LEGACY",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps comparing against the first page's address space after a cross-process run, "
            "as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Hands the pages after such a run to the wrong process."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_BUFFER_DOWNLOADS",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Off on Switch 2 titles, on for others"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Copies every GPU-written buffer range back to guest memory at the next fence."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "1 on a Switch 2 title may fix data the CPU reads back stale, if the read traps miss "
            "it."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "In Mario Kart World it copied 250-330 MB/s back: fences took ~5 µs instead of ~1, "
            "and draw setup ~7% longer."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_BUFFER_SKIP",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps each vertex stream's buffer lookup while its registers, the page table and the "
            "buffers stay the same, skips the upload check of vertex buffers no CPU write reached, "
            "and touches each buffer's LRU entry once a frame."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less GPU thread time per draw (vertex binding took 12% of it in Donkey Kong "
            "Bananza, per Lichii)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Experimental. A missed CPU write would leave stale vertices (stretched or flickering "
            "geometry); EDEN_NX2_FAST_DRAW_VERIFY=1 counts them."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_COMMIT_ALL",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Commits all emulated memory at start instead of as the game touches it (Windows)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "No commit failure later in a session."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Needs up to 20 GiB of free commit (RAM plus page file) at boot."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TRIM_ZERO",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A low-priority thread takes the emulated memory pages that hold only zeros out of "
            "the working set, every 5 s while it finds 64 MiB or more, else up to once a minute "
            "(Windows). 0 leaves them in RAM."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less RAM in use: games clear their heaps while booting (Lichii: Ocarina of Time left "
            "about 7 GB of zero pages, and a 16 GB PC had 0.2-0.6 GB free)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Each pass reads the resident zero candidates; a page used again after a trim costs a "
            "page fault, a slow one if Windows wrote it to the page file meanwhile."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_DEVICE_RANGE_CHECK",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Destroyed Switch 2 GPU memory releases its device ranges, and a CPU range mapped "
            "again reuses its device range only while it still reaches the same pages."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Stops a stale device range from being reused for memory mapped again."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Not compared yet on the titles it touches, so it stays off by default."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_EXACT_PREFLUSH",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Of a GPU write that covers pages the CPU has read, only those pages are queued for "
            "download at fences."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 copies whole buffers back at every fence (gigabytes a second with Mario Kart "
            "World's compute bindings)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PREFLUSH",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Buffer memory the guest CPU read after the GPU wrote it is copied back at every "
            "fence from then on. 0 copies it back only when the CPU reads it again after the "
            "next GPU write, together with the GPU writes in every range read that way before "
            "(Lichii's default for Switch 2 titles)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0: no copies at fences for data the CPU no longer reads. Lichii measured 26 to 31 "
            "FPS in Ocarina of Time's Kokiri Forest from taking the read ranges together."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0: each such read waits for the GPU, which can stall the reading core in titles that "
            "read buffers back every frame."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "The Legend of Zelda: Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_PREFLUSH_READ_WAIT",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off; on in Ocarina of Time"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A CPU read of buffer memory that is copied back at every fence (EDEN_NX2_PREFLUSH) "
            "waits for the GPU when the GPU wrote it after the last copy, instead of reading the "
            "last copy."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The CPU never reads data a frame old; it was meant for Ocarina of Time's white "
            "flashes near the sword boulder."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Each such read stalls the reading core: Mario Kart World waited 2 to 3 s every 10 s "
            "of a race with it."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "The Legend of Zelda: Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_READBACK_FENCE_COPY",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A CPU read of GPU-written buffer memory that a fence already copied back since the "
            "GPU last wrote it takes that copy once the GPU is past it, instead of copying the "
            "memory again and waiting for the GPU to finish everything submitted. The log says "
            "every 10 s how many reads took a fence copy."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shorter stalls of the reading core and the GPU thread: the read waits for the copy "
            "made at the fence, not for the work queued after it. Ocarina of Time waited 170 to "
            "660 ms every 10 s on 120 to 310 such reads."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Not measured yet. Only reads that wait for the GPU and whose data a fence copied "
            "gain (Ocarina of Time with EDEN_NX2_PREFLUSH_READ_WAIT, titles with "
            "EDEN_NX2_BUFFER_DOWNLOADS); tracking the copied ranges costs a little on every GPU "
            "write while it is on."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "The Legend of Zelda: Ocarina of Time, Switch 1 titles"),
    },
    {
        .name = "EDEN_NX2_ASYNC_READBACK",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A CPU read of GPU-written buffer memory has the GPU thread submit the copies back and "
            "go on, instead of waiting for the host GPU with the buffer cache locked. The reading "
            "core waits for the copies and writes them to guest memory itself. A later read of "
            "bytes already on their way only waits for them, and GPU-side writes over them (DMA, "
            "semaphores, query results) let them land first. With EDEN_NX2_GPU_PROFILE set, the "
            "log says every 10 s how many reads went this way and how long they waited (Lichii "
            "#1028)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The GPU thread no longer stalls in read flushes: Lichii measured 80 to 90 ms a "
            "second less on Ocarina of Time's GPU thread and 10 to 15% more frames in Ocarina of "
            "Time and Mario Kart World. Not measured in Nemyu yet."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Not measured in Nemyu. The reading core still waits for the host GPU when its copies "
            "have not run. Reads no longer take EDEN_NX2_READBACK_FENCE_COPY's fence copies. "
            "OpenGL keeps the synchronous flush."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "The Legend of Zelda: Ocarina of Time, Mario Kart World"),
    },
    {
        .name = "EDEN_NX2_FENCE_RANGE_MERGE",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Merges the GPU-modified range sets of committed fences instead of subtracting each "
            "from the earlier ones."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 does more range work at every fence."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_GPU_ONLY_LAZY",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Backs Switch 2 GPU-only memory regions with host memory as they are used, instead "
            "of whole."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 commits the whole regions: much more RAM."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_GPU_ONLY_ZERO_SKIP",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Leaves zero writes to GPU-only regions without host memory."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less memory committed for regions that are only cleared."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Opt-in and little measured."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_HELD_WRITES",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Guest CPU writes to GPU-written memory are applied before every submitted command "
            "list, copying only the written bytes."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 applies them only at fences, marking whole pages CPU-modified and uploading them "
            "over GPU writes that were never copied back."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_CPU_WRITE_DEFER",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A guest CPU write that finds a GPU cache busy is held with the other held writes "
            "instead of waiting for the cache. Off with EDEN_NX2_HELD_WRITES=0."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The game's CPU threads stop waiting for draws and uploads on the GPU thread."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 is for comparisons: the writes wait for the cache lock, up to tens of "
            "milliseconds."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_HELD_WRITE_BATCH",
        .category = Category::BufferCache,
        .type = ValueType::Choice,
        .choices = HELD_WRITE_BATCH_CHOICES,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Applies the held writes of one cache invalidation as one batch, each cache locked "
            "once for all of them; 1 also joins touching writes within a 4 KiB page."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer lock handoffs that wake guest CPU threads."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Not measured yet."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_INDEX_SYNC_SKIP",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Skips the upload check of an index buffer range no CPU write reached since it was "
            "last checked, as EDEN_NX2_VERTEX_SYNC_SKIP does for vertex buffers. "
            "EDEN_NX2_FAST_DRAW_VERIFY=1 counts the uploads a skip would miss."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less GPU thread time per indexed draw. Not measured here; Lichii ships it on as part "
            "of a package that took Donkey Kong Bananza from 11.65 to 14.83 fps."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A missed CPU write would leave stale indices (spiky or missing geometry)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_LAYOUT_CACHE",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps the GPU memory kind run that the last storage buffer layout query fell in, "
            "instead of locking and walking the map for every buffer of every draw."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 is slower."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_LOW_COMMIT_WARNING_18G",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .flags = GuiOnly,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Warns about low virtual memory when less than 18 GiB can be committed instead of 12 "
            "GiB (Switch 2 titles)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "An earlier warning on machines that run out."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The old threshold warned on normal boots."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_COMPUTE_TABLE_256",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "On for Ocarina of Time, off for other titles"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Binds read-only pools that compute programs reach through a pointer table up to 256 "
            "MiB past the pointer instead of 64 MiB (within the device's limit). Draws keep 64 "
            "MiB. 1 turns it on for every title, any other value off, Ocarina of Time included."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Ocarina of Time's fog finds its light probes, 104-122 MiB past their pointer, and "
            "keeps its sky ambient instead of turning into a white haze outdoors."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Larger bindings: more memory to upload and track the first time they are used."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_NATIVE_SSBO_MAPPED",
        .category = Category::BufferCache,
        .type = ValueType::Choice,
        .choices = NATIVE_SSBO_MAPPED_CHOICES,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "1 for Ocarina of Time, 0 for Mario Kart World, w for other titles"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Which native storage buffer bindings are sized from their memory mapping (up to 8 "
            "MiB) instead of the size word after their pointer."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mapped bindings keep writes and reads past the stated size (Donkey Kong Bananza's "
            "command lists, Ocarina of Time's draws)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mapping them all costs Mario Kart World and Donkey Kong Bananza about a third of "
            "their frame rate (Mario Kart World: 28-31 against 21-22 ms a frame)."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Ocarina of Time, Mario Kart World, Donkey Kong Bananza"),
    },
    {
        .name = "EDEN_NX2_NATIVE_SSBO_WRITTEN_SIZE",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .flags = NotInThisBuild,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When the word after a native binding's pointer reads as a smaller size, only that "
            "much is marked GPU-written."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 marks all of it, which also covered CPU-written data past the buffer (Pokopia "
            "lost the GPU device within a minute)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Pokopia"),
    },
    {
        .name = "EDEN_NX2_NATIVE_UBO_REUPLOAD",
        .category = Category::BufferCache,
        .type = ValueType::Choice,
        .choices = NATIVE_UBO_REUPLOAD_CHOICES,
        .suggested = "2",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "2 for Ocarina of Time, 0 for other titles"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Refreshes native 64 KiB uniform banks, in case writes to them went unnoticed. 1 "
            "uploads each bank again once per frame. 2 compares the bank's pages with their "
            "contents at the last bind on every bind and uploads the ones that changed. 0 is "
            "off, Ocarina of Time included."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "May fix stale data and flicker (the corners of Splatoon Raiders' dialogue box). 2 "
            "also catches data rewritten between two draws of a frame: Ocarina of Time's "
            "one-frame flashes on leaves and Link's tunic (Eglantine measured about 1 in 600 "
            "presents at 1, none in 1800 on every bind)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "1: an extra upload per bank and frame. 2: a compare of up to 64 KiB per bind, an "
            "upload whenever a page changed, and up to 64 MiB of page copies."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Ocarina of Time, Splatoon Raiders"),
    },
    {
        .name = "EDEN_NX2_PRIVATE_DEVICE_RANGES",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Each GPU address space of a Switch 2 game maps memory outside the heap at a device "
            "range of its own, as before, instead of one range shared by the process. On with "
            "EDEN_NX2_DEVICE_RANGE_CHECK=1."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog", "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A game with two address spaces can draw into one copy of the memory while the screen "
            "shows another (Fortune's Weave stayed black in Eglantine)."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Switch 2 titles with two GPU address spaces (Welcome Tour, Yoshi demo, Fortune's "
            "Weave)"),
    },
    {
        .name = "EDEN_NX2_REACTIVE_FLUSHING",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Off on Switch 2 titles unless set per game"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Forces reactive flushing for Switch 2 titles, over the per-game setting."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "1 keeps every GPU-cached page CPU-visible: a fallback when the read traps miss "
            "something."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "1 read-protects every cached page (~12% of Mario Kart World's main core went to "
            "slow reads) and turns the read traps off."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_READ_AHEAD",
        .category = Category::BufferCache,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 2,
        .suggested = "2",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "What a guest read of GPU-written buffer memory flushes besides its page. 1: when a "
            "guest thread scans page by page, a growing window ahead of it (up to 1 MiB). 2: all "
            "GPU writes in the 1 MiB block of the buffer around the page."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer waits for the GPU thread in such scans; with 2 also for scattered reads "
            "(Mario Kart World)."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Flushes data that may never be read."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_READ_FLUSH_HELD_WRITES",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Applies the held writes before a guest CPU read flushes a range, so the download "
            "can't put the GPU's older copy over them."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Can fix stale data the CPU reads back."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Not compared yet on the titles it can change, so it stays off by default."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mario Kart World, Donkey Kong Bananza, Pokopia"),
    },
    {
        .name = "EDEN_NX2_READ_REQUEST_EARLY",
        .category = Category::GpuCommands,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A guest CPU read of GPU-written memory is flushed after the next compute dispatch "
            "too, as after the next draw, and with the split the first thread hands it to the "
            "second between command list entries instead of after the whole list."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The reading core waits less for its turn on the GPU thread: Ocarina of Time's reads "
            "spent 3.3 of their 4.7 s outside the flush itself, Mario Kart World's half."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Not measured yet. Flushes can land in the middle of a list, so a read can end a "
            "render pass that the next draws would have continued."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_READ_TRAPS",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Takes read permission off pages the GPU wrote and doesn't copy back, so the guest's "
            "first read flushes them (fastmem only; not under NCE or with reactive flushing)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 sent 8x fewer GPU submissions in one test."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 leaves animated colliders at their default pose in Mario Kart World (Dino Dino "
            "Jungle's dino, DK Spaceport, Bowser's Castle)."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "All Switch 2 titles, Mario Kart World above all"),
    },
    {
        .name = "EDEN_NX2_RELEASE_GUEST_RAM",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Frees the guest RAM (every page the game touched, up to 12 GiB) when a Switch 2 game "
            "stops, instead of keeping it for the next game."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 keeps that memory until the next game reuses it or Nemyu closes."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_RUN_CACHE",
        .category = Category::BufferCache,
        .type = ValueType::Choice,
        .choices = RUN_CACHE_CHOICES,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How the GPU memory manager remembers how far the mapping runs on from an address, "
            "asked at every native storage buffer bind (about 90,000 times a second in Ocarina of "
            "Time's Kokiri village)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None expected; 0 and 2 are for comparisons and checks."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 misses at every new address; 2 walks the page tables for every answer."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_SELECTED_WRITE_BOUND",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A storage buffer written only through addresses picked at run time is marked "
            "GPU-written up to the next buffer of the same program instead of over its whole span "
            "(up to 8 MiB)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer copies back to guest memory at fences (Ocarina of Time: 289 MB/s, per "
            "Lichii)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Experimental. A write past the bound would not reach the CPU: missing vegetation or "
            "objects that read GPU results."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_SPARSE_LAZY",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Backs Switch 2 sparse memory regions with host memory as they are used, instead of "
            "whole."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 commits the whole regions: much more RAM."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_SPARSE_REMAP",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Reports remap support to the Switch 2 GPU driver, so titles can map and unmap "
            "sparse texture tiles."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 makes Mario Kart World stream its terrain into unmapped sparse memory."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Mario Kart World"),
    },
    {
        .name = "EDEN_NX2_STAGING_LEGACY",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Staging buffers with power-of-two sizes, no reuse of larger ones, and one size "
            "class checked per frame, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "More staging memory and allocations."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_STREAM_BUFFER_256",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps the 256 MiB stream buffer on GPUs with 7 GiB or more instead of 128 MiB."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Room for titles that stream more data per frame."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "128 MiB more video memory; 256 MiB sat mostly unused on Switch 2 titles."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_UNIFORM_STREAM",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A uniform binding (up to 64 KiB) whose CPU-written data would have to be uploaded in "
            "the middle of a render pass is copied into the stream buffer and bound from there, "
            "up to 16 MiB a frame. Vulkan."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer render passes split by uploads, each a full GPU drain: Splatoon Raiders' hub "
            "split 4-7 a frame for uniform data (the author's measurement)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The data is copied again for every draw that uses it until the buffer itself is "
            "uploaded."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Splatoon Raiders"),
    },
    {
        .name = "EDEN_NX2_UBOREUSE",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Reuses an uploaded small uniform buffer when the same data comes again, instead of "
            "copying it every time."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 copies every small uniform buffer."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_UBO_INPLACE",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Compares a small uniform buffer with its last upload straight in guest memory instead "
            "of copying it to a scratch buffer first (with EDEN_NX2_UBOREUSE)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "One copy less per small uniform buffer bound on the GPU thread, none when unchanged: "
            "27.6 to 8.9 ns for an unchanged 256-byte buffer in Lichii's microbenchmark."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 copies each buffer to a scratch buffer first, as before. None expected, the "
            "uploaded bytes are the same."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_UPLOAD_BARRIER",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Records a barrier after mapped buffer uploads anyway; they carry their own, and "
            "this one only ended the render pass."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "More render pass splits."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_UPLOAD_BATCH",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Records a run of in-order uploads from the stream buffer as one command between one "
            "pair of barriers, instead of a pair of barriers around each upload. An upload that "
            "writes bytes a pending one writes, or reads a buffer one writes, records the run "
            "first; a run holds at most 64 copies."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer barriers on the GPU: Ocarina of Time makes ~600 such uploads a frame "
            "(~1,200 barriers)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 records each upload with its own barriers, as before. A missed overlap would show "
            "as stale or mixed buffer contents."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles; mostly Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_UPLOAD_ORDER",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Puts an upload ahead of its submission's work unless work recorded before it in "
            "that submission uses its bytes, with a barrier on earlier submissions still running. "
            "Before, an upload went ahead only while its buffer's usage bits were clear, and they "
            "were reset only once the buffer's last use finished."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer render pass breaks for uploads: about 48 a frame in Ocarina of Time, each "
            "~12 us of GPU time plus the attachments' store and load."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 decides by the usage bits, as before. A missed use would show as a buffer "
            "updated too early (flicker or wrong geometry for a frame)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles; mostly Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_VERTEX_SYNC_SKIP",
        .category = Category::BufferCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Skips the upload check of a vertex buffer range no CPU write reached since it was "
            "last checked (the upload half of EDEN_NX2_BUFFER_SKIP, which turns it on too). "
            "EDEN_NX2_FAST_DRAW_VERIFY=1 counts the uploads a skip would miss."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less GPU thread time per draw: the check was ~6% of it in Donkey Kong Bananza."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 checks every vertex buffer on every draw, as before. A missed CPU write would "
            "leave stale vertices (stretched or flickering geometry)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_ALIAS_WALK_LEGACY",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Looks through an image's aliases for newer contents every time it is used, instead "
            "of only after one of them was written, synchronized or linked."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Slower draws in titles with many aliased textures (Donkey Kong Bananza)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Donkey Kong Bananza"),
    },
    {
        .name = "EDEN_NX2_ASTC_ARRAY_MIB",
        .category = Category::TextureCache,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 65536,
        .suggested = "512",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "512 MiB, 256 MiB on GPUs with less than 7 GiB"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The most a trimmed ASTC texture array keeps on the host GPU, in MiB."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Higher keeps more terrain detail resident on large GPUs."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mario Kart World's two terrain arrays took 724 MiB of a 6 GB card."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Mario Kart World"),
    },
    {
        .name = "EDEN_NX2_ASTC_BC1",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When ASTC textures are recompressed to BC3, those without alpha (about 70% of them) "
            "go to BC1 instead."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 keeps a little more quality in opaque textures."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 doubles their memory: more evictions and decodes (Mario Kart World's attract "
            "loop: 17 against 22-23 fps)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_ASTC_FLUSH",
        .category = Category::TextureCache,
        .type = ValueType::Choice,
        .choices = ASTC_FLUSH_CHOICES,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "No wait"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "What a GPU ASTC decode does once recorded: nothing (a barrier orders later uses), "
            "submit, or wait for the host GPU to go idle."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "1 or 0 rule out an ordering problem in texture decodes."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 stalls the GPU thread at every decode; 1 adds submissions."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_ASTC_VIEW_LEGACY",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lists the A8B8G8R8 view for recompressed ASTC images and decodes ASTC for the BC "
            "encoder as A8B8G8R8, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Brings back the old format handling."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_CLEAR_SKIP_UPLOAD",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Skips uploading a new render target's guest data when a clear overwrites the whole "
            "image."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 uploads data the clear throws away."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_FB_LAYERS_MAX",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Gives framebuffers the most layers of their attachments and clears the requested "
            "layers unclipped, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Clears can write past the framebuffer's layers."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_FEEDBACK_COPIES",
        .category = Category::TextureCache,
        .type = ValueType::Choice,
        .choices = FEEDBACK_COPIES_CHOICES,
        .suggested = "exact",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "coalesce"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When a draw samples one of its own colour targets through an alias of another size "
            "or format, copies the target into the alias once per guest texture barrier, "
            "render-target set and other write to the target, instead of before every draw."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "exact: none expected; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "exact: a full-screen copy for every such draw (~1,600 a frame at Donkey Kong "
            "Bananza's Ingot Isle, 9 FPS instead of 15 upstream)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Donkey Kong Bananza"),
    },
    {
        .name = "EDEN_NX2_FEEDBACK_DIRTY_TARGETS",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With EDEN_NX2_FEEDBACK_COPIES coalescing (Vulkan), a feedback copy may also be "
            "skipped while the draw's render targets are marked dirty; once they are set, it is "
            "made after all if they changed. Only for an alias scaled like its target."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Splatoon Raiders' pier sets its targets again before each of ~480 draws a frame that "
            "sample an 880-wide view of their 896-wide target: each was an image copy ending the "
            "render pass."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Opt-in: a copy made after the targets are set may come later than one made before "
            "them would have; stale or flickering feedback effects would point at it."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Splatoon Raiders"),
    },
    {
        .name = "EDEN_NX2_FORCE_LOD0",
        .category = Category::TextureCache,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Samples only the top mip level of every texture."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shows whether a glitch comes from the mip layout."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Shimmering, aliased textures."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_IGPU_HEAP_BUDGET",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "On integrated GPUs, follows the memory heap's budget instead of capping it at the "
            "share of RAM the integrated GPU gets (4 GiB, 6 GiB with aggressive VRAM use)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "More texture memory on APUs with plenty of RAM."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The texture cache can fill that much RAM before it collects, and ASTC stays "
            "uncompressed."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles on integrated GPUs"),
    },
    {
        .name = "EDEN_NX2_KEEP_ASTC",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps ASTC textures uncompressed on Switch 2 titles when ASTC recompression is set "
            "to Uncompressed and the GPU has less than 12 GiB, instead of recompressing them to "
            "BC3."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Full texture quality."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Uncompressed ASTC is 4x larger: running out of video memory evicts and decodes "
            "textures again (stutter, pop-in)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_LEGACY_SECTOR_LAYOUT",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "On for Tears of the Kingdom and the Xenoblade Switch 2 Editions, off for other titles"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Reads and writes block-linear textures whose descriptor sets bit 88 in the legacy "
            "Tegra sector order instead of the Switch 2 one, along with the DMA copies that fill "
            "them."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Tears of the Kingdom's movie and menu textures are no longer scrambled, nor the "
            "Xenoblade Switch 2 Editions' colour grading, fonts and textures (Orchid's finding)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Only seen in Tears of the Kingdom and Xenoblade: in a title that sets the bit for "
            "another reason, its textures would be scrambled instead."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Tears of the Kingdom; Xenoblade Chronicles DE, 2 and X (Switch 2 Editions)"),
    },
    {
        .name = "EDEN_NX2_MEMORY_PRIORITY",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Enables VK_EXT_memory_priority (and VK_EXT_pageable_device_local_memory where "
            "present) and ranks device memory: render targets in memory of their own 1.0, the "
            "buffer cache 0.75, textures 0.5, staging 0.25. Over the VRAM budget the driver and "
            "Windows move the lowest first. Read when a game starts."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer frame-time spikes from render targets paged out to system memory on cards "
            "with little VRAM (6 GB)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "One driver allocation per new render target instead of a place in a shared block."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_NULL_VIEW_RESCALE_LEGACY",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Asks buffer and null image views whether their image is rescaled, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Those views have no image to ask."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_SPARSE_UNBACKED",
        .category = Category::TextureCache,
        .type = ValueType::Choice,
        .choices = SPARSE_UNBACKED_CHOICES,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "On, except on AMD drivers and RADV"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Whether images with no memory bound may be sampled and used as attachments."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 backs large images normally, a fix to try for GPU hangs on other drivers too."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "1 on AMD hangs the GPU shortly after the first such draw; 0 uses much more video "
            "memory."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_STORAGE_VIEW_BY_SIGN",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keys storage image views by signedness only, as before, instead of one view per "
            "format."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "An R32_UINT view was returned for a later R16_UINT or R32G32_UINT store."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TC_BUDGET_GUARD_MIB",
        .category = Category::TextureCache,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 4096,
        .suggested = "256",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                           "Off (256 MiB for Mario Kart World)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Once the video memory the driver counts, holes in the allocator's blocks included, "
            "comes within this many MiB of the budget, the texture cache thresholds go down by "
            "the excess. EDEN_NX2_TC_SLACK does the same from the full margin and takes "
            "precedence. 0 turns it off."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps a small card from going over its budget, where Windows pages video memory "
            "out: Mario Kart World on a 6 GB RTX 3050 reaches 5057 of 5211 MiB late in a session."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Near the budget the cache evicts more textures, which are loaded again."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TC_COMPACT",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Within a quarter of the video memory budget, with 128 MiB or more of the allocator's "
            "blocks unused, the texture cache moves its images out of the blocks they fill at "
            "most halfway into free space in fuller blocks (a copy on the GPU, at most 32 MiB "
            "every 10 frames, never into a new block), so that the emptied blocks go back to the "
            "driver."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Returns the 0.2-1 GB of video memory that holes in the allocator's blocks hold on "
            "a card near its budget (Mario Kart World on a 6 GB RTX 3050), where evicting more "
            "textures only makes more holes."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Each moved image is copied and its views and framebuffers are made again on their "
            "next use. Experimental: a missed case shows as a texture with stale or missing "
            "contents."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TC_CRITICAL_MIB",
        .category = Category::TextureCache,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 1048576,
        .suggested = "6144",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Computed from the GPU's memory"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The texture cache's critical usage in MiB: above it, eviction turns aggressive."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Tunes eviction for a card the defaults misjudge."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Too high runs out of video memory; too low decodes textures again and again."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TC_EXPECTED_MIB",
        .category = Category::TextureCache,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 1048576,
        .suggested = "4096",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Computed from the GPU's memory"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The texture cache's expected usage in MiB: above it, unused images are evicted "
            "sooner."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Tunes eviction for a card the defaults misjudge."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Too high runs out of video memory; too low decodes textures again and again."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_CLEAR_KEEPS_UPLOAD",
        .category = Category::TextureCache,
        .type = ValueType::Presence,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A clear never drops a render target's pending CPU upload. By default only a clear "
            "that overwrites the whole target (its colour slot, every channel or the full depth "
            "and stencil, layer 0, the whole render area) drops it."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog", "Diagnostics: rules out a clear losing data written by the CPU."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog", "Uploads targets that are about to be cleared anyway."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TC_GC_ASYNC",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "An image the texture cache evicts after copying it back to guest memory is deleted "
            "once the GPU has run the copy, instead of after a wait for the whole GPU. 0 waits "
            "as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "No GPU pipeline bubble per eviction batch: Ocarina of Time evicted up to 385 "
            "GPU-written images per 600 frames near its memory limit, each batch a full GPU wait."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Evicted images keep their memory a frame or two longer (at most 512 MiB in flight)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TC_GC_BATCH",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Images the texture cache collector must download before deleting them are "
            "downloaded together, with one wait for the GPU per 64 MiB. 0 waits for each one."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer GPU waits when video memory is full (Ocarina of Time on 6 GB cards made 868 in "
            "600 frames)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Holds up to 64 MiB of staging memory while a batch is recorded."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TC_GC_BUDGET_US",
        .category = Category::TextureCache,
        .type = ValueType::Number,
        .flags = Diagnostic,
        .minimum = 0,
        .maximum = 100000,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "300 microseconds"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "While video memory is below the expected use, the texture cache stops evicting "
            "images after this long each frame. 0 removes the limit. Under memory pressure "
            "there is never a limit."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps image deletion from stalling the GPU thread in large scenes."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Unused images stay in video memory a little longer."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TC_GC_PRESSURE_US",
        .category = Category::TextureCache,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 100000,
        .suggested = "4000",
        .default_value =
            QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "No limit (4000 for Ocarina of Time)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "While video memory is over the expected use but under the critical line, the "
            "texture cache stops evicting images after this many microseconds each frame. 0 "
            "removes the limit. Past the critical line there is never a limit."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Deleting an image can take a millisecond: up to 20 a frame held the GPU thread "
            "~20 ms in Ocarina of Time, which stays over the expected line."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Memory comes down more slowly; if it reaches the critical line the collector "
            "evicts without a limit, as before."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TC_IMAGE_POOL",
        .category = Category::TextureCache,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 4096,
        .suggested = "256",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off (256 MiB for Ocarina of Time)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The texture cache keeps the video memory of up to this many MiB of deleted images "
            "and gives it to new images of the same size, format and usage instead of "
            "allocating. Pooled images nobody takes are freed after 300 frames, and the pool is "
            "the first memory given back at the critical line. 0 turns it off."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer driver allocations, memory binds and frees: Ocarina of Time makes 3-4 new "
            "render targets a frame as the camera moves and deletes them ~25 frames later (Lichii "
            "#267: 46-51 to 57 fps in Castle Town on an RTX 4080 with 256 MiB)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Holds up to this much video memory no image uses: below the critical line the "
            "collector counts it and may evict textures sooner, and memory blocks it holds are "
            "not returned to the driver."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TC_JOIN_LAYERS",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When the texture cache joins images into a larger one (a texture array or a mipmapped "
            "texture over render targets drawn one layer or level at a time), it skips the upload "
            "from guest memory when the copies from the drawn images together overwrite every "
            "layer and level whole. It already did so when one copy covers the whole image "
            "(Lichii #1079)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mario Kart World draws each layer of its shadow map and colour arrays as a render "
            "target of its own, then samples the array: the upload under those copies took 13-31 "
            "ms an array in Lichii's logs. Not measured in Nemyu yet."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "If a copy writes less than its size says, that part shows uninitialized memory "
            "(garbage or black patches) instead of the guest's data. Only at 1x resolution, "
            "for single-sample images of the same plain format."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Mario Kart World, all titles"),
    },
    {
        .name = "EDEN_NX2_TC_MIN_AGE",
        .category = Category::TextureCache,
        .type = ValueType::Number,
        .flags = Diagnostic,
        .minimum = 0,
        .maximum = 100000,
        .suggested = "120",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 frames"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The texture cache never evicts images used within this many frames, while video "
            "memory allows: near its limit the age drops to 120 frames, and at the limit the "
            "cache evicts as usual."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Can stop eviction churn on a card near its limit."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps more images in video memory; 120 made no difference in Mario Kart World tests."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_RT_FRESH",
        .category = Category::TextureCache,
        .type = ValueType::Choice,
        .choices = RT_FRESH_CHOICES,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A new render target starts empty instead of loading what guest memory holds there. "
            "1: only targets no cached texture overlaps. 2: also overlapped ones, whose cached "
            "data is still copied in."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Ocarina of Time makes its post-processing targets at new addresses as the camera "
            "moves: about +15-20% while moving in Kokiri Forest (Lichii #834)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A game that draws only part of a new target, over what the CPU wrote there, loses "
            "those bytes."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TC_MINIMUM_MIB",
        .category = Category::TextureCache,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 16384,
        .suggested = "2048",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                           "Not set: 1 GiB below the expected threshold"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Texture cache size in MiB below which unused images are never collected. Capped at "
            "the expected threshold (Nolar-Hack's switch, via Lichii-Superbuild)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Revisited textures stay on the GPU instead of being created, uploaded and unswizzled "
            "again, which hitches."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Uses more video memory; too high on a small card makes the driver page."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TC_SKIP_STALE",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When a texture the GPU drew is dropped without being written back, its memory only "
            "holds stale bytes. A new texture made entirely on that memory starts without "
            "uploading them, since the GPU draws over it."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer uploads from guest memory: about +20% in Ocarina of Time's Castle Town "
            "(Lichii #463)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A game that reads such a texture before drawing it sees uninitialized contents "
            "instead of the stale ones. In Ocarina of Time on a 6 GB RTX 3050, frame times grew "
            "past a second after an area change and did not recover."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_TC_SLACK",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off (on for Ocarina of Time)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lowers the texture cache thresholds by the video memory the allocator holds beyond "
            "the driver's figure."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Stays within budget on cards where allocations leave holes."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "On a 6 GB card it evicted 2-4x more costly images and frames were slower."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TC_UNBACKED_FALLBACK",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When no memory is left anywhere for a texture cache image, it gets an image with "
            "nothing bound, as before, instead of being left out until the collector has freed "
            "memory."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Render targets with nothing bound are drawn into: AMD's drivers lose the device, and "
            "the cache mistakes them for images over unwritten sparse memory."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_NO_RESCALE",
        .category = Category::TextureCache,
        .type = ValueType::Text,
        .suggested = "2048x2048",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Not set: every size scales"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A comma-separated list of guest image sizes (WxH) that stay at 1x under a "
            "resolution scale, with the render passes they are bound in. The log names the sizes "
            "at the first check."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less GPU time and video memory for targets that gain little from the scale: at 2x "
            "Splatoon Raiders drew its 2048x2048 shadow maps at 4096x4096 and its 896x880 ink map "
            "at 1792x1760."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Those images and the passes that draw into them look as at 1x (blockier shadows or "
            "effects)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Splatoon Raiders"),
    },
    {
        .name = "EDEN_NX2_NO_RESCALE_ABOVE",
        .category = Category::TextureCache,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 32768,
        .suggested = "4096",
        .default_value =
            QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 (every size scales); 4096 for Splatoon Raiders"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Images wider or taller than this many texels stay at 1x under a resolution scale, "
            "with the render passes they are bound in. 0 scales every size. Can be a per-title "
            "default."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Splatoon Raiders' raid draws into 6400x6400 targets: at 2x they became 12800x12800, "
            "took a 6 GB card to 7.8 GB of video memory and ended in a lost device."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Those images look as at 1x. Nothing changes without a resolution scale above 1x."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Splatoon Raiders"),
    },
    {
        .name = "EDEN_NX2_TC_ZERO_CLEAR",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A colour image of 16 MiB or more whose guest pages have no memory is cleared to zero "
            "on the GPU instead of uploading the zeros it reads. 0 uploads it as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mario Kart World loads several such images of 95-341 MiB, each holding the GPU "
            "thread 60-230 ms."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                       "None expected: the image holds the same zeros."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_ZERO_CLEAR",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Widens EDEN_NX2_TC_ZERO_CLEAR: colour and depth images of 1 MiB or more whose guest "
            "memory is there but reads all zero (targets not drawn into yet) are also cleared on "
            "the GPU instead of uploaded. Images of 16 MiB or more are read in up to 8 parts at "
            "once."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Splatoon Raiders uploaded seven 6400x6400 targets (936 MiB) in ~480 ms at every raid "
            "entry; checking them took ~250 ms on one thread."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Opt-in: every image of 1 MiB or more that would be uploaded is read once more (most "
            "stop at their first page), and the check uses up to 7 extra threads for large ones."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Splatoon Raiders"),
    },
    {
        .name = "EDEN_NX2_TRANSCODE_POOL",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Reuses scratch images for texture transcodes instead of creating one each time."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 creates an image for every transcode."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TRANSCODE_THREADS",
        .category = Category::TextureCache,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 256,
        .suggested = "4",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A quarter of the CPU threads, at least 2"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How many threads decode and transcode textures on the CPU."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "More threads bring streamed textures in sooner."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Half the CPU threads took ~3.5 cores from the emulated CPU and the GPU thread on an "
            "8-core laptop; 2 left textures seconds late."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_UPLOAD_READ_BOUNDS",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Accelerated texture uploads read only the image's size from a reused staging "
            "buffer, not its whole capacity."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 reads past the texture data."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_VALID_ENTRY_LEGACY",
        .category = Category::TextureCache,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Sizes the memory check of native texture descriptors from the raw descriptor, as "
            "before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Legacy layer bits oversize the range, and buffers and 3D images assert."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_VRAM_LIMIT_MIB",
        .category = Category::TextureCache,
        .type = ValueType::Number,
        .flags = Diagnostic,
        .minimum = 1,
        .maximum = 1048576,
        .suggested = "6144",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Not set: the real size"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Treats every device-local memory heap as this many MiB and fails allocations past "
            "it, like a smaller card."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Tests how a title behaves on smaller GPUs."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Less video memory for everything."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_AMD_PARALLEL_PIPELINES",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                           "On for Mario Kart World, off for other titles"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps parallel pipeline compilation on AMD's Windows driver for Switch 2 titles."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Faster pipeline builds: with one worker Mario Kart World's boot took minutes for its "
            "cached pipelines."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The driver can crash compiling pipelines at the same time (Donkey Kong Bananza on "
            "RDNA1/2)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles on AMD GPUs under Windows"),
    },
    {
        .name = "EDEN_NX2_AMD_PIPELINE_WORKERS",
        .category = Category::Shaders,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 16,
        .suggested = "3",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1: one pipeline at a time"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How many pipelines AMD's Windows driver compiles at once for Switch 2 titles, when "
            "EDEN_NX2_AMD_PARALLEL_PIPELINES is off."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A scene's new pipelines build side by side: shorter and fewer multi-second stalls the "
            "first time it is seen."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The driver can crash compiling pipelines at the same time (Donkey Kong Bananza on "
            "RDNA1/2)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles on AMD GPUs under Windows"),
    },
    {
        .name = "EDEN_NX2_GCN_PIPELINE_WORKERS",
        .category = Category::Shaders,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 16,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How many pipelines compile at once for Switch 2 titles on AMD GCN4 and older GPUs "
            "(Polaris: RX 470-590) with AMD's driver. 0 removes the cap."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The cap keeps the driver's compiler from using up the RAM: an RX 580 ran out after "
            "Ocarina of Time's intro with one build per core."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A higher number builds a scene's new pipelines sooner but uses more RAM."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles on AMD GCN4 and older"),
    },
    {
        .name = "EDEN_NX2_DRIVER_AMD",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .flags = Diagnostic,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Reports the GPU driver as AMD's Windows driver to the renderer, so its workarounds "
            "(shader profile, pipeline builds, sparse images) run on any card."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Looks for faults in the AMD-only paths on another GPU."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Drops the workarounds of the real driver; slower and possibly wrong rendering."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_AMD_SHUFFLE_LOOP",
        .category = Category::Shaders,
        .type = ValueType::Choice,
        .choices = AMD_SHUFFLE_LOOP_CHOICES,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog", "Unset: on AMD's Windows driver and AMDVLK only"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Replaces warp shuffles from a lane computed at run time in vertex, geometry and "
            "fragment shaders with a loop of broadcasts."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Avoids the AMD driver crash while building such pipelines (Ocarina of Time goes "
            "black, then closes, after the intro)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Slower shaders where they shuffle; 0 brings the crash back on AMD."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles on AMD GPUs"),
    },
    {
        .name = "EDEN_NX2_COMPUTE_FAST_FIRST",
        .category = Category::Shaders,
        .type = ValueType::Choice,
        .choices = COMPUTE_FAST_FIRST_CHOICES,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog", "Unset: AMD's Windows driver in Switch 2 titles"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A compute shader first met in game (of EDEN_NX2_COMPUTE_FAST_FIRST_MIN_KIB or more) "
            "is built without driver optimizations, so its dispatch waits less, then built "
            "optimized in the background and swapped in."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shorter freezes when a scene brings new compute shaders (AMD took 17 and 36 s "
            "unoptimized for builds that took 6 and 18 min)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The shader runs slower for the seconds until the optimized build is ready, and it is "
            "compiled twice."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_COMPUTE_FAST_FIRST_MIN_KIB",
        .category = Category::Shaders,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 65536,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "64"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The SPIR-V size, in KiB, from which EDEN_NX2_COMPUTE_FAST_FIRST builds a compute "
            "shader unoptimized first. 0: every compute shader."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Lower: more shaders build fast first."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Lower: more shaders compile twice."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_COMPUTE_NOINLINE",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off (on for Ocarina of Time)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Compute shaders build the large functions they call from several places (a native "
            "program's global memory functions) as calls instead of letting the driver inline "
            "them at every call site. The cache and shader dumps keep the code as emitted."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Much shorter driver compiles, so shorter freezes at a new compute shader's first "
            "dispatch (NVIDIA: 92 shaders of a test title went from 246 s to 72.5 s)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Calls can run a little slower than inlined code; the driver's shader cache is built "
            "again once."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_GRAPHICS_NOINLINE",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off (on for Ocarina of Time)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The same for vertex, tessellation, geometry and fragment shaders: large functions "
            "called from several places are built as calls, not inlined by the driver."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shorter driver compiles of graphics pipelines, so fewer and shorter hitches when "
            "new ones are built."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Calls can run a little slower than inlined code; the driver's shader cache is built "
            "again once."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_POINTER_HANDLES",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A texture whose handle word is where the same shader keeps a storage buffer's "
            "address gets no texture. The shader loads the real handle per pixel, so the word "
            "only named an unrelated texture."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "No water flicker in Ocarina of Time (Zora's River waterfall, "
                                   "rivers, lakes)."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                       "Off: such surfaces sample a random texture again."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_SKIP_GUARD",
        .category = Category::Shaders,
        .type = ValueType::Choice,
        .choices = SKIP_GUARD_CHOICES,
        .suggested = "depth",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1; depth in Mario Kart World"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With asynchronous shaders, a Switch 2 draw whose pipeline is still building is only "
            "skipped when its render target is cleared and drawn again every frame. Draws into "
            "textures the game draws once (shadows, lighting, terrain) wait for the build. depth "
            "also skips depth-tested draws whose depth target is cleared every frame."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "1: no black characters or flat, untextured surfaces left by a draw skipped while its "
            "shader compiled. depth: scene draws over an uncleared color target no longer stall "
            "for the whole compile (Mario Kart World: up to 834 ms mid-race)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "depth and 0: such a draw is missing for the frames its shader compiles; 0 can leave "
            "textures the game draws once wrong until it draws them again."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PIPELINE_BINARIES",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On (off on NVIDIA)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Saves the shaders the GPU driver compiles (VK_KHR_pipeline_binary) in "
            "vulkan_binaries.bin beside the game's shader cache and gives them back to the driver "
            "on later runs. NVIDIA's driver keeps its own cache, so it is skipped there."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Each shader is compiled once instead of every session: on AMD, Ocarina of Time went "
            "from 513 s to 14 s to the title screen (RX 6750 XT, CheezerWheezer's measurement)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Uses disk space and keeps the file in memory while the game runs. A driver update "
            "starts the file over."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All games"),
    },
    {
        .name = "EDEN_NX2_WARM_THREADS",
        .category = Category::Shaders,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 32,
        .suggested = "2",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "4"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "CPU threads of the shader warm-up, which compiles the shaders extracted from the "
            "game files (Build Shader Warm-up from Game Files in the game list) at low priority "
            "while the game runs."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "More: the warm-up finishes sooner."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                       "More: less CPU is left to the game while it runs."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_NV_SHADER_CACHE",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .flags = GuiOnly,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Sets __GL_SHADER_DISK_CACHE_SIZE to 10 GiB and __GL_SHADER_DISK_CACHE_SKIP_CLEANUP "
            "to 1 for Nemyu's process only, unless they are already set. NVIDIA's driver reads "
            "them instead of the Control Panel's Shader Cache Size, which newer drivers no longer "
            "show. Read when Nemyu starts."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "NVIDIA: compiled shaders are no longer dropped once the driver's default of about "
            "1 GiB is full, so places visited before don't compile again after a restart."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The driver's shader cache folder can grow to 10 GiB. No effect on other GPUs."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_COMPUTE_WARM",
        .category = Category::Shaders,
        .type = ValueType::Text,
        .suggested = "",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "The title's warm-compute.bin"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Path of a compute warm-up file to load instead of the one built for the title."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Tests warm-up files made elsewhere."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                       "A file of another title only wastes CPU time."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PRECOMPILE_COMPUTE",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Prepare Shaders also scans the game's files for its native compute programs (the "
            "result is kept as native_programs.bin beside the shader cache) and builds each one "
            "with the workgroup and memory sizes of its shader control section, then drops it. "
            "Only the driver's cache keeps it. The pipelines built are listed in "
            "native_precompiled.bin, so the next Prepare Shaders only builds new ones. Nothing "
            "happens on the loading screen or in game."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer stutters the first time a compute shader is dispatched, when the game's "
            "translation matches the guessed one. The session end log says how many of the "
            "session's new compute pipelines were precompiled."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "New and unmeasured. Prepare Shaders takes longer, the first time much longer (the "
            "whole RomFS is read). The game state a program reads is guessed, so some builds "
            "don't match the game's."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PRECOMPILE_GRAPHICS",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Prepare Shaders also scans the game's files for its native vertex and pixel programs "
            "and builds each pair the game's shader containers name, for the most common render "
            "setups and vertex attribute types of the pipelines in the title's shader cache, then "
            "drops it. A program in no pair is built with a stand-in. Only the driver's cache "
            "keeps them. The pipelines built are listed in native_precompiled.bin, so the next "
            "Prepare Shaders only builds new ones. Needs a shader cache with pipelines; nothing "
            "happens on the loading screen or in game."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer stutters the first time new tracks and effects are drawn, when the driver "
            "reuses what was built. The session end log says how many of the session's new "
            "pipelines had their programs precompiled."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "New and unmeasured. Prepare Shaders takes much longer: each pair is built for up to "
            "six render setups, and the driver's cache grows. The game state a program reads is "
            "guessed, so some builds don't match the game's."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_AMD_SAFE_COMPILE",
        .category = Category::Shaders,
        .type = ValueType::Choice,
        .choices = AMD_SAFE_COMPILE_CHOICES,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog", "Unset: on AMD's Windows driver older than 2.0.388 only"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Builds the largest compute shaders (EDEN_NX2_AMD_SAFE_COMPILE_MIN_KIB, and two of "
            "Ocarina of Time's) without the driver's optimizations."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Ocarina of Time's intro no longer sits at 0 FPS for 6 to 18 minutes on RX 5000/6000 "
            "cards with older drivers."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Those shaders run slower (about 25 instead of 33 FPS in those scenes on an RX 6600 "
            "XT); updating the driver avoids both."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles on AMD GPUs under Windows"),
    },
    {
        .name = "EDEN_NX2_AMD_SAFE_COMPILE_GRAPHICS",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Builds every graphics pipeline without the driver's optimizations on AMD's Windows "
            "driver."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "May avoid a driver crash while pipelines build that EDEN_NX2_AMD_SHUFFLE_LOOP does "
            "not."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Slower shaders, lower frame rate."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles on AMD GPUs under Windows"),
    },
    {
        .name = "EDEN_NX2_AMD_SAFE_COMPILE_MIN_KIB",
        .category = Category::Shaders,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 65536,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "800"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The SPIR-V size, in KiB, from which EDEN_NX2_AMD_SAFE_COMPILE builds a compute "
            "shader unoptimized. 0: every compute shader."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A lower size can get past another long driver compile at boot."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "More shaders run slower."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles on AMD GPUs under Windows"),
    },
    {
        .name = "EDEN_NX2_ASYNC_SKIP",
        .category = Category::Shaders,
        .type = ValueType::Choice,
        .choices = ASYNC_SKIP_CHOICES,
        .suggested = "0",
        .default_value =
            QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Unset: as EDEN_NX2_NATIVE_SYNC_PIPELINES"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Selevet's switch, read when EDEN_NX2_NATIVE_SYNC_PIPELINES is unset: 0 always waits "
            "for a native pipeline (full); 1 skips depth-tested draws and draws of more than six "
            "vertices or indices while their pipeline builds, and waits for the rest."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lets a Selevet nx2_switches.txt work unchanged."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 stutters on new pipelines; 1 leaves objects missing for a few frames."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_BACKGROUND_PIPELINES",
        .category = Category::Shaders,
        .type = ValueType::Choice,
        .choices = BACKGROUND_PIPELINES_CHOICES,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Unset: Switch 2 titles, where the free memory is known (Windows) and the driver "
            "compiles pipelines in parallel (not AMD's Windows driver in Switch 2 titles)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Builds the cached pipelines boot leaves out (the ones recent sessions didn't use) on "
            "low-priority threads while the game runs, each only while 8 GiB of memory is free, "
            "instead of on the GPU thread when a draw first needs one."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer shader hitches in places recent sessions didn't visit (Frizti measured 118 such "
            "builds, 1.4 s of GPU thread time, in Ocarina of Time's first 10 s of play)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "CPU time on those threads for a while after boot, and ~0.45 MB of driver memory per "
            "pipeline built; 1 also builds them where the free memory isn't known."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_BACKGROUND_PIPELINE_THREADS",
        .category = Category::Shaders,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 16,
        .suggested = "2",
        .default_value =
            QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A quarter of the CPU's threads, 1 to 6"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How many threads build the cached pipelines boot leaves out "
            "(EDEN_NX2_BACKGROUND_PIPELINES)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "More finish them sooner."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "More take CPU time from the game while they run."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_BUILD_PRIORITY",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Pipelines a draw waits for, and the ones boot builds, go ahead of the ones built in "
            "the background (the cached pipelines boot left out, the compute warm-up): a "
            "background build waits, up to 2 s, while the pipeline builder threads have builds "
            "to do."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Draws can wait less for new pipelines while the background builds run. The log's "
            "\"Async pipelines\" line tells how much of the wait was while they ran."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The background builds finish later. A background build already running is not "
            "stopped."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_BUILD_WAITED_PIPELINES",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A draw or dispatch that waits for a queued pipeline builds it on the spot instead "
            "of waiting for its turn on the builder threads."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 can wait behind hundreds of queued builds."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_PIPELINE_BOOST",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "While a draw or dispatch waits for a pipeline a builder thread is compiling, that "
            "thread runs at the highest normal priority instead of below normal. Windows only. 0 "
            "leaves it below normal."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The compile a frame is stuck on is not starved by the emulator's own busy threads."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None expected: only the build a draw waits for is raised, until it ends."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_DLSS_AUTO_EXPOSURE",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "NVIDIA DLSS works out the exposure of the game's colour itself. Only builds with "
            "NVIDIA DLSS, with Use NVIDIA DLSS in place of a game's DLSS on."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 gives DLSS no exposure for colour that goes above 1."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fire Emblem: Fortune's Weave"),
    },
    {
        .name = "EDEN_NX2_DLSS_HDR",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "NVIDIA DLSS takes the game's colour as HDR (linear, above 1). Only builds with "
            "NVIDIA DLSS, with Use NVIDIA DLSS in place of a game's DLSS on."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 treats Fire Emblem's linear colour as display colour."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fire Emblem: Fortune's Weave"),
    },
    {
        .name = "EDEN_NX2_DLSS_PRESET",
        .category = Category::Shaders,
        .type = ValueType::Choice,
        .choices = DLSS_PRESET_CHOICES,
        .suggested = "default",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "K"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The model NVIDIA DLSS uses in every mode. Only builds with NVIDIA DLSS, with Use "
            "NVIDIA DLSS in place of a game's DLSS on. The log says how much video memory DLSS "
            "allocated each time it is set up, which tells the models apart."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Another model may look better; default leaves the choice to NVIDIA. The older CNN "
            "models E and F use about a third less video memory than J and K (DLSS 310.6 or "
            "later)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "NVIDIA's defaults for Performance and Ultra Performance (M, L) drew thin horizontal "
            "lines in Fire Emblem at 2x resolution scale."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fire Emblem: Fortune's Weave"),
    },
    {
        .name = "EDEN_NX2_DYNAMIC_STATE_LOOKUP",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Looks a draw's pipeline up again after setting its dynamic states, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Rebuilds the whole pipeline key on every draw with dynamic vertex input."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_DYNAMIC_SKIP",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A draw with none of the dirty flags that gate the dynamic state updates skips them. "
            "Never on AMD drivers, whose logic-op workaround changes a register every draw."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A little less GPU thread time per draw (dynamic states took 3% of it in Donkey Kong "
            "Bananza, per Lichii)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Experimental. A missed state would show as wrong culling, depth or blending; "
            "EDEN_NX2_FAST_DRAW_VERIFY=1 counts them."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PIPELINE_ASYNC",
        .category = Category::Shaders,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 2,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2 for Switch 2 titles"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With asynchronous shaders, a new graphics pipeline is translated and built on worker "
            "threads instead of the GPU thread, and its draws are skipped for the frames it "
            "builds. 0 off, 1 Switch 1 draws only, 2 Switch 2 draws too."),
    },
    {
        .name = "EDEN_NX2_ATTRIBUTE_ALIAS",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Without dynamic vertex input, a draw whose state differs from a cached pipeline only "
            "in vertex attributes its shader doesn't read uses that pipeline instead of building "
            "a new one. 0 builds every variant."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer pipeline builds in play (stutter, or skipped draws with asynchronous shaders)."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected: the pipeline is the same."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles, on GPUs without dynamic vertex input or with it off"),
    },
    {
        .name = "EDEN_NX2_ASYNC_SHADERS",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Follows the Asynchronous shaders setting (off)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Asynchronous shader compilation for Switch 2 titles. 1 skips draws whose pipeline "
            "is still building (less stutter, objects missing or black for a few frames); 0 "
            "waits for every new pipeline."),
    },
    {
        .name = "EDEN_NX2_CACHE_DISCARD_OLD",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Deletes a shader cache written by another Nemyu version, as before. By default it is "
            "set aside (up to three versions per game) and restored when that version runs again."),
    },
    {
        .name = "EDEN_NX2_SHADER_CACHE_V2",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps a game's Vulkan pipelines and its list of used pipelines in one log of "
            "checked records (vulkan_log.bin) instead of vulkan.bin, vulkan_mesh.bin and "
            "vulkan_used.bin. The first boot takes in what vulkan.bin, vulkan_mesh.bin, their "
            "set-aside copies of the same layout and vulkan_used.bin hold; later boots take in "
            "what a session with this off added to vulkan.bin. The older files are only read. "
            "The Shader Manager counts, imports into and exports the log. 0 goes back to "
            "vulkan.bin as it was."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer shader stutters after a relaunch: an update that only changes how shaders are "
            "translated keeps the log instead of starting the cache over, a damaged or cut-off "
            "record costs only itself, and a pipeline saved again replaces its old record."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Pipelines built while it is on are only in the log: with it off they are built anew "
            "(a Shader Manager export with it on writes them as a vulkan.bin that can be "
            "imported). The first boot of each game after turning it on copies the old cache "
            "into the log once, which takes a little longer."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_CACHE_REPAIR",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A shader cache whose last pipeline was cut off (the emulator closed or crashed while "
            "saving it) or that holds a damaged one is cut off before it, keeping every pipeline "
            "saved before. A cached pipeline that this build can't translate from what an older "
            "build saved of it is saved again when the game builds it, and that copy is used from "
            "the next boot on. 0 deletes the whole cache in the first case and keeps failing on "
            "the old copy in the second, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A crash no longer costs the next boot the whole shader cache, and with it every "
            "shader stutter the game had already gone through. A pipeline whose old copy fails "
            "stutters once instead of at every session."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None known: the pipelines kept are the ones that read back whole, and a copy saved "
            "again adds one record to the cache file."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_KEEP_CACHE_DRIVER_VERSION",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Ignores cached pipelines saved by another version of the GPU driver, as before. By "
            "default they are used after a driver update instead of being built again."),
    },
    {
        .name = "EDEN_NX2_EVICT_PIPELINES",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Always evicts unused Switch 2 pipelines under memory pressure, instead of keeping "
            "them while Windows still has 8 GiB or more to commit."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Less memory."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Coming back to a place builds its pipelines and translates their shaders again: "
            "hitches."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PIPELINE_EVICT_ABOVE",
        .category = Category::Shaders,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 100000,
        .suggested = "4000",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "8000"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How many graphics pipelines a Switch 1 title keeps before the ones unused for 9,000 "
            "frames are destroyed, least recently used first and only down to this count. 0 "
            "destroys every unused one, as before. Switch 2 titles keep theirs while Windows has "
            "8 GiB or more to commit (EDEN_NX2_EVICT_PIPELINES)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Coming back to a place does not build its pipelines again: fewer hitches."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                       "About 0.45 MB of driver memory per pipeline kept."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 1 titles"),
    },
    {
        .name = "EDEN_NX2_EXPERIMENTAL_GEOMETRY",
        .category = Category::Shaders,
        .type = ValueType::Choice,
        .choices = EXPERIMENTAL_GEOMETRY_CHOICES,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Pass-through programs only"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Which native Switch 2 geometry programs are translated. Pass-through ones only "
            "forward vertices and set per-primitive state."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1 may draw geometry that is missing."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "1 is experimental and can break draws; 0 drops the pass-through ones."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_FORCE_SUBGROUP_ALL",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Requires the guest subgroup size for every compute pipeline, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Constrains pipelines that don't need it."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_GAME_DLSS",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .flags = Diagnostic,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Off: Fire Emblem: Fortune's Weave only"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Follows a game's DLSS evaluation in every title, not only in Fire Emblem: Fortune's "
            "Weave, the only one whose layout is known."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Looks for the same DLSS layout in other titles."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "One comparison more per compute dispatch."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_GENERIC_OUTPUT_GUARD",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Drops shader stores to output components that have no output variable, instead of "
            "emitting an invalid id."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 brings back invalid shaders, such as Donkey Kong Bananza's geometry program on "
            "AMD."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_GS_PASSTHROUGH_KEEP_FLAGS",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps the NV pass-through flags on geometry programs lowered to emit their own "
            "vertices, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The pipeline then sees a geometry stage and pass-through decorations at once."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_GS_PRUNE_OUTPUTS",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Drops the generic outputs the fragment shader never reads from a geometry shader "
            "that outputs more components than the device allows."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 brings back pipelines over the limit, which AMD's Windows driver closes the "
            "emulator on (Mario Tennis Fever)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_INTEL_CONDITIONAL_BARRIERS",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps shader barriers inside branches and loops on Intel's Windows driver, which "
            "otherwise drops them as a driver workaround."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Compute passes that share data between warps stop reading stale shared memory "
            "(Mario Kart World's lighting on Mesa had this)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The workaround exists because the driver mishandled such barriers; it may hang or "
            "crash on some driver versions. Intel on Windows only."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_LOWER_EXTENDED",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lowers Ocarina of Time's indexed byte constant loads, ULEA carries past UP1, "
            "two-source F2IP.U8 and I2F.U8 of a uniform byte instead of rejecting the program."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0: Eglantine measured ~8 ms less work on the GPU thread per 50 ms frame without "
            "these forms."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 skips the compute programs that use them every frame (LOD vegetation)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "The Legend of Zelda: Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_MESH_PUSH_LEGACY",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Leaves the task and mesh stages out of graphics push constants, and scales the null "
            "image for buffer views, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mesh shaders lose their rescaling and render area data."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_MKW_WATER",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mario Kart World's water: writes the collision height map a compute program makes "
            "back to guest memory at the next fence, and gives the wave program a depth factor "
            "of 1 where its water and ground inputs are both empty. Each acts on one exact "
            "program. 0 turns both off."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Karts stay on the water after the first water section, and waves come back on "
            "flat water."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A 512 KiB image download per collision dispatch, copied at a fence without a wait."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Mario Kart World"),
    },
    {
        .name = "EDEN_NX2_NAN_MINMAX",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On for AMD drivers, off for others"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shaders' min, max and saturate return the number when the other operand is NaN, as "
            "the Switch GPU does, on every driver instead of only AMD's (which pass the NaN on)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Tells whether black pixels or flashes come from NaN: if they go away with it, they "
            "do."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Slower shaders on NVIDIA (Donkey Kong Bananza 31 -> 49 ms a frame), and every "
            "pipeline is compiled again by the driver once after turning it on or off."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_NATIVE_PROGRAMS",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Records each Switch 2 native program the GPU thread lowers at its first bind in the "
            "shader cache log (vulkan_log.bin), and lowers the recorded ones on the pipeline "
            "workers while the game boots, so that their first binds take them from there. "
            "Every boot lowers them with the running build, so a lowering change never leaves a "
            "stale program. Needs EDEN_NX2_SHADER_CACHE_V2; EDEN_NX2_REJECT, "
            "EDEN_NX2_SHADER_DUMP and EDEN_NATIVE_SHADER_OVERRIDE_DIR turn it off."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shorter hitches when a native program is bound for the first time in a session, by "
            "what its lowering costs (EDEN_NX2_GPU_PROFILE logs it)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "New and unmeasured. The boot lowers every recorded program on the workers, which "
            "takes longer, and the lowered programs stay in memory for the session."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_NATIVE_SYNC_BUDGET_US",
        .category = Category::Shaders,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 1000000,
        .suggested = "50000",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                           "3000 µs; 50000 µs in Ocarina of Time"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With asynchronous shaders, in budget mode, how long a draw waits for its pipeline, "
            "in microseconds, before it is skipped."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Higher skips fewer draws (objects missing or black while a pipeline builds)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Higher stutters longer on new pipelines."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_NATIVE_SYNC_WINDOW_LENGTH_US",
        .category = Category::Shaders,
        .type = ValueType::Number,
        .minimum = 1000,
        .maximum = 1000000,
        .suggested = "250000",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                           "16667 µs; 250000 µs in Ocarina of Time"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With asynchronous shaders, in budget mode, the length in microseconds of the window "
            "in which EDEN_NX2_NATIVE_SYNC_WINDOW_US limits the waits for pipelines."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Longer lets a burst of new pipelines wait more at once, so fewer draws are skipped."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Longer makes that burst one long stall instead of a few ms each frame."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_NATIVE_SYNC_WINDOW_US",
        .category = Category::Shaders,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 1000000,
        .suggested = "100000",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                           "8000 µs; 100000 µs in Ocarina of Time"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With asynchronous shaders, in budget mode, how long all draws may wait for their "
            "pipelines in total within the window EDEN_NX2_NATIVE_SYNC_WINDOW_LENGTH_US sets, in "
            "microseconds; past it they are skipped."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Higher skips fewer draws when many new pipelines come at once."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Higher makes those bursts stall longer."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_NATIVE_SYNC_PIPELINES",
        .category = Category::Shaders,
        .type = ValueType::Choice,
        .choices = NATIVE_SYNC_PIPELINES_CHOICES,
        .suggested = "full",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "budget"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Whether a draw waits for its native pipeline to finish building: briefly (budget), "
            "always (full), or never (the draw is skipped until it is ready)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "full draws everything on its first frame; 0 never stalls."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "full stutters on new pipelines; 0 leaves objects missing for a few frames."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_NOPERSP_AS_SMOOTH",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off on NVIDIA, on elsewhere"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Screen-linear (noperspective) shader outputs are written times the vertex's w and "
            "read back perspective-correct, so a primitive clipped close to the camera "
            "interpolates them right on every driver."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "No red dotted lines through Ocarina of Time's water volumes (Royal Family's Tomb) "
            "on AMD (Lichii #441)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Ocarina of Time's fog at the Hyrule Field drawbridge turns solid white on NVIDIA, "
            "and lines show in Kokiri Forest's fog (Fritchii took it back in v5.1). Pipelines "
            "with such outputs compile once more after changing it."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_NO_MESH",
        .category = Category::Shaders,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Skips task and mesh shader draws, as before they were supported (over the mesh "
            "shader setting)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A/B comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mario Kart World's demo stalls on the world map without them."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Mario Kart World"),
    },
    {
        .name = "EDEN_NX2_NO_MESH_FILE",
        .category = Category::Shaders,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Skips task and mesh draws while this file exists (checked every 1024 draws), to "
            "compare one scene with and without them."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A/B comparisons in one run."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mario Kart World's demo stalls on the world map without them."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Mario Kart World"),
    },
    {
        .name = "EDEN_NX2_NO_TENSOR",
        .category = Category::Shaders,
        .type = ValueType::Presence,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Leaves tensor-core matrix multiplies (HMMA) unimplemented: programs that use them "
            "are rejected, and upscalers fall back to a bilinear blit."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Avoids long shader compiles and slow frames from emulated neural upscalers."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lower-quality upscaling where a title uses them."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_OOT_BLOOM_EXP_SCALE",
        .category = Category::Shaders,
        .type = ValueType::Text,
        .suggested = "1.6",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1 (unchanged)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the Ocarina of Time visual workaround, scales the bloom constant of the final "
            "pass; higher is weaker bloom. Values outside 0 to 16 keep the default."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Less glare (Selevet ships 1.6)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A tuning of the workaround, not a fix; too high removes bloom the game has."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_OOT_EXPOSURE_SCALE",
        .category = Category::Shaders,
        .type = ValueType::Text,
        .suggested = "0.55",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0.65"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the Ocarina of Time visual workaround, scales the exposure before the filmic "
            "curve; lower is darker. Values outside 0 to 16 keep the default."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Less glare (Selevet ships 0.55)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A tuning of the workaround, not a fix; too low darkens the whole image."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_OOT_FOG_SCALE",
        .category = Category::Shaders,
        .type = ValueType::Text,
        .suggested = "0.12",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0.25"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the Ocarina of Time visual workaround, scales the fog slice thickness; lower is "
            "less haze. Values outside 0 to 16 keep the default."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Less haze (Selevet ships 0.12)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A tuning of the workaround, not a fix; too low removes fog the game has."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_OOT_NETWORK",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off: a plain upscale stands in"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Emulates Ocarina of Time's gameplay upscaler, a tensor-core network of 768 matrix "
            "multiplies, instead of drawing a plain upscale of its input."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The game's own reconstruction of detail and anti-aliasing."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "GPU time every frame: minutes to compile and about 8 FPS on Mesa's Intel driver."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_OOT_VISUAL_WORKAROUND",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Temporary workaround, not a fix: thins Ocarina of Time's outdoor fog and lowers its "
            "bloom exposure, tuned against the console, while the cause of the haze is open."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 shows the game as emulated, to compare."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 brings back outdoor areas far hazier than on the console."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_OOT_WIDE_AFFINITY",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Ocarina of Time's threads that may run on more than one core may run on all of the "
            "game's cores, so an idle core can take a main thread its job workers preempted."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Uses the cores that otherwise sit idle about 30% of the time (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Threads move between cores more often."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_PIN_THREADS",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Windows, 1: the Switch 2 GPU thread and the GPU split's back each get a whole fast "
            "physical core (the 2nd and 3rd, with 6 or more fast cores), and the guest CPU cores "
            "keep off them. Applied with CPU sets; the log names the ones chosen."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "No guest core shares the GPU threads' core. Frizti measured Lake Hylia in Ocarina of "
            "Time at 51.3 -> 55.6 FPS median, worst window 28.8 -> 53.8 (Ryzen 7 7800X3D); Lichii "
            "#769 measured Kokiri Forest at 31.7 -> 32.4 FPS (i5-12400F, Linux)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Skipped below 6 fast physical cores. Other emulator threads still share those two "
            "cores. The earlier hard affinity version made Mario Kart World stall for seconds and "
            "Ocarina ~2% slower on a 6 P-core + 4 E-core laptop; this one is not measured there."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PIPELINE_CLEAN",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The graphics pipeline lookup skips rebuilding the pipeline key while none of the "
            "registers it is built from changed since the last draw (compared by value)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less GPU thread time per draw: the key rebuild cost ~40 ms per second in Ocarina of "
            "Time at 37,700 draws a second (Dr. Sergio). The log counts lookups with and without "
            "it every 10 s."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Experimental. A register missing from the watched list would draw with the previous "
            "pipeline: wrong blending, culling or depth on some draws."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_PRELOAD_ALL_PIPELINES",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Builds every cached pipeline at boot instead of only the ones used in recent "
            "sessions."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer build hitches in places not visited lately."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Slower boot and more memory; a boot that ran out of memory never saved its list."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_PRMT",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Translates the PRMT byte permute instruction in its default mode."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 leaves every PRMT unimplemented, so those shaders fail."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PTP_RUNTIME_OFFSETS",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Native shaders whose per-tap texture gather offsets can't be proven constant read "
            "them at run time instead of being rejected."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 rejects those programs, so their draws are skipped."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_REJECT",
        .category = Category::Shaders,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Treats the native programs at these hex addresses (comma separated) as rejected."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Finds which newly translated program changes a game, without rebuilding."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Those draws and dispatches are skipped."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_REJECT_RECHECK_GPU",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A native program the lowering rejected is re-read coherently, with the GPU's writes "
            "there flushed first, when its rejection is checked again (at most every 250 ms). A "
            "draw whose stages failed also looks at them again every 250 ms without waiting for a "
            "new shader bind."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A program the GPU uploads after its first bind is noticed once the upload lands, so "
            "its draws come back instead of staying skipped until the scene changes."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A check of a range the GPU wrote flushes it to guest memory first, a short GPU wait "
            "every 250 ms per such program while it stays rejected."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Titles with programs rejected right after loading (Yoshi and the Mysterious Book in "
            "Orchid)"),
    },
    {
        .name = "EDEN_NX2_SKIP_VS",
        .category = Category::Shaders,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Drops draws that use the vertex shaders at these hex addresses (comma separated)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Isolates which draw causes a glitch."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Those objects disappear."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SKIP_COMPUTE",
        .category = Category::Shaders,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Drops the dispatches of the compute shaders with these hex hashes (comma separated). "
            "The log names each compute shader at its first dispatch."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "Finds which compute shader hangs the GPU or causes a glitch."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                       "What those shaders compute is missing."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SPIRV_CAPS_LEGACY",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Declares PrimitiveId and draw parameters without their SPIR-V capabilities, and "
            "enables quad control without maximal reconvergence, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shaders that spirv-val rejects, which drivers may miscompile."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_UNDEF_ZERO",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shader registers read before any write are zero, as on NVIDIA hardware, instead of "
            "undefined."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 brings back green patches on Mario Kart World's roads on AMD."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_USED_LIST_MERGE",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Merges short sessions into the used-pipeline list instead of starting a new "
            "two-session window at every save."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 can forget pipelines after a few short sessions."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_USED_SESSIONS",
        .category = Category::Shaders,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 16,
        .suggested = "8",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2, 8 for Ocarina of Time"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How many recent sessions' graphics pipelines are built at boot, at most 3,000."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "More sessions: fewer pipeline builds early in play, in areas the last sessions "
            "did not visit."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A longer boot and about 0.45 MB of driver memory per extra pipeline."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_GPL",
        .category = Category::Shaders,
        .type = ValueType::Choice,
        .choices = GPL_CHOICES,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Graphics pipelines built in game are linked from VK_EXT_graphics_pipeline_library "
            "parts: the vertex and fragment shader parts are compiled once and shared by every "
            "state variant of the same shaders, each variant then costs a fast link. Pipelines "
            "built at boot stay whole. Only where the driver links fast and lets stages disagree "
            "on interpolation; AMD's proprietary driver only takes it with 2, and links "
            "optimized there."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shorter \"building shaders\" hitches when a known shader set meets new state (vertex "
            "formats, render targets, depth): a link of a fraction of a millisecond instead of a "
            "compile of 1 to 116 ms on NVIDIA."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "More driver memory per pipeline (its own vertex input and fragment output parts, and "
            "the shared parts it holds). A fast-linked pipeline may run slightly slower on the "
            "GPU; EDEN_NX2_GPL_OPTIMIZE relinks it optimized. AMD's Windows driver draws "
            "fast-linked pipelines wrong."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_GPL_OPTIMIZE",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With EDEN_NX2_GPL, every fast-linked pipeline is linked again with link time "
            "optimization on one low priority thread, and replaces the fast one at a later draw."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Pipelines run as fast on the GPU as whole ones, once their optimized link is in."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Libraries keep link time optimization info and both linked pipelines stay alive: "
            "more driver memory. CPU time on that thread after each new pipeline."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_SHADER_REUSE",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A new graphics pipeline that runs the same shaders as an earlier one and differs only "
            "in state its shaders don't read (render target formats, blending, depth, vertex "
            "formats of the same type) takes the earlier one's translated shaders instead of "
            "translating them again on the GPU thread, when the game state translation read "
            "(constant buffer words, texture types) is the same too."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shorter freezes when a game meets many new pipelines at once: in Mario Kart World 116 "
            "of 119 new pipelines in one burst were such variants, translated one after another "
            "for about half a second (a frame held 878 ms). Only their driver compile is left, "
            "on the pipeline workers."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Up to 64 MiB of memory for the kept shaders. A pipeline that needed its own "
            "translation would draw wrong: turn it off to rule that out."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_PARALLEL_STAGES",
        .category = Category::Shaders,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 4,
        .suggested = "2",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 (off)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A new graphics pipeline the GPU thread has to build itself (a draw that can't be "
            "skipped) has its shaders translated at the same time on this many helper threads, "
            "while the GPU thread translates the fragment shader, instead of one after another. "
            "0 off."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shorter freezes when new shaders appear: the GPU thread waits for the longest "
            "shader of a pipeline instead of all of them (vertex and fragment usually come "
            "together)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "New and untested: if a game crashes or draws wrong when new shaders appear, set it "
            "back to 0. The helpers take CPU time from the game while they translate."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_VIDS",
        .category = Category::Shaders,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps VK_EXT_vertex_input_dynamic_state on NVIDIA and Intel Windows drivers for "
            "Switch 2 titles."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fewer pipelines."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Per-draw setup cost more with it in Mario Kart World on an RTX 3050."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Switch 2 titles on NVIDIA and Intel GPUs under Windows"),
    },
    {
        .name = "EDEN_STREAMLINE_DIR",
        .category = Category::Shaders,
        .type = ValueType::Text,
        .flags = EnvironmentOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Not set: the streamline folders"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Folder searched first for NVIDIA Streamline's files (sl.interposer.dll and its "
            "plugins), before the streamline folder of the data folder and the one next to the "
            "executable. Only builds with NVIDIA DLSS read it."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps NVIDIA's files in one place for several installs."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fire Emblem: Fortune's Weave"),
    },
    {
        .name = "EDEN_CPU_ACCURATE_FLOAT",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With CPU accuracy on Unsafe or Auto, drops the reduced-error, NaN and FPCR "
            "floating point shortcuts, as Accurate does (Selevet's switch)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Accurate floating point results while the rest of the CPU stays fast."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Slower floating point code in the guest."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_CPU_FUSED_FMA",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With CPU accuracy on Unsafe or Auto, keeps fused multiply-adds fused instead of "
            "splitting them (Selevet's switch)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Bit-exact multiply-add results, which physics and animation code can depend on."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Slower on CPUs without FMA."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_CPU_GLOBAL_MONITOR",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With CPU accuracy on Unsafe or Auto, keeps exclusive memory accesses checked across "
            "cores instead of ignoring the global monitor (Selevet's switch)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lock-free hand-offs between guest threads fail when they should, which can fix rare "
            "hangs and corruption."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Slower exclusive loads and stores."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_CPU_FASTMEM_CHECK",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With CPU accuracy on Unsafe or Auto, keeps the bounds check on fast memory accesses "
            "(Selevet's switch)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Out-of-range guest accesses are caught instead of reaching host memory."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A little slower memory access in the guest."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_CPU_SETS",
        .category = Category::CpuTiming,
        .type = ValueType::Choice,
        .flags = EnvironmentOnly,
        .choices = CPU_SETS_CHOICES,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Hybrid CPUs (P- and E-cores) only"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Places the emulator's threads on the performance or efficiency cores of hybrid CPUs "
            "(Windows)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 lets Windows schedule freely; 1 places threads even with few P-cores."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Threads that land on E-cores run slower."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NUP_DAMAGED_FALLBACK",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A .zs file in a .nup whose zstd header is missing (a damaged dump) is replaced by "
            "the closest file of the same kind in its folder."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "No corrupt textures in Ocarina of Time's fairy cave from damaged dumps "
            "(Lichii #740)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The replaced texture is a look-alike, not the original."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Titles loaded from .nup files"),
    },
    {
        .name = "EDEN_NX2_AVX512",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lets the CPU JIT use AVX-512 code paths."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Possibly faster guest code on CPUs with AVX-512."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Zen 4 players crashed in Mario Kart World shortly after boot, and one path gave "
            "wrong results."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles on CPUs with AVX-512"),
    },
    {
        .name = "EDEN_NX2_CNTFRQ",
        .category = Category::CpuTiming,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 2000000000,
        .suggested = "19200000",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "31,250,000 Hz on Switch 2 titles, 19,200,000 Hz otherwise"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The guest system counter's frequency, in Hz."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A/B tests of timing code."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Switch 2 titles convert ticks at 31.25 MHz: any other value makes their times wrong "
            "(1.63x off at 19.2 MHz)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_CNTPCT_PAUSE",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On with SMT, off without"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Pauses the host core for a moment when guest code reads the system counter in a "
            "tight loop (two reads within ~1.7 µs), handing it to the SMT sibling."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 may help a title that polls the counter in timing-critical work."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 lets the polling thread slow down its SMT sibling, often the GPU thread."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_INLINE_CNTPCT",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Guest reads of the system counter are computed from RDTSC in the recompiled code "
            "instead of calling into the emulator. Only with an invariant TSC, the normal CPU "
            "clock and core speed sync off."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Cheaper counter reads, which games make thousands of times a frame."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Skips EDEN_NX2_CNTPCT_PAUSE, so a thread polling the counter runs at full speed "
            "beside its SMT sibling. Not measured on Nemyu."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_HINT_PAUSE",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Guest spin-wait hints (YIELD, WFE) give the core's SMT sibling a moment with two "
            "CPU pause instructions."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The sibling, often a GPU thread, runs while a guest thread spins (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 makes the hints do nothing again."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_ATOMICS",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Guest LSE atomics (LDADD, SWP, CAS and friends) become one host atomic instruction "
            "instead of an exclusive load and store that ends the block. Needs fastmem "
            "exclusives and the global monitor off (Unsafe CPU accuracy)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer block exits in code that uses atomics heavily (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 keeps the exclusive load/store emulation."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_ATOMICS_MONITOR",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Guest LSE atomics become host atomics (EDEN_NX2_JIT_ATOMICS) also while the global "
            "monitor is on: Accurate CPU accuracy, or EDEN_CPU_GLOBAL_MONITOR."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With Accurate CPU accuracy, each atomic no longer takes the exclusive monitor's lock, "
            "which all guest cores share, twice and ends its block."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "An exclusive store on another core no longer fails when an atomic wrote the value it "
            "had read back unchanged, as with Auto. Not measured on Nemyu."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_JIT_BACKEDGE_HALT",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Halt checks only on links to the same or a lower guest PC and on indirect "
            "branches, instead of on every conditional branch (multicore, without cycle "
            "counting)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shorter block exits; every loop still sees a halt request (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 checks on every branch again."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_BRANCH_CACHE",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Each guest indirect branch (BR, BLR) keeps a one-entry cache of the block it last "
            "went to, filled from the fast dispatch table and emptied whenever code is "
            "invalidated (notsapinho)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Indirect branches skip the table's hash and get their own host indirect jump "
            "(0.994-0.997 of the JIT cycles per guest instruction in Pokopia, measured by "
            "notsapinho)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "16 bytes per compiled branch until the code cache is cleared, and a pass over all "
            "entries when code is invalidated. Not measured on Nemyu."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_WARM",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Each CPU core keeps a list of the game code it recompiled, by code region and offset "
            "(Switch 2 games load at another address every boot), in the cache folder's nx2_jit, "
            "and recompiles it before the game first runs in the next session, all cores at once. "
            "A region whose start or size changed (another game version) is skipped."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer frames held up by recompiling game code on its first run in a session: "
            "Splatoon Raiders' slow frames spent 9.1 s of a session recompiling, up to 49,000 "
            "blocks in one frame of its hub load; with the list, 0.4 s. x64 hosts only."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A longer boot (~5 s more in Splatoon Raiders) and a list file of a few MB per game. "
            "Each core stops warming with 128 MiB of its code cache left. Not measured on Nemyu."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_COMPACT_EXITS",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Unlinked block exits jump to one shared stub per target, so patch sites are 5 or 6 "
            "bytes instead of 22 or 23 plus an inline stub."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Denser code and fewer instruction cache misses (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 restores inline exit stubs."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_DISPATCH",
        .category = Category::CpuTiming,
        .type = ValueType::Text,
        .suggested = "16",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2^16 entries, 4-way"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Size of the JIT's fast dispatch table: unset gives 2^16 entries, 4-way set "
            "associative (1 MiB per core instead of 16 MiB); 0 the old direct-mapped 2^20 "
            "table; 8-24 that many bits."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Indirect branches hit a table that fits in the CPU caches (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Too small a table misses more often."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_FAR_CODE",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fault and abort paths, atomic fallbacks and exit stubs go to a cold region at the "
            "end of the code cache."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Hot blocks sit densely in the instruction cache (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 keeps cold paths inline."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_FLAGS",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Branches, CSEL and CBZ/TBZ read the host flags the instruction right before them "
            "left, instead of reloading NZCV from the guest state (without cycle counting)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer instructions on every compare and branch (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 reloads NZCV every time."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_FOLD_ADDR",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A base plus immediate address used by one load or store is folded into that "
            "access's addressing mode instead of computed with LEA (unchecked 64-bit fastmem)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer instructions per guest memory access (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 computes the address with LEA again."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_INLINE_FRINT",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Scalar FRINTA (round()) and FRINTX (rint()) run as a few SSE4.1 instructions instead "
            "of a call into the soft-float library, with the same results and flags. Blocks "
            "running with flush-to-zero keep the call."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "No host call (register spills and a function call) per rounding in code that rounds "
            "floats, such as timers, animation frames and UI snapping."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 calls the soft-float again. Not measured on Nemyu."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_INLINE_RET",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A guest return in a block that leaves the FPCR alone pops the return stack buffer "
            "inline instead of jumping to the shared handler (notsapinho)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Each return has its own indirect jump for the host's branch predictor and skips the "
            "FPCR load (0.990-0.991 of the JIT cycles per guest instruction in Pokopia, measured "
            "by notsapinho)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "About 50 more bytes of code per return. 0 jumps to the shared handler as before. Not "
            "measured on Nemyu."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_NO_FRAME",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Blocks do not save and set up %rbp as a frame pointer, which nothing in the JIT "
            "reads."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Three instructions less per block (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 restores the frame."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_RSB32",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The return stack buffer keeps 32 entries instead of 8."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Returns from deeper call chains skip the dispatcher (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 keeps 8."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_SCALAR_XMM",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Scalar float operands and results stay in XMM registers instead of a round trip "
            "through general registers (notsapinho)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Float-heavy guest code runs in fewer host instructions (2.6-3.7% fewer JIT cycles "
            "per guest instruction in Pokopia, measured by notsapinho)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 moves them through general registers as before. Not measured on Nemyu."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_SCRATCH_COPY",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "An operand an instruction overwrites but that is still needed is copied to a free "
            "register instead of being evicted."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer spills to the stack (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 evicts as before."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_SPILL_GPR",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A value the register allocator must evict moves to a free callee-saved register "
            "instead of the stack (about 9% of instructions spilled in Ocarina of Time's hot "
            "code)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer stack loads and stores (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 spills to the stack as before."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_TRAPPED_READS",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With reactive flushing off, a guest load that faults on fast memory (mostly at a "
            "read trap, a page the GPU wrote) is recompiled off fast memory on its 8th fault "
            "instead of its first."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shared code that once read GPU output, such as memcpy, keeps its fast loads instead "
            "of walking the page table, and calling into the emulator for GPU-cached pages, for "
            "the rest of the session."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A load that faults on every access (unmapped memory) takes 7 more faults before it "
            "is recompiled. Not measured on Nemyu."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_JIT_ZEXT",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Writes and zero-extends of values a 32-bit instruction produced skip the second "
            "mov r32, r32."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer instructions in 32-bit code (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 zero-extends every time."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_X64_TSO",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "On x64 hosts, guest acquire loads (LDAR) are plain loads instead of locked "
            "instructions, and DMB barriers over only loads or only stores emit no fence: x64's "
            "memory order already gives both. Release stores stay full barriers."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Guest locks and atomics no longer take each other's cache lines on every acquire "
            "load (Ocarina of Time has about 68,000 such loads in its static init guards; on by "
            "default in Lichii #717 and Fritchii)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 restores the locked loads and the fences, for A/B tests. Exclusive loads keep "
            "their locked form."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_CONTEXT_GUARD_PAUSE",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Pauses the CPU between tries for a thread's context guard in the scheduler."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 retries back to back, starving the SMT sibling."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_DUMMY_WAKE_UNLOCKED",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Wakes host service threads after the global scheduler lock is released, instead of "
            "under it."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 makes every core wait for each wake-up system call."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_EVENT_WAIT_LEGACY",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Brings back the timed event wait from before the MWAITX/UMWAIT fix."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A wake the signal didn't cause returned false early, and the fallback returned true "
            "at the deadline."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_GPU_WAIT_SPIN_US",
        .category = Category::CpuTiming,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 100000,
        .suggested = "50",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: no polling"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How long waits for the host GPU, and the Vulkan worker waiting for work, poll "
            "before they sleep, in microseconds."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Lower wake-up latency."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A busy core while waiting."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_HIGH_PRIORITY",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Runs the emulator at the high process priority class on Windows (Selevet's switch, on "
            "by default in its performance builds 1-6). Read at startup."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Background programs yield the CPU to the emulator, which can help on CPUs with few "
            "cores."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Other programs, the desktop included, get less CPU time while a game runs."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_IDLE_SPIN_US",
        .category = Category::CpuTiming,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 100000,
        .suggested = "50",
        .default_value = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "100 µs on Switch 2 titles with 8 or more CPU threads, else 0"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How long an idle emulated core polls for new work before it blocks, in "
            "microseconds."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lower wake-up latency for job systems (Ocarina of Time: 42 ms frames without, 34 ms "
            "with 100 µs)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Spinning cores take CPU time from the GPU and shader threads, and heat and battery "
            "on laptops; 300 µs was slower again."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_ARB_SPIN",
        .category = Category::CpuTiming,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 1000,
        .suggested = "8",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "8 µs"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How long, in microseconds, a guest thread about to wait on an address first watches "
            "the word without a lock, in case the signal comes a moment later."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Job systems hand work between threads many times a frame: a short watch skips the "
            "sleep and wake-up round trip (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 turns it off. Threads whose watches rarely pay off stop watching on their own."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_CORE_LOCK",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "An emulated core's context lock is a spin lock, and an idle core sleeps on an address "
            "wait instead of a mutex and condition variable."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Cheaper core switches and wake-ups of idle cores (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 restores the mutex and condition variable."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_IDLE_PAUSE",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "An idle emulated core polls with CPU pause instructions instead of yielding its host "
            "thread while EDEN_NX2_IDLE_SPIN_US runs."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "No system call per poll, and the core's SMT sibling (often a GPU thread) keeps most "
            "of the core (Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 restores the yield loop."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_KSPIN",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A contended kernel lock spins for EDEN_NX2_KSPIN_US before it sleeps on an address "
            "wait."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Holders keep these locks for microseconds, so most waits end without a sleep "
            "(Lichii #717)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 restores the short spin and std::atomic waits."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_KSPIN_US",
        .category = Category::CpuTiming,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 1000,
        .suggested = "20",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "20 µs"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How long a contended kernel lock spins before it sleeps, in microseconds "
            "(EDEN_NX2_KSPIN)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Longer spins avoid more sleeps when locks are busy."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Spinning takes CPU time from other threads."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_TIMING_DROP_EXPIRED",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Drops a timing event that expired without being unscheduled."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 can spin forever with both timing locks held: a freeze."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TIMING_WAKE_EARLIEST",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Wakes the timing thread for a new event only when it became the earliest one."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 wakes it for every event."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_HOSTTIMING_SLEEP",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The timing thread sleeps in the OS until its next event, on a high-resolution "
            "waitable timer on Windows. 0 waits in MWAITX/UMWAIT, which keeps a core busy."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Frees about one CPU core (Lichii: the timing thread 99.7% of a core -> 1-4%, no "
            "change in fps or frame-time p90 in Donkey Kong Bananza, Ocarina of Time and Mario "
            "Kart World)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Timed events run a little later (Lichii: p50 0.1-0.3 ms, p99 about 0.5 ms)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_TIMING_COARSE",
        .category = Category::CpuTiming,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The timing thread sleeps on the OS through a wait longer than 2 ms (or four timer "
            "ticks) and only waits precisely for the last 1 ms (Selevet's switch). Nothing is left "
            "for it to do while EDEN_NX2_HOSTTIMING_SLEEP is on."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Frees the core the timing thread kept busy while waiting; can help on CPUs with few "
            "cores or laptops."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "An OS wait that ends late delays timed events: frame pacing or audio could suffer."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_FRAME_GEN_NO_PACING",
        .category = Category::VideoAudio,
        .type = ValueType::Presence,
        .flags = EnvironmentOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Presents generated frames back to back instead of spacing them over the frame "
            "interval."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Uneven frame pacing with frame generation."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles with frame generation"),
    },
    {
        .name = "EDEN_NX2_APPLET_CAPTURE_EVERY_FRAME",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Draws the applet capture of Switch 2 frames at every composite, as before, instead "
            "of only when it is read."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Extra GPU work every frame."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_CAPTURE_SETS",
        .category = Category::VideoAudio,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 8,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "3"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How many sets of layer resources applet captures rotate through (1 to 8)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1 uses a little less memory."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With 1, each capture waits for the host GPU to finish the previous one."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_CROP_HEIGHT",
        .category = Category::VideoAudio,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 16384,
        .suggested = "1080",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Automatic"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Height of the shown region of Switch 2 present images; needs EDEN_NX2_CROP_WIDTH "
            "too."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fixes a title drawn into a corner of its images with black margins."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A wrong size cuts off or pads the picture."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_CROP_WIDTH",
        .category = Category::VideoAudio,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 16384,
        .suggested = "1920",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Automatic"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Width of the shown region of Switch 2 present images; needs EDEN_NX2_CROP_HEIGHT "
            "too."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fixes a title drawn into a corner of its images with black margins."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A wrong size cuts off or pads the picture."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_DECODER_IMAGE_MAP",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A video decoder output image the game maps for the GPU (gsv 426) is mapped at its "
            "own address when no GPU mapping covers its memory yet, and unmapped again with 427, "
            "430, 912 or 112."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The GPU reads the decoded frames instead of unmapped memory (black movies in "
            "Eglantine's Fortune's Weave)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog", "0 is for comparisons; it brings back the unmapped images."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog", "Switch 2 titles that sample their movies on the GPU"),
    },
    {
        .name = "EDEN_NX2_FG_KEEP_FIFO",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With frame generation, keeps a FIFO VSync mode instead of switching to Mailbox, as "
            "before."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Keeps the VSync mode you picked."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Every generated frame waits for a vblank of its own, and the real frame waits "
            "behind them."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles with frame generation"),
    },
    {
        .name = "EDEN_NX2_FRAMES_AHEAD",
        .category = Category::VideoAudio,
        .type = ValueType::Choice,
        .choices = FRAMES_AHEAD_CHOICES,
        .suggested = "auto",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1, but 0 for Donkey Kong Bananza"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Which frame's GPU work a Switch 2 present waits for. Waiting for the previous one "
            "lets the game prepare the next frame while the GPU thread runs this one."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "1 overlaps CPU and GPU work (8-56 ms of every frame were blocked in Mario Kart "
            "World); 0 or auto fix games that rewrite command memory too early."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "1 on Donkey Kong Bananza misreads commands: freezes, crashes and rubberbanding. 0 "
            "is slower."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Mario Kart World, Donkey Kong Bananza"),
    },
    {
        .name = "EDEN_NX2_MKW_FPS_MODE",
        .category = Category::VideoAudio,
        .type = ValueType::Choice,
        .choices = MKW_FPS_MODE_CHOICES,
        .suggested = "dynamic",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "The per-game setting"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Overrides Mario Kart World's frame-rate mode setting, which patches how the game "
            "picks its present interval."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "dynamic keeps the game speed right at uneven frame rates; 30 steadies it on slow "
            "PCs."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "dynamic forced with a CPU overclock or off-speed timing runs the game at the wrong "
            "speed; off is for performance comparisons."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Mario Kart World"),
    },
    {
        .name = "EDEN_NX2_NV12_LEGACY",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Drops the check that an NV12 row fits the output pitch when decoded frames are "
            "copied."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The scaler can write past rows and past the buffer."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_OPUS_GUEST_STATE",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Brings back the old Opus decoder layout: the libopus state in the guest's work "
            "buffer, sized by the host libopus."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Work buffer sizes then depend on how the host libopus was built."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_PACE_HOST_SLEEP",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Present pacing sleeps the service thread on the host instead of in the guest "
            "kernel."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 frees guest core 3 during the wait, so game threads there keep running (Fortune's "
            "Weave's loader read one RomFS block per frame otherwise)."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 is opt-in and little tested."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fire Emblem: Fortune's Weave; all Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_PRESENT_LEGACY_ACQUIRE",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Acquires swapchain images as before: with the swapchain's semaphore for the frame "
            "index, no timeout, a scheduler wait on the present thread, and the whole frame "
            "tried again when presenting fails."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Brings back hangs and stalls on the present thread."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_PACING_VIDEO",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "While a video decodes, the frame pacing target is ignored and the game runs at its "
            "own rate. 0 keeps the target during videos too."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Movies stay in sync with their audio with a 30 FPS target (Ocarina of Time's 60 fps "
            "intro played at half speed)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A game that decodes video all the time runs uncapped while it does."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles, with a frame pacing target"),
    },
    {
        .name = "EDEN_NX2_PRESENT_QUEUE",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A Switch 2 present returns at once with a release fence signalled after the frame's "
            "swap interval, instead of holding the game until its vblank. A game that doesn't "
            "wait for the fence is held once it gets 4 frames ahead. Can be a per-title "
            "default."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Games that measure their frame time on the CPU no longer count the display wait "
            "(Super Mario Bros. Wonder NS2E fell to 30 fps with it)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Opt-in and little tested: a game that ignores the fence runs up to 4 frames ahead "
            "of the display (more input lag), and frame pacing may change."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Super Mario Bros. Wonder NS2E; to try on Mario Kart World and Ocarina of Time"),
    },
    {
        .name = "EDEN_NX2_FSR_EASU_DOWNSCALE",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off: area average"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the FSR window filters (Vulkan), a frame larger than the window (a resolution "
            "scale above the screen size) is shrunk by averaging the pixels each screen pixel "
            "covers, then sharpened by RCAS. 1 shrinks it with EASU, as before. A frame the size "
            "of the window or smaller still goes through EASU."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "1 only to compare: EASU is made to enlarge, and shrinking with it skips source "
            "pixels, which shimmers on thin edges."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None expected: only changes the look of downscaled frames with FSR selected."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_SWAPCHAIN_IMAGES",
        .category = Category::VideoAudio,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 8,
        .suggested = "2",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Not set: 3 when the surface allows"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How many swapchain images Vulkan asks for, within what the window surface allows. "
            "0 or unset keeps triple buffering."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "2 under FIFO (VSync on) keeps at most one finished frame waiting for the display: a "
            "frame less of input lag."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With 2, a frame that misses its vblank holds the next one back: more stutter when "
            "the game is close to full speed, and fewer frames in flight for the present thread."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_PRESENT_QUEUE_FRAMES",
        .category = Category::VideoAudio,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 4,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Not set: 4 frames"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With EDEN_NX2_PRESENT_QUEUE, how many frames a game's release fences may run ahead "
            "of the display before its present is held."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Less input lag at full speed: each frame ahead is one more frame between the input "
            "and the screen. Splatoon Raiders' hub ran 34-49 ms ahead at 60 fps, never held at "
            "4."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A frame that takes longer than its interval shows at once as a missed vblank "
            "instead of being absorbed by the frames queued ahead."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Splatoon Raiders"),
    },
    {
        .name = "EDEN_NX2_BEHIND_WAIT",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "When a Switch 2 present finds the previous frame still on the GPU thread, it also "
            "waits for the frame being presented. 0 waits for the previous frame only. The log's "
            "600-present line counts how often the GPU thread was a frame behind."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 keeps the game's CPU work and the GPU thread's side by side when the GPU thread is "
            "the limit, instead of one after the other every frame."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The wait was added because Splatoon Raiders rewrote command memory the GPU thread "
            "had not read yet on slow GPUs (misread commands, then crashes): 0 may bring those "
            "back. Not used by Ocarina of Time with its UI copy, which never waits for it."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Splatoon Raiders, Mario Kart World"),
    },
    {
        .name = "EDEN_NX2_HOST_PACE",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Host frame pacer: holds each finished frame until just before the display refresh "
            "it should be shown at, so a 60 fps game shows every frame for the same number of "
            "refreshes. Needs VSync on Mailbox or FIFO and a driver with VK_KHR_present_wait; "
            "off while frame generation or Turbo runs. =0 only measures (logged every 10 s)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Even frame pacing at the same frame rate: from Fritchii, frames shown for exactly "
            "two refreshes 97-99% of the time at 60 fps on 120 Hz vs 91-96% without, longest "
            "interval 22-34 ms vs 70-200 ms."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Up to a refresh of extra latency per frame; the game's frame period is locked to "
            "whole display refreshes (at most 1% slower)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Every Switch 2 game"),
    },
    {
        .name = "EDEN_NX2_PRESENT_SHARED_QUEUE",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Presents on the graphics queue under the submit lock, as before, instead of on a "
            "second queue."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A fallback for drivers that misbehave with a separate present queue."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A FIFO present waiting for a vblank holds the lock that submissions need."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_PRESENT_WAIT_MS",
        .category = Category::VideoAudio,
        .type = ValueType::Number,
        .minimum = 250,
        .maximum = 600000,
        .suggested = "16000",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "8000 ms"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How long a Switch 2 present may wait while the GPU syncpoint stands still before it "
            "gives up (60 s at most in total)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Higher rides out very long shader compile stalls."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lower lets a game get ahead of the GPU thread: desynced commands and crashes."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_REPORT_VRR",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Makes the Switch 2 SDK report a display with variable refresh rate (VRR) to the game, "
            "by patching its QueryVrrSupported at load (Selevet's switch, on in its performance "
            "builds 4-6)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Games that support VRR time their frames themselves instead of assuming 60 Hz, so "
            "their speed stays steady while the frame rate moves."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A game may take paths it never takes on a TV without VRR."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_STARFOX_FPS",
        .category = Category::VideoAudio,
        .type = ValueType::Number,
        .minimum = 30,
        .maximum = 60,
        .suggested = "60",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off: the game's own rate"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Patches Star Fox's frame loop to present at this rate, through its explicit-rate "
            "path, which updates its render delta too. Only the build it was made for is "
            "patched."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "60 FPS at normal game speed in local tests."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Replaces how the game picks its frame rate; levels other than those tested and "
            "other updates are unverified."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Star Fox"),
    },
    {
        .name = "EDEN_NX2_SWAP_INTERVAL",
        .category = Category::VideoAudio,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 4,
        .suggested = "2",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0: the game's own interval"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Holds every Switch 2 present for this many 60 Hz vblanks (but not Mario Kart World "
            "with a frame-rate mode)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "2 caps a title at 30 fps; 1 brings back one vblank per present."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A wrong value runs a game too fast or too slow: 1 on a 30 fps game doubles its "
            "speed."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_VDEC_DAV1D_THREADS",
        .category = Category::VideoAudio,
        .type = ValueType::Number,
        .minimum = 0,
        .maximum = 64,
        .suggested = "4",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 (FFmpeg's default)"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How many threads libdav1d decodes AV1 movies with. 0 leaves FFmpeg's default, which "
            "scales with the host's threads; 4 is the count used before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The default keeps more frames in flight: upstream saw Ocarina of Time's intro drop "
            "5-7 frames per 4200 at the default against 20-24 with half the host threads."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "More decoder threads compete with the emulator's own while an AV1 movie plays."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "Switch 2 titles with AV1 movies (Ocarina of Time)"),
    },
    {
        .name = "EDEN_NX2_VDEC_EOS_RELEASE_NOW",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "After a movie's end of stream, a frame release completes at once instead of staying "
            "pending until Stop or Finalize."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mario Kart World waits for those fences after its title movie."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Donkey Kong Bananza acquires a frame for every completed fence and aborts on an "
            "empty ring."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Mario Kart World, Donkey Kong Bananza"),
    },
    {
        .name = "EDEN_NX2_VDEC_FRAME_THREADS",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Decodes Switch 2 movies with frame and slice threads (4) instead of one thread."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 may help on CPUs with few cores."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 decoded 2560x1440 VP9 movies below 60 fps, and players abort when no frame is "
            "ready."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_VDEC_PACE",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps a movie clock from the frames the game acquires and skips decoded frames a later "
            "frame has already overtaken, so the picture keeps up with the movie's sound."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Games that show one movie frame per frame they draw (Ocarina of Time) stay in sync "
            "with the audio below 60 fps, for example with 30 fps frame pacing. At full speed "
            "nothing is skipped."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A slow game shows fewer movie frames. 0 keeps every frame, and the picture falls "
            "behind the sound instead."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles with movies"),
    },
    {
        .name = "EDEN_NX2_VIDEO_READY_GATE",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Stops signalling the video decoder's ready event after Stop and Finalize, and "
            "answers Start and Finalize with their real results."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 brings back the old replies."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_VOICE_DROP",
        .category = Category::VideoAudio,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Switch 2 titles that ask for it drop audio voices when the audio renderer goes over "
            "its time budget, as Switch 1 titles do."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Bounds the audio renderer's time when it runs late."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Sounds can cut out while the renderer is behind."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_VSYNC_HZ",
        .category = Category::VideoAudio,
        .type = ValueType::Number,
        .minimum = 1,
        .maximum = 1000,
        .suggested = "120",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "60 Hz"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The rate of the vsync event Switch 2 titles start their frames on (an experiment)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A frame that misses a tick idles less at higher rates."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Games may run at the wrong speed."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_CLI_RESTART_HANDOFF",
        .category = Category::Services,
        .type = ValueType::Text,
        .flags = EnvironmentOnly | CliOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Not set"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Set by the launcher when nemyu-cli restarts for a game's ExecuteProgram: the new "
            "process reads the handoff file (program, indexes and user channel) and boots that "
            "program."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: the launcher sets it."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Set by hand, a damaged or missing handoff file stops the boot."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Switch 2 titles that launch another program"),
    },
    {
        .name = "EDEN_NX2_AM320_ZERO",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Reports screenshots and recording as off in "
            "GetScreenShotAndContinuousRecordingInitiallyEnabled, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The old reply left callers reading stale message words."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_TENSORRT_SUPPORTED",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Tells titles the console can run TensorRT networks (IsTensorRtSupported), as a "
            "Switch 2 does. By default they are told it can't."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Titles may turn on their tensor-based upscaling, as on the console."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Slower boot, and in-game tensor upscaling runs through software HMMA, which can be "
            "very slow."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles that ask for TensorRT"),
    },
    {
        .name = "EDEN_NX2_AUTO_STUB",
        .category = Category::Services,
        .type = ValueType::Presence,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Answers every unknown service command with an empty success, like the Auto Stub "
            "setting. Switch 2 titles are always auto-stubbed."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Switch 1 titles get past unimplemented commands."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A stubbed command can make a game misbehave later instead of stopping now."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 1 titles"),
    },
    {
        .name = "EDEN_NX2_AUTO_STUB_ZERO",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Auto-stubbed commands reply with 32 zeroed words instead of the result alone."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 hands callers the request's leftover words as data (a slot mask, a device id)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_COMPAT_PLATFORM",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Tells a Switch 1 program that it runs on a Switch 2: nn::pl::IsRunningOnOunce, an SDK "
            "stub returning false, is patched at load to return true, as the Switch 2 system "
            "answers it (Satsuma's switch)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Switch 2 Editions of Switch 1 games can take their Switch 2 paths (resolution, frame "
            "rate, Switch 2 Edition content) instead of running as on a Switch 1."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The game still runs its Switch 1 program and only sees the add-on content that is "
            "installed or in the dump; Switch 2 paths may need content or hardware it doesn't "
            "find, and the game may crash or ask to buy the upgrade."),
        .games = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Switch 2 Editions of Switch 1 games (Pokemon Legends: Z-A, Super Mario Bros. Wonder)"),
    },
    {
        .name = "EDEN_NX2_EMPTY_SYSTEM_DATA",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Serves an empty archive for Switch 2 system data that Switch 1 firmware doesn't "
            "have."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 fails the request, and titles abort (Nintendo Switch Sports Resort)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_ERROR_NOTIFIER_CLEAR",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Clears a GPU channel's error notifier buffer when it is installed, like the host "
            "driver."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 leaves old data that NVN reads as a channel error."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_FONT_DATA_LEGACY",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Brings back the old guess at the Switch 2 GetBuiltInFontData layout."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Clamped font types and a wrong reply layout."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_GSV_HOST_THREAD",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Runs the Switch 2 graphics system service on its own host thread instead of on AM's "
            "server."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Presents stop occupying guest core 3."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Opt-in; not used with single-core CPU emulation."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_KEEP_SESSIONS",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps the server's reference to client-owned sessions (video decoders) after the "
            "client closes them, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Each kept a decoder thread, an FFmpeg context and a syncpoint, per movie."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_MOTION_RESTART",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Restarts the motion sensors of an SDL controller that stopped reporting them, after "
            "1 s and then backing off to every 8 s while it stays silent."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Motion comes back on controllers that stop sending it when another program (Steam "
            "Input) changes their sensor mode, or after they reconnect."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A controller that lists sensors but never reports them gets them switched off and on "
            "every 8 s, with a log line each time."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles (Qt frontend)"),
    },
    {
        .name = "EDEN_NX2_NFP_INIT_EX",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Implements the Switch 2 amiibo (NFP) commands, InitializeEx and the others from 300 "
            "up."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 leaves it unimplemented."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 titles that use amiibo"),
    },
    {
        .name = "EDEN_NX2_NUP_CACHE_MIB",
        .category = Category::Services,
        .type = ValueType::Number,
        .minimum = 64,
        .maximum = 65536,
        .suggested = "4096",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "512"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "MiB of decompressed NUP members (game files) kept in memory. Members over 256 MiB "
            "still go to a temporary file."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Fewer hitches when an area loads files the cache had evicted (the log names each "
            "decompression over 2 ms)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog", "Up to that much more RAM while the game runs."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Switch 2 games dumped as NUP"),
    },
    {
        .name = "EDEN_NX2_SIXAXIS_LEGACY",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Reports zeros for the motion sensor's ranges and calibration, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Zero ranges break the guest's sensor fusion: broken motion controls."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_STREAMPLAY_300",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Answers GameShare (stream play) user service command 300 with guest HID shared "
            "memory."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Experimental GameShare support."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Donkey Kong Bananza runs with the auto stub's reply instead."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "GameShare titles, Donkey Kong Bananza"),
    },
    {
        .name = "EDEN_NX2_SWKBD_NATIVE",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Reads the Switch 2 software keyboard config revisions with their own layout."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 reads them with the Switch 1 layout."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_SYNCPT_POLL_DELAY_US",
        .category = Category::Services,
        .type = ValueType::Number,
        .flags = Diagnostic,
        .minimum = 0,
        .maximum = 400,
        .suggested = "200",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Test only: holds each syncpoint poll this long between its two reads, up to 400 µs "
            "(multicore only)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With EDEN_NX2_SYNCPT_VALUES_FIRST=1, reproduces the movie player abort."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Delays every wait it answers, so frame times mean nothing."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SYNCPT_PUBLISH_IN_ACTION",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Publishes syncpoint values as they change, instead of leaving them to the 500 µs "
            "poll."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 makes movie players find their fences unreached and wait more often."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_SYNCPT_SIGNAL_NEW_SLOTS",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Creates the events of new syncpoint slots signalled."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 can leave a movie that got a fresh slot waiting forever."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_SYNCPT_VALUES_FIRST",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The syncpoint poll reads the values before the armed waits, as before."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Mario Kart World's movie player aborted (2162-0001) about once per two minutes of "
            "movie."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Mario Kart World"),
    },
    {
        .name = "EDEN_NX2_SYNC_SUBMIT",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Diagnostic: each GPU submission waits, up to 2 s, until the GPU has run it."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shows whether a title rewriting command memory it already submitted (the Yoshi "
            "demo, per rdenno) still misreads commands."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The game's CPU waits for the GPU every submission: much slower, and a submission "
            "waiting on a later CPU release stalls for the full 2 s."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Yoshi and the Mysterious Book demo"),
    },
    {
        .name = "EDEN_NX2_TSG_STUB_ALL",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lets every GPU TSG ioctl succeed, as before, instead of only the implemented ones."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "None; it brings back the old behaviour, for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The SDK probes the others and must see them unsupported."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_VI2_CLIENT_STATE",
        .category = Category::Services,
        .type = ValueType::Toggle,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Implements the client state commands of the Switch 2 display service (vi2)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None expected; 0 is for comparisons."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "0 leaves them to the auto stub, and command 11 to an event that never fires."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_LOG_DEBUG_OUTPUT",
        .category = Category::Diagnostics,
        .type = ValueType::Toggle,
        .flags = Diagnostic | EnvironmentOnly,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Sends log lines to the Windows debugger output even with no debugger attached (for "
            "DebugView)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Read the log live with DebugView."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Slower logging."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NATIVE_DRAW_CAPTURE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic | EnvironmentOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Writes the state of the first 16 native draws to native-draw-* files in the working "
            "folder."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Disk writes."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NATIVE_DRAW_DIAGNOSTICS",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic | EnvironmentOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Writes native macro, QMD and constant buffer dumps and a query trace to the working "
            "folder."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Slower, with many disk writes."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NATIVE_FRAME_CAPTURE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic | EnvironmentOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Writes each context's native graphics macro code to a file and logs the native "
            "display source."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NATIVE_SHADER_OVERRIDE_DIR",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic | EnvironmentOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "For a native shader that fails to translate, writes its code to this folder and "
            "pauses up to 30 minutes until a <key>.override.bin appears there."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Test hand-written shader replacements."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "The game stops while it waits."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_3D_HISTORY",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Keeps the last 48 3D register writes for error logs (always on in debug builds)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A small cost on every register write."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_3D_METHODS",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs the first write to each 3D register (finds methods the emulator doesn't "
            "model)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A little log noise."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_AUDIO_LEVEL",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs the audio output peak about every two seconds."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Shows whether a game is audible."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A little log noise."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_BACK_SPLIT_VERIFY",
        .category = Category::Diagnostics,
        .type = ValueType::Toggle,
        .flags = Diagnostic,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With EDEN_NX2_BACK_SPLIT, also prepares each draw the usual way, uses that result "
            "and logs every 10 s how often the workers' result differed (the first 16 "
            "differences in full)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Shows whether the back split is safe."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Slower than without the back split: its frame rate says nothing about the gain."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_CB24_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "For draws whose vertex shader reads constant bank 24, logs the inline constant "
            "buffer upload target and its first words."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_CRASH_DUMP",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic | CliOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "On a crash, prints the faulting stack as module+offset and writes a minidump to "
            "this folder (Windows)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Crash reports with a stack."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Disk space for the dump."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_CRASH_LOG",
        .category = Category::Diagnostics,
        .type = ValueType::Toggle,
        .flags = Diagnostic,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "On a crash, the log ends with the exception code, the faulting address and the "
            "stack as module+offset (Windows). 0 leaves the crash to Windows Error Reporting "
            "alone."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "A crash can be diagnosed from the log alone."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_DLSS_LOG",
        .category = Category::Diagnostics,
        .type = ValueType::Number,
        .flags = Diagnostic,
        .minimum = 0,
        .maximum = 2,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs NVIDIA Streamline's messages: 1 its information lines, 2 verbose. Only builds "
            "with NVIDIA DLSS."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds why NVIDIA DLSS doesn't run."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_DUMP",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "<present>:<hex address>:<hex size>[,<address>:<size>...]: at that present, flushes "
            "those GPU ranges and saves them as nx2-dump-<address>.bin in the "
            "EDEN_NX2_FRAME_CAPTURE folder."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Inspect what a pass rendered, offline."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A stall at that present."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_FAST_DRAW_VERIFY",
        .category = Category::Diagnostics,
        .type = ValueType::Toggle,
        .flags = Diagnostic,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With EDEN_NX2_BUFFER_SKIP, EDEN_NX2_INLINE_INVALIDATE, EDEN_NX2_DYNAMIC_SKIP or "
            "EDEN_NX2_FAST_DRAW, does the skipped work anyway and logs every 10 s how often it "
            "would have changed something."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Shows whether those skips are safe."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Slower than without the skips."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_FLASH_WATCH",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Looks for flashes too short to capture by hand and writes reports to this folder: "
            "every frame, the outputs of every pass and the presented image are reduced to tiles, "
            "and a presented tile brighter than the frames around it is reported with the first "
            "output that was abnormal."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the pass behind a flash."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A compute pass over every output each frame: much slower."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_FLASH_WATCH_DELTA",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .suggested = "0.15",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0.15"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the flash watch, how much brighter (mean, absolute) a presented tile must also "
            "be than the frames around it to count as a flash."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Lower values catch fainter flashes."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Lower values report more noise."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_FLASH_WATCH_HOT",
        .category = Category::Diagnostics,
        .type = ValueType::Number,
        .flags = Diagnostic,
        .minimum = 1,
        .maximum = 1000000,
        .suggested = "1024",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1024"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the flash watch, the channel magnitude from which a texel counts as hot in the "
            "tiles."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Matches the game's colour range."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_FLASH_WATCH_MAX",
        .category = Category::Diagnostics,
        .type = ValueType::Number,
        .flags = Diagnostic,
        .minimum = 0,
        .maximum = 10000,
        .suggested = "10",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "10"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the flash watch, the most reports written in a session; later flashes are only "
            "logged."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "More reports for a long session."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Each report takes disk space."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_FLASH_WATCH_PASSES",
        .category = Category::Diagnostics,
        .type = ValueType::Toggle,
        .flags = Diagnostic,
        .suggested = "0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "On"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the flash watch, watches the outputs of every pass; 0 watches only the "
            "presented image."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 costs much less."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0 can't say which pass flashed."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_FLASH_WATCH_PASS_RATIO",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .suggested = "2.0",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "2.0"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the flash watch, how many times its mean in the frames around it an output must "
            "reach to count as abnormal in a report."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Lower values flag fainter outputs."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Lower values flag more outputs."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_FLASH_WATCH_RATIO",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .suggested = "1.5",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "1.5"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With the flash watch, how many times brighter than the frames around it a presented "
            "tile must be to count as a flash."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Lower values catch fainter flashes."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Lower values report more noise."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_FRAME_CAPTURE",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Every 120th present, saves the shown frame as nx2-frame-<n>.tga in this folder."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Lets hidden test runs check the picture."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A readback and a file write every 120 presents."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All Switch 2 titles"),
    },
    {
        .name = "EDEN_NX2_GPU_PROFILE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Times where the GPU thread spends its time and logs the zones every 10 s."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Shows what limits the GPU thread."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A little overhead in every zone."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_HIDE_WINDOW",
        .category = Category::Diagnostics,
        .type = ValueType::Toggle,
        .flags = Diagnostic | CliOnly,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Creates the render window hidden and unfocusable."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Unattended test runs don't cover or interrupt the desktop."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "You can't see or use the game."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_IPC_SUMMARY",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Every 5 s, logs how often each service command ran and with which result."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Finds a game polling something that never becomes ready."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Counts every request: slower, and fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_IPC_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs every dispatched service command and the result it returned."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Very slow, and huge logs."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_JIT_STATS",
        .category = Category::Diagnostics,
        .type = ValueType::Toggle,
        .flags = Diagnostic,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Every 10 s while guest code is compiled, logs the JIT blocks compiled, their code "
            "size, the compile time (and the longest compile), and how many of them another CPU "
            "core had compiled already."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shows whether JIT compiles, and compiling the same code once per core, cost time in "
            "a scene."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A lock and a map entry per compiled block (a few MiB per game)."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_KEY_RECORD",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic | CliOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Appends each key press and release (present number and SDL scancode) to this file."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Records input for EDEN_NX2_KEY_SCRIPT."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A file write per key."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_KEY_SCRIPT",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic | CliOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Presses and releases keys by SDL scancode at set times: "
            "\"seconds:code,seconds:-code,...\" or \"@<file>\"; code 0 ends the run."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Drives menus in unattended test runs."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Keys pressed on their own."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_LOG_PIPELINE_COMPILES",
        .category = Category::Diagnostics,
        .type = ValueType::Toggle,
        .flags = Diagnostic,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs every driver pipeline compile when it starts and when it returns, with its "
            "shader hashes and the time it took."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A freeze inside the driver's compiler leaves a start line with no done line, naming "
            "the shader."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log while shaders build."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_MEM_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs a title's memory layout queries and its heap and physical memory requests."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A little log noise."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_METHOD_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Logs every GPU engine method."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Very slow, and huge logs."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_MME_DUMP",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Writes the compute macro code to a file once per engine, and logs each compute "
            "macro call and macro data RAM changes."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_MME_LOG",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs every Switch 2 macro call at debug level (about 750,000 a second)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Very slow, and huge logs."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_MME_MEMO_STATS",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs the macro memo's counters every 16.7 million calls."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A little log noise."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_MME_RECORD",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Writes the first 200,000 macro calls, and then the macro code, to this file, to "
            "check interpreter changes offline."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Disk writes while it records."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_MME_RECORD_SKIP",
        .category = Category::Diagnostics,
        .type = ValueType::Number,
        .flags = Diagnostic,
        .minimum = 0,
        .maximum = 2147483647,
        .suggested = "1000000",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Skips this many macro calls before EDEN_NX2_MME_RECORD starts."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_PB_CHECK",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Hashes command lists when they are submitted and logs entries that changed before "
            "they ran."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Reads every list twice: slower."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_LIVE_READ_CHECK",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic | EnvironmentOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Compares each command list entry parsed from memory instead of its copy with the "
            "copy, and logs how often they differ, by the reason the copy was skipped."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Compares every entry read from memory; GPU-written pages differ often without a bug."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SNAPSHOT_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic | EnvironmentOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs the command list copies taken after submit that changed words (how long after "
            "submit, how many entries), a summary every 10 s, and channels that waited long on "
            "a semaphore or resumed late after the value was in memory."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog", "Compares every copy taken again with the one it replaces."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_PB_DUMP",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "At the first command list dump (from EDEN_NX2_PB_CHECK), also writes the whole "
            "command list to this file, for offline replay."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "One file write."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_PENDING_WRITE_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs bound buffer ranges that still hold guest CPU writes not yet applied to the "
            "host buffers, which a draw would read old (up to 100,000 lines)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds stale buffer reads."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Slower draws; fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_PERF_VERIFY",
        .category = Category::Diagnostics,
        .type = ValueType::Toggle,
        .flags = Diagnostic,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The GPU thread optimizations from Lichii also run the old code where they can "
            "(3D method dispatch, uniform uploads, texture unswizzles, storage buffer bindings, "
            "pipeline shader code and macro memory reads), log any difference (\"Method fast:\", \"Nx2Perf\") and keep "
            "the old result."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Shows whether those optimizations are exact."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Slower than without them."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_PIPELINE_DIFF",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "For a new graphics pipeline whose shaders an earlier one also uses, logs which "
            "fixed state differs."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Finds state that splits the same draws into many pipelines."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_PULLER_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Logs every GPU puller method."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Very slow, and huge logs."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_QMD_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs the compute QMD scheduler's loads, dependency counts and launches."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_QUERY_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs every GPU report (query) the game asks for."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_ROMFS_READ_LOG",
        .category = Category::Diagnostics,
        .type = ValueType::Toggle,
        .flags = Diagnostic,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off; on in Mario Kart World"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs the path of each RomFS file of 1 MiB or more the first time the game reads it, "
            "up to 2000 files a session."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shows which course or area the game loaded before a slow stretch of the log "
            "(Mario Kart World: which track a 600-present report belongs to)."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Up to 2000 log lines, and one walk of the RomFS's file table when the game opens it."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog",
                                   "All titles; on by default in Mario Kart World"),
    },
    {
        .name = "EDEN_NX2_RT_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs the first draw into every distinct colour target, and the first 2D blits and "
            "DMA copies."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SAMPLE",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic | CliOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Sampling profiler for the host threads named here (comma separated; a trailing * "
            "matches a prefix): records their call stacks (Windows)."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shows where named threads spend their time."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Pauses those threads about 1000 times a second while it records."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SAMPLE_AT",
        .category = Category::Diagnostics,
        .type = ValueType::Number,
        .flags = Diagnostic,
        .minimum = 0,
        .maximum = 2147483647,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "At the n-th upscale fallback, downloads its input, output and targets and counts "
            "their non-zero words."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The downloads froze a title for ~1.3 s."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SAMPLE_OUT",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic | CliOnly,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "nx2_samples.txt"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The file EDEN_NX2_SAMPLE writes its counts to."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shows where named threads spend their time."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Pauses those threads about 1000 times a second while it records."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SAMPLE_SECONDS",
        .category = Category::Diagnostics,
        .type = ValueType::Number,
        .flags = Diagnostic | CliOnly,
        .minimum = 1,
        .maximum = 86400,
        .suggested = "30",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "30 s"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "How many seconds EDEN_NX2_SAMPLE records for."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shows where named threads spend their time."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Pauses those threads about 1000 times a second while it records."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SAMPLE_START",
        .category = Category::Diagnostics,
        .type = ValueType::Number,
        .flags = Diagnostic | CliOnly,
        .minimum = 0,
        .maximum = 86400,
        .suggested = "60",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "60 s"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Seconds after start when EDEN_NX2_SAMPLE begins recording."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Shows where named threads spend their time."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Pauses those threads about 1000 times a second while it records."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SEM_WATCH",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs every release, acquire, report, inline write and macro parameter that touches "
            "the 16 bytes at this GPU address (hex)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SEM_WATCH_EMITS",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With EDEN_NX2_SEM_WATCH, also logs every write of a watched compute macro."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SHADER_DUMP",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Saves every captured native shader program, and the rejected ones, to this folder."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Lets the shader translation be tested offline."),
        .downsides = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "A disk write the first time each program is seen."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SPLIT_CHECK",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "While the split runs, reports with a stack trace any thread other than the second "
            "GPU thread that records into the Vulkan scheduler."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Finds the cause of crashes in command recording."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A small cost at every scheduler use."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SPLIT_VERIFY",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Every 4096 macro calls on the split's first thread, waits for the second and "
            "compares their register copies, logging and fixing differences."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Finds split bugs that leave wrong GPU state."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Stalls the GPU thread regularly."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_SVC_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "1 logs every SVC with its arguments and results; <first>:<last> logs only during "
            "presents first to last."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Very slow, and huge logs."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "All titles"),
    },
    {
        .name = "EDEN_NX2_TABLE_POINTER_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs each change of a native pointer table slot's entry or size, and whether the "
            "guest and host copies of the entry can differ."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Finds wrong buffers bound through pointer tables."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Slower binds; fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_TEXTURE_DUMP",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "With EDEN_NX2_WINDOW_TRACE, saves the start of the last traced texture's memory to "
            "this file."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A file write per traced draw."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_TRACE_AFTER_UPSCALE",
        .category = Category::Diagnostics,
        .type = ValueType::Number,
        .flags = Diagnostic,
        .minimum = 1,
        .maximum = 2147483647,
        .suggested = "1",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Traces the 40 draws that follow the n-th upscale fallback."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_TRACE_DST",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs DMA copies and 2D blits that write within 32 MiB above this GPU address (hex)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_TRACE_PS",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Limits EDEN_NX2_WINDOW_TRACE to the pixel shader at this address (hex)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_TRACE_VS",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Describes the next draws that use the vertex shader at this address (hex): render "
            "target, blending and textures."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_UPSCALE_SOURCE",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The upscale fallback blits from the same-sized image at this GPU address (hex) "
            "instead."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Wrong picture while set."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_UPSCALE_SOURCE_FORMAT",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "The render target format (hex) of EDEN_NX2_UPSCALE_SOURCE's image."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Wrong picture while set."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_VDEC_DUMP",
        .category = Category::Diagnostics,
        .type = ValueType::Text,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Saves the luma of every 120th decoded movie frame, at a quarter of its size, as "
            "nx2-vdec-<n>.pgm in this folder."),
        .gains = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Checks what the decoder wrote, apart from how the game shows it."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "A file write every 120 frames."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_VDEC_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Logs every video decoder call at Info level."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_WINDOW_TRACE",
        .category = Category::Diagnostics,
        .type = ValueType::Presence,
        .flags = Diagnostic,
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Off"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Describes clears and draws into 3840-pixel-wide colour targets (up to 400)."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
    {
        .name = "EDEN_NX2_WINDOW_TRACE_SKIP",
        .category = Category::Diagnostics,
        .type = ValueType::Number,
        .flags = Diagnostic,
        .minimum = 0,
        .maximum = 2147483647,
        .suggested = "1000",
        .default_value = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "0"),
        .description = QT_TRANSLATE_NOOP(
            "Nx2SwitchCatalog",
            "Skips this many events before EDEN_NX2_WINDOW_TRACE starts describing them."),
        .gains = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Finds the cause of a bug."),
        .downsides = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Fills the log."),
        .games = QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "None: a developer tool"),
    },
};

constexpr std::array<const char*, NUM_CATEGORIES> CATEGORY_NAMES{
    QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "GPU thread & command stream"),
    QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Buffer cache & memory"),
    QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Texture cache"),
    QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Shaders & pipelines"),
    QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "CPU, kernel & timing"),
    QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Video, presentation & audio"),
    QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "System services"),
    QT_TRANSLATE_NOOP("Nx2SwitchCatalog", "Diagnostics & logging"),
};

} // Anonymous namespace

std::span<const Entry> Entries() {
    return ENTRIES;
}

const Entry* Find(std::string_view name) {
    const auto it =
        std::ranges::find_if(ENTRIES, [name](const Entry& entry) { return name == entry.name; });
    return it != std::ranges::end(ENTRIES) ? &*it : nullptr;
}

QString Translate(const char* text) {
    if (text == nullptr || *text == '\0') {
        return {};
    }
    return QCoreApplication::translate("Nx2SwitchCatalog", text);
}

QString CategoryName(Category category) {
    return Translate(CATEGORY_NAMES[static_cast<size_t>(category)]);
}

} // namespace Nx2SwitchCatalog
