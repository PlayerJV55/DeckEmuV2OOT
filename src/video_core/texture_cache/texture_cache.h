// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <future>
#include <numeric>
#include <cstdlib>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>
#include <optional>
#include <bit>
#include "common/container/unordered_map.h"
#include <boost/container/small_vector.hpp>
#include <fmt/format.h>

#include "common/alignment.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/slot_vector.h"
#include "video_core/control/channel_state.h"
#include "video_core/dirty_flags.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/nx2_fast_draw.h"
#include "video_core/guest_memory.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/texture_cache/image_view_base.h"
#include "video_core/texture_cache/samples_helper.h"
#include "video_core/texture_cache/texture_cache_base.h"
#include "video_core/texture_cache/util.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"
#include "video_core/texture_barrier_epoch.h"
#include "video_core/textures/decoders.h"

namespace VideoCommon {

// EDEN_NATIVE_DRAW_DIAGNOSTICS, read once: some of its checks run at every fence.
inline bool NativeDrawDiagnostics() {
    static const bool enabled = std::getenv("EDEN_NATIVE_DRAW_DIAGNOSTICS") != nullptr;
    return enabled;
}

using Tegra::Texture::TICEntry;
using Tegra::Texture::TSCEntry;
using VideoCore::Surface::GetFormatType;
using VideoCore::Surface::PixelFormat;
using VideoCore::Surface::SurfaceType;
using namespace Common::Literals;

// An image download into guest memory that holds the GPU thread for 50 ms or more: logged with
// where the time went, 64 lines per site.
inline void ReportLongImageDownload(std::atomic<u32>& logged, const char* site, GPUVAddr gpu_addr,
                                    u64 bytes, std::chrono::steady_clock::duration recording,
                                    std::chrono::steady_clock::duration finishing,
                                    std::chrono::steady_clock::duration swizzling) {
    const auto download_time = recording + finishing + swizzling;
    if (download_time < std::chrono::milliseconds{50} ||
        logged.fetch_add(1, std::memory_order_relaxed) >= 64) {
        return;
    }
    const auto to_ms = [](std::chrono::steady_clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    };
    LOG_INFO(HW_GPU,
             "Long GPU-thread operation: image download ({}) {:.1f} ms ({:.1f} ms recording, "
             "{:.1f} ms in Finish, {:.1f} ms swizzling): {:#x} {} MiB",
             site, to_ms(download_time), to_ms(recording), to_ms(finishing), to_ms(swizzling),
             gpu_addr, bytes >> 20);
}

template <class P>
TextureCache<P>::TextureCache(Runtime& runtime_, Tegra::MaxwellDeviceMemoryManager& device_memory_)
    : runtime{runtime_}, device_memory{device_memory_} {
    // Configure null sampler
    TSCEntry sampler_descriptor{};
    sampler_descriptor.min_filter.Assign(Tegra::Texture::TextureFilter::Linear);
    sampler_descriptor.mag_filter.Assign(Tegra::Texture::TextureFilter::Linear);
    sampler_descriptor.mipmap_filter.Assign(Tegra::Texture::TextureMipmapFilter::Linear);
    sampler_descriptor.cubemap_anisotropy.Assign(1);

    // These values were chosen based on typical peak swizzle data sizes seen in some titles
    constexpr size_t SWIZZLE_DATA_BUFFER_INITIAL_CAPACITY = 8_MiB;
    constexpr size_t UNSWIZZLE_DATA_BUFFER_INITIAL_CAPACITY = 1_MiB;
    swizzle_data_buffer.resize_destructive(SWIZZLE_DATA_BUFFER_INITIAL_CAPACITY);
    unswizzle_data_buffer.resize_destructive(UNSWIZZLE_DATA_BUFFER_INITIAL_CAPACITY);

    // Make sure the first index is reserved for the null resources
    // This way the null resource becomes a compile time constant
    void(slot_images.insert(NullImageParams{}));
    void(slot_image_views.insert(runtime, NullImageViewParams{}));
    void(slot_samplers.insert(runtime, sampler_descriptor));

    if constexpr (HAS_DEVICE_MEMORY_INFO) {
        const s64 device_local_memory = static_cast<s64>(runtime.GetDeviceLocalMemory());
        const s64 min_spacing_expected = device_local_memory - 1_GiB;
        const s64 min_spacing_critical = device_local_memory - 512_MiB;
        const s64 mem_threshold = (std::min)(device_local_memory, TARGET_THRESHOLD);
        const s64 min_vacancy_expected = (6 * mem_threshold) / 10;
        const s64 min_vacancy_critical = (2 * mem_threshold) / 10;
        expected_memory = static_cast<u64>(
            (std::max)((std::min)(device_local_memory - min_vacancy_expected, min_spacing_expected),
                     DEFAULT_EXPECTED_MEMORY));
        critical_memory = static_cast<u64>(
            (std::max)((std::min)(device_local_memory - min_vacancy_critical, min_spacing_critical),
                     DEFAULT_CRITICAL_MEMORY));
        minimum_memory = static_cast<u64>((device_local_memory - mem_threshold) / 2);
    } else {
        expected_memory = DEFAULT_EXPECTED_MEMORY + 512_MiB;
        critical_memory = DEFAULT_CRITICAL_MEMORY + 1_GiB;
        minimum_memory = 0;
    }
    ApplyThresholdOverrides();

    const bool gpu_unswizzle_enabled = Settings::values.gpu_unswizzle_enabled.GetValue();

    if (gpu_unswizzle_enabled) {
        switch (Settings::values.gpu_unswizzle_texture_size.GetValue()) {
            case Settings::GpuUnswizzleSize::VerySmall:    gpu_unswizzle_maxsize = 16_MiB; break;
            case Settings::GpuUnswizzleSize::Small:        gpu_unswizzle_maxsize = 32_MiB; break;
            case Settings::GpuUnswizzleSize::Normal:       gpu_unswizzle_maxsize = 128_MiB; break;
            case Settings::GpuUnswizzleSize::Large:        gpu_unswizzle_maxsize = 256_MiB; break;
            case Settings::GpuUnswizzleSize::VeryLarge:    gpu_unswizzle_maxsize = 512_MiB; break;
            default:                                       gpu_unswizzle_maxsize = 128_MiB; break;
        }

        switch (Settings::values.gpu_unswizzle_stream_size.GetValue()) {
            case Settings::GpuUnswizzle::VeryLow: swizzle_chunk_size = 4_MiB; break;
            case Settings::GpuUnswizzle::Low:     swizzle_chunk_size = 8_MiB; break;
            case Settings::GpuUnswizzle::Normal:  swizzle_chunk_size = 16_MiB; break;
            case Settings::GpuUnswizzle::Medium:  swizzle_chunk_size = 32_MiB; break;
            case Settings::GpuUnswizzle::High:    swizzle_chunk_size = 64_MiB; break;
            default:                              swizzle_chunk_size = 16_MiB;
        }

        switch (Settings::values.gpu_unswizzle_chunk_size.GetValue()) {
            case Settings::GpuUnswizzleChunk::VeryLow: swizzle_slices_per_batch = 32; break;
            case Settings::GpuUnswizzleChunk::Low:     swizzle_slices_per_batch = 64; break;
            case Settings::GpuUnswizzleChunk::Normal:  swizzle_slices_per_batch = 128; break;
            case Settings::GpuUnswizzleChunk::Medium:  swizzle_slices_per_batch = 256; break;
            case Settings::GpuUnswizzleChunk::High:    swizzle_slices_per_batch = 512; break;
            default:                                   swizzle_slices_per_batch = 128;
        }
    } else {
        gpu_unswizzle_maxsize = 0;
        swizzle_chunk_size = 0;
        swizzle_slices_per_batch = 0;
    }
}

template <class P>
void TextureCache<P>::RunGarbageCollector(bool out_of_memory) {
    bool high_priority_mode = false;
    bool aggressive_mode = false;
    u64 ticks_to_destroy = 0;
    size_t num_iterations = 0;
    const auto Configure = [&](bool allow_aggressive) {
        high_priority_mode = out_of_memory || total_used_memory >= expected_memory;
        aggressive_mode =
            allow_aggressive && (out_of_memory || total_used_memory >= critical_memory);
        ticks_to_destroy = aggressive_mode ? 10ULL : high_priority_mode ? 25ULL : 50ULL;
        // EDEN_NX2_TC_MIN_AGE=<frames>: never evict images used more recently (thrash tests).
        static const u64 min_age = [] {
            const char* value = std::getenv("EDEN_NX2_TC_MIN_AGE");
            return value != nullptr ? std::strtoull(value, nullptr, 10) : 0ULL;
        }();
        // Only while memory allows: near the limit the age shrinks to 120 frames, at the limit
        // the collector evicts as usual (OoT's Saria cutscene ran out of GPU memory and crashed).
        if (!aggressive_mode) {
            ticks_to_destroy = (std::max)(ticks_to_destroy,
                                          high_priority_mode ? (std::min)(min_age, u64{120})
                                                             : min_age);
        }
        num_iterations = aggressive_mode ? 40 : (high_priority_mode ? 20 : 10);
    };
    // Below the expected memory nothing is urgent, but deleting an image can take a millisecond
    // (its overlaps, aliases, framebuffers and views go with it): up to 10 a frame were a sixth of
    // the GPU thread in OoT's fields. There, spend at most EDEN_NX2_TC_GC_BUDGET_US (default
    // 300, 0 = no limit) per frame; under memory pressure there is no limit.
    static const s64 gc_budget_ns = [] {
        const char* value = std::getenv("EDEN_NX2_TC_GC_BUDGET_US");
        return (value != nullptr ? std::strtoll(value, nullptr, 10) : 300LL) * 1000LL;
    }();
    // Over the expected memory up to 20 deletes a frame could hold the GPU thread ~20 ms, and
    // Ocarina of Time sits over that line all the time (its slack offset). There a pass stops after
    // EDEN_NX2_TC_GC_PRESSURE_US (0 = no limit, the default; 4000 for Ocarina of Time). Past the
    // critical memory or out of memory there is still no limit.
    static Common::Nx2::SessionValue<u32> pressure_budget_us{[] {
        const std::string_view text = Common::Nx2::Setting("EDEN_NX2_TC_GC_PRESSURE_US", "0");
        u32 result = 0;
        std::from_chars(text.data(), text.data() + text.size(), result);
        return result;
    }};
    const s64 gc_pressure_budget_ns = s64{pressure_budget_us.Get()} * 1000LL;
    const auto gc_start = std::chrono::steady_clock::now();
    // Images that must be downloaded before they go: their copies are recorded as the collector
    // picks them, and they are swizzled and deleted after one wait for the GPU per 64 MiB instead
    // of one wait each (OoT at the expected memory line: 868 waits in 600 frames).
    // EDEN_NX2_TC_GC_BATCH=0 downloads and deletes each one on its own.
    static const bool batch_downloads = [] {
        const char* value = std::getenv("EDEN_NX2_TC_GC_BATCH");
        return value == nullptr || value[0] != '0';
    }();
    // Even batched, each wait stops the GPU thread until the whole GPU is idle (a pipeline
    // bubble of tens of ms with the frame's work queued). The victims' downloads are instead
    // recorded and the images deleted once the GPU is past them (CompleteCollectorDownloads),
    // up to 512 MiB of them in flight; EDEN_NX2_TC_GC_ASYNC=0 waits as before.
    static const bool defer_downloads = [] {
        const char* value = std::getenv("EDEN_NX2_TC_GC_ASYNC");
        return value == nullptr || value[0] != '0';
    }();
    constexpr u64 MaxDeferredDownloadBytes = 512_MiB;
    struct PendingVictim {
        ImageId image_id;
        AsyncBuffer map;
        boost::container::small_vector<BufferImageCopy, 16> copies;
    };
    boost::container::small_vector<PendingVictim, 8> victims;
    u64 victim_bytes = 0;
    size_t victim_staging_bytes = 0;
    std::chrono::steady_clock::duration victim_recording{};
    const auto used_memory = [&] {
        return total_used_memory - (std::min)(total_used_memory, victim_bytes);
    };
    const auto Delete = [this](ImageId image_id) { CollectImage(image_id); };
    const auto FlushVictims = [&] {
        if (victims.empty()) {
            return;
        }
        if constexpr (requires { runtime.CurrentTick(); }) {
            if (defer_downloads &&
                collector_download_bytes + victim_bytes <= MaxDeferredDownloadBytes) {
                CollectorDownloadBatch batch{
                    .tick = runtime.CurrentTick(),
                    .bytes = victim_bytes,
                    .first_addr = slot_images[victims.front().image_id].gpu_addr,
                    .downloads = {},
                };
                batch.downloads.reserve(victims.size());
                for (PendingVictim& victim : victims) {
                    Image& image = slot_images[victim.image_id];
                    image.flags |= ImageFlagBits::CollectorDownload;
                    batch.downloads.push_back(CollectorDownload{
                        .image_id = victim.image_id,
                        .modification_tick = image.modification_tick,
                        .map = victim.map,
                        .copies = std::move(victim.copies),
                    });
                }
                collector_download_bytes += victim_bytes;
                collector_downloads.push_back(std::move(batch));
                victims.clear();
                victim_bytes = 0;
                victim_staging_bytes = 0;
                victim_recording = {};
                return;
            }
        }
        const auto finish_begin = std::chrono::steady_clock::now();
        runtime.Finish();
        const auto swizzle_begin = std::chrono::steady_clock::now();
        const GPUVAddr first_addr = slot_images[victims.front().image_id].gpu_addr;
        u64 guest_bytes = 0;
        for (PendingVictim& victim : victims) {
            Image& image = slot_images[victim.image_id];
            SwizzleImage(*gpu_memory, image.gpu_addr, image.info, victim.copies,
                         victim.map.mapped_span, swizzle_data_buffer);
            runtime.FreeDeferredStagingBuffer(victim.map);
            // Guest memory holds the GPU's data now (Delete must not mark it stale).
            image.flags &= ~ImageFlagBits::GpuModified;
            ClearStaleImage(image);
            guest_bytes += image.guest_size_bytes;
            ++gc_download_deletes;
            Delete(victim.image_id);
        }
        static std::atomic<u32> logged_collector_batches{};
        ReportLongImageDownload(logged_collector_batches, "collector batch", first_addr,
                                guest_bytes, victim_recording, swizzle_begin - finish_begin,
                                std::chrono::steady_clock::now() - swizzle_begin);
        victims.clear();
        victim_bytes = 0;
        victim_staging_bytes = 0;
        victim_recording = {};
    };
    const auto Cleanup = [&, gc_start](ImageId image_id) {
        if (num_iterations == 0) {
            return true;
        }
        const s64 budget_ns = !high_priority_mode               ? gc_budget_ns
                              : aggressive_mode || out_of_memory ? 0
                                                                 : gc_pressure_budget_ns;
        if (budget_ns > 0 &&
            std::chrono::steady_clock::now() - gc_start >= std::chrono::nanoseconds{budget_ns}) {
            return true;
        }
        --num_iterations;
        auto& image = slot_images[image_id];
        if (True(image.flags & (ImageFlagBits::IsDecoding | ImageFlagBits::CollectorDownload))) {
            return false;
        }
        const bool must_download = IsDownloadable(image) && False(image.flags & ImageFlagBits::BadOverlap);
        if ((!aggressive_mode && True(image.flags & ImageFlagBits::CostlyLoad)) || (!high_priority_mode && must_download)) {
            return false;
        }
        if (must_download && batch_downloads) {
            constexpr size_t MaxBatchBytes = 64_MiB;
            if (!victims.empty() &&
                victim_staging_bytes + image.unswizzled_size_bytes > MaxBatchBytes) {
                FlushVictims();
            }
            Image& victim = slot_images[image_id];
            const auto download_begin = std::chrono::steady_clock::now();
            auto map = runtime.DownloadStagingBuffer(victim.unswizzled_size_bytes, true);
            auto copies = FullDownloadCopies(victim.info);
            victim.DownloadMemory(map, copies);
            victim_recording += std::chrono::steady_clock::now() - download_begin;
            victim_staging_bytes += victim.unswizzled_size_bytes;
            victim_bytes += ImageMemoryBytes(victim);
            victims.push_back(PendingVictim{image_id, map, std::move(copies)});
        } else if (must_download) {
            const auto download_begin = std::chrono::steady_clock::now();
            auto map = runtime.DownloadStagingBuffer(image.unswizzled_size_bytes);
            const auto copies = FullDownloadCopies(image.info);
            image.DownloadMemory(map, copies);
            const auto finish_begin = std::chrono::steady_clock::now();
            runtime.Finish();
            const auto swizzle_begin = std::chrono::steady_clock::now();
            SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, map.mapped_span, swizzle_data_buffer);
            static std::atomic<u32> logged_collector_downloads{};
            ReportLongImageDownload(logged_collector_downloads, "collector", image.gpu_addr,
                                    image.guest_size_bytes, finish_begin - download_begin,
                                    swizzle_begin - finish_begin,
                                    std::chrono::steady_clock::now() - swizzle_begin);
            ++gc_download_deletes;
            image.flags &= ~ImageFlagBits::GpuModified;
            ClearStaleImage(image);
        }
        if (!(must_download && batch_downloads)) {
            Delete(image_id);
        }
        if (out_of_memory) {
            return false;
        }
        if (aggressive_mode && used_memory() < critical_memory) {
            num_iterations >>= 2;
            aggressive_mode = false;
        } else if (high_priority_mode && used_memory() < expected_memory) {
            num_iterations >>= 1;
            high_priority_mode = false;
        }
        return false;
    };
    // Deleting by age alone left every device memory block partly used: the collector deletes
    // thousands of images a minute and new ones land in the holes, so blocks seldom empty (title
    // 1: ~0.8 GB of holes in ~190 32 MiB blocks after ten minutes, which the driver counts as
    // used). Among the oldest candidates (8 per image this pass may delete), those in the
    // emptiest blocks go first, so blocks drain and go back to the driver; placement already
    // prefers the fullest blocks (best fit). Images with memory of their own sit in the middle.
    const auto Collect = [&] {
        if constexpr (requires(const Image& image) { image.MemoryBlock(); }) {
            std::vector<ImageId> candidates;
            const size_t limit = num_iterations * 8;
            lru_cache.ForEachItemBelow(frame_tick - ticks_to_destroy, [&](ImageId image_id) {
                candidates.push_back(image_id);
                return candidates.size() >= limit;
            });
            if (candidates.empty()) {
                return;
            }
            if (block_image_bytes_frame == ~0ULL || frame_tick - block_image_bytes_frame >= 60) {
                block_image_bytes_frame = frame_tick;
                block_image_bytes.clear();
                for (const auto [image_id, image] : slot_images) {
                    const auto block = image->MemoryBlock();
                    if (block.block != 0) {
                        block_image_bytes[block.block] += block.size;
                    }
                }
            }
            std::vector<std::pair<double, ImageId>> ordered;
            ordered.reserve(candidates.size());
            for (const ImageId image_id : candidates) {
                const auto block = slot_images[image_id].MemoryBlock();
                double fill = 0.5;
                if (block.block != 0 && block.block_size != 0) {
                    const auto it = block_image_bytes.find(block.block);
                    fill = it != block_image_bytes.end()
                               ? static_cast<double>(it->second) / static_cast<double>(block.block_size)
                               : 0.0;
                }
                ordered.emplace_back(fill, image_id);
            }
            std::stable_sort(ordered.begin(), ordered.end(),
                             [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [fill, image_id] : ordered) {
                if (Cleanup(image_id)) {
                    break;
                }
            }
        } else {
            lru_cache.ForEachItemBelow(frame_tick - ticks_to_destroy, Cleanup);
        }
        FlushVictims();
    };
    Configure(false);
    Collect();
    if (out_of_memory || total_used_memory >= critical_memory) {
        Configure(true);
        Collect();
    }
}

template <class P>
void TextureCache<P>::CollectImage(ImageId image_id) {
    auto& image = slot_images[image_id];
    MarkStale(image); // before UntrackImage (see MarkStale)
    if (True(image.flags & ImageFlagBits::Tracked)) {
        UntrackImage(image, image_id);
    }
    ++(True(image.flags & ImageFlagBits::CostlyLoad) ? gc_costly_deletes : gc_deletes);
    UnregisterImage(image_id);
    DeleteImage(image_id, image.scale_tick > frame_tick + 5);
}

template <class P>
void TextureCache<P>::CompleteCollectorDownloads() {
    if constexpr (requires { runtime.CurrentTick(); }) {
        while (!collector_downloads.empty() && runtime.IsFree(collector_downloads.front().tick)) {
            CollectorDownloadBatch& batch = collector_downloads.front();
            const auto swizzle_begin = std::chrono::steady_clock::now();
            u64 guest_bytes = 0;
            for (CollectorDownload& download : batch.downloads) {
                if (download.image_id) {
                    Image& image = slot_images[download.image_id];
                    image.flags &= ~ImageFlagBits::CollectorDownload;
                    // Written by the GPU since the copy was recorded: the copy is stale and the
                    // image is in use, so it stays. Written by the CPU since: guest memory is
                    // newer than the copy, as for any CpuModified image.
                    if (image.modification_tick == download.modification_tick) {
                        if (True(image.flags & ImageFlagBits::GpuModified) &&
                            False(image.flags & ImageFlagBits::CpuModified)) {
                            SwizzleImage(*gpu_memory, image.gpu_addr, image.info, download.copies,
                                         download.map.mapped_span, swizzle_data_buffer);
                            ClearStaleImage(image);
                            guest_bytes += image.guest_size_bytes;
                        }
                        image.flags &= ~ImageFlagBits::GpuModified;
                        ++gc_download_deletes;
                        CollectImage(download.image_id);
                    }
                }
                runtime.FreeDeferredStagingBuffer(download.map);
            }
            collector_download_bytes -= (std::min)(collector_download_bytes, batch.bytes);
            static std::atomic<u32> logged_deferred_batches{};
            ReportLongImageDownload(logged_deferred_batches, "collector batch, deferred",
                                    batch.first_addr, guest_bytes, {}, {},
                                    std::chrono::steady_clock::now() - swizzle_begin);
            collector_downloads.pop_front();
        }
    }
}

template <class P>
void TextureCache<P>::CancelCollectorDownload(ImageId image_id) {
    for (CollectorDownloadBatch& batch : collector_downloads) {
        for (CollectorDownload& download : batch.downloads) {
            if (download.image_id == image_id) {
                download.image_id = ImageId{};
                return;
            }
        }
    }
}

template <class P>
bool TextureCache<P>::SkipStaleUploads() {
    // EDEN_NX2_TC_SKIP_STALE=1. Off by default: in Ocarina of Time on an RTX 3050, frame times
    // grew to over a second after an area change with it on, and recovered with it off.
    // Switch 2 titles only: RefreshContents learns which stale pages the buffer cache wrote on the
    // GPU from Nx2MayBeGpuWritten, which only they feed.
    static Common::Nx2::SessionValue<bool> enabled{[] {
        return Common::Nx2::Setting("EDEN_NX2_TC_SKIP_STALE", "0") == "1";
    }};
    return Tegra::g_nx2_ampere.load(std::memory_order_relaxed) && enabled.Get();
}

template <class P>
void TextureCache<P>::UpdateStaleTracking(u64 run_begin, u64 run_pages, s32 delta) {
    if (run_pages != 0) {
        device_memory.UpdatePagesCachedCount(run_begin << STALE_PAGE_BITS,
                                             run_pages << STALE_PAGE_BITS, delta);
    }
}

template <class P>
void TextureCache<P>::MarkStale(const ImageBase& image) {
    // A CPU write after the GPU's leaves the image GpuModified and CpuModified, and its pages
    // hold the CPU's data, which a new image there must upload.
    if (!SkipStaleUploads() || False(image.flags & ImageFlagBits::GpuModified) ||
        True(image.flags &
             (ImageFlagBits::CpuModified | ImageFlagBits::Sparse | ImageFlagBits::Unbacked)) ||
        image.guest_size_bytes == 0) {
        return;
    }
    // Whole pages only: a page the image shares with other data keeps that data's state.
    constexpr u64 page_size = u64{1} << STALE_PAGE_BITS;
    const u64 first = (image.cpu_addr + page_size - 1) >> STALE_PAGE_BITS;
    const u64 end = (image.cpu_addr + image.guest_size_bytes) >> STALE_PAGE_BITS;
    if (end <= first || end > (u64{1} << 28)) {
        return;
    }
    if (stale_bits.size() * 64 < end) {
        stale_bits.resize((end + 63) / 64);
    }
    // Tracked from here on so CPU writes reach WriteMemory. Marking before the image's own
    // tracking goes keeps the pages' write protection from switching off and on again.
    u64 run_begin = 0;
    u64 run_pages = 0;
    for (u64 page = first; page < end; ++page) {
        u64& word = stale_bits[page / 64];
        const u64 bit = u64{1} << (page % 64);
        if ((word & bit) != 0) {
            UpdateStaleTracking(run_begin, run_pages, 1);
            run_pages = 0;
            continue;
        }
        word |= bit;
        ++stale_page_count;
        if (run_pages == 0) {
            run_begin = page;
        }
        ++run_pages;
    }
    UpdateStaleTracking(run_begin, run_pages, 1);
}

template <class P>
void TextureCache<P>::ClearStale(DAddr addr, size_t size) {
    if (stale_page_count == 0 || size == 0) {
        return;
    }
    constexpr u64 page_size = u64{1} << STALE_PAGE_BITS;
    const u64 first = addr >> STALE_PAGE_BITS;
    const u64 end = (std::min<u64>)((addr + size + page_size - 1) >> STALE_PAGE_BITS,
                                    stale_bits.size() * 64);
    u64 run_begin = 0;
    u64 run_pages = 0;
    for (u64 page = first; page < end;) {
        u64& word = stale_bits[page / 64];
        if (word == 0 && page % 64 == 0) {
            UpdateStaleTracking(run_begin, run_pages, -1);
            run_pages = 0;
            page += 64;
            continue;
        }
        const u64 bit = u64{1} << (page % 64);
        if ((word & bit) != 0) {
            word &= ~bit;
            --stale_page_count;
            if (run_pages == 0) {
                run_begin = page;
            }
            ++run_pages;
        } else {
            UpdateStaleTracking(run_begin, run_pages, -1);
            run_pages = 0;
        }
        ++page;
    }
    UpdateStaleTracking(run_begin, run_pages, -1);
}

template <class P>
void TextureCache<P>::ClearStaleImage(const ImageBase& image) {
    if (stale_page_count == 0) {
        return;
    }
    if (False(image.flags & ImageFlagBits::Sparse)) {
        ClearStale(image.cpu_addr, image.guest_size_bytes);
        return;
    }
    ClearStaleGpu(image.gpu_addr, image.guest_size_bytes);
}

template <class P>
void TextureCache<P>::ClearStaleGpu(GPUVAddr gpu_addr, size_t size) {
    if (stale_page_count == 0) {
        return;
    }
    // Each piece is contiguous in device memory (GetSubmappedRange splits where it is not).
    for (const auto& [piece_addr, piece_size] : gpu_memory->GetSubmappedRange(gpu_addr, size)) {
        if (const auto device_addr = gpu_memory->GpuToCpuAddress(piece_addr)) {
            ClearStale(*device_addr, piece_size);
        }
    }
}

template <class P>
bool TextureCache<P>::IsAllStale(DAddr addr, size_t size) const {
    if (stale_page_count == 0 || size == 0) {
        return false;
    }
    constexpr u64 page_size = u64{1} << STALE_PAGE_BITS;
    const u64 first = addr >> STALE_PAGE_BITS;
    const u64 end = (addr + size + page_size - 1) >> STALE_PAGE_BITS;
    if (end > stale_bits.size() * 64) {
        return false;
    }
    for (u64 page = first; page < end;) {
        const u64 word = stale_bits[page / 64];
        if (page % 64 == 0 && page + 64 <= end) {
            if (word != ~u64{0}) {
                return false;
            }
            page += 64;
            continue;
        }
        if (((word >> (page % 64)) & 1) == 0) {
            return false;
        }
        ++page;
    }
    return true;
}

template <class P>
void TextureCache<P>::ApplyThresholdOverrides(bool log) {
    // Past what a frame needs, more device memory only keeps the textures of earlier scenes
    // (title 1 filled 6.7 GB of an 8 GB card and draws its races in ~5 GB), so collect from
    // MaxCriticalMemory on rather than near the card's budget. Larger cards get 5/8 of the
    // driver's budget (GetDeviceLocalMemory is capped near 7 GiB): at 5 GiB title 1's races
    // recreate and upload ~8 MiB of render targets a frame (16 GB card: 53 -> 59 fps at 10 GiB).
    u64 max_critical = MaxCriticalMemory;
    if constexpr (requires { runtime.GetDeviceMemoryBudget(); }) {
        max_critical = (std::max)(max_critical, runtime.GetDeviceMemoryBudget() / 8 * 5);
    }
    // What the device budget allows (budget minus margins), before that cap.
    const u64 budget_critical = critical_memory;
    const u64 budget_expected = expected_memory;
    critical_memory = (std::min)(critical_memory, max_critical);
    expected_memory =
        (std::min)(expected_memory, critical_memory - (std::min)(critical_memory, u64{768_MiB}));
    minimum_memory =
        (std::min)(minimum_memory, expected_memory - (std::min)(expected_memory, u64{1_GiB}));
    // EDEN_NX2_TC_EXPECTED_MIB / EDEN_NX2_TC_CRITICAL_MIB raise the collector's thresholds past
    // that cap, but never past what the device budget allows: set outright, they kept the cache
    // above a shrinking budget (other programs' video memory, out-of-memory margins) and the
    // driver paged (9 fps on an 8 GB card in Ocarina's Kokiri Forest).
    for (const auto& [name, target, limit] :
         {std::tuple{"EDEN_NX2_TC_EXPECTED_MIB", &expected_memory, budget_expected},
          std::tuple{"EDEN_NX2_TC_CRITICAL_MIB", &critical_memory, budget_critical}}) {
        if (const char* value = std::getenv(name)) {
            *target = (std::min)(static_cast<u64>(std::strtoull(value, nullptr, 10)) << 20, limit);
        }
    }
    expected_memory = (std::min)(expected_memory, critical_memory);
    // EDEN_NX2_TC_MINIMUM_MIB: below this the collector does not run at all. By default it deletes
    // every image unused for a few seconds past a low minimum, so revisited textures are created,
    // uploaded and unswizzled again (hitches).
    if (const char* value = std::getenv("EDEN_NX2_TC_MINIMUM_MIB")) {
        minimum_memory =
            (std::min)(static_cast<u64>(std::strtoull(value, nullptr, 10)) << 20, expected_memory);
    }
    if (log) {
        LogThresholds();
    }
}

template <class P>
void TextureCache<P>::LogThresholds() const {
    LOG_INFO(HW_GPU, "Texture cache thresholds: expected {} MiB, critical {} MiB, minimum {} MiB "
                     "(VMA slack offset {} MiB)",
             expected_memory >> 20, critical_memory >> 20, minimum_memory >> 20,
             slack_offset >> 20);
}

template <class P>
void TextureCache<P>::UpdateSlackOffset(u64 budget, u64 margin) {
    if constexpr (requires { runtime.GetDeviceMemorySlack(); }) {
        // The driver counts the space in VMA's blocks no image uses, the usage figure does not:
        // with ~1 GB of it on a 6 GB card (title 1) the process sat 300-750 MiB over the budget,
        // and Windows paged it out (100-760 ms GPU stalls). Over the critical line by the
        // driver's figure, the thresholds go down by the excess. Evicting from blocks other
        // images still use frees nothing (it adds holes), so they go down again only once the
        // usage has moved since: blocks were freed, or new ones made. EDEN_NX2_TC_SLACK=1, the
        // default only for Ocarina of Time: on a 6 GB card title 1's lower thresholds evicted
        // 2-4x more costly images and frames were slower than with the plain thresholds, while
        // Ocarina on a 6 GB RTX 3050 sat ~1 GB over the budget (presents held up to 1.8 s).
        static Common::Nx2::SessionValue<bool> enabled{[] {
            return Common::Nx2::Setting("EDEN_NX2_TC_SLACK", "0") == "1";
        }};
        // EDEN_NX2_TC_BUDGET_GUARD_MIB=<MiB> (0 = off, the default; 256 for Mario Kart World):
        // without EDEN_NX2_TC_SLACK, the same offset, but only within that much of the budget
        // rather than within the margin. Title 1 on a 6 GB card holds its images at the critical
        // line while the slack grows to ~570 MiB, which leaves the driver's usage ~150 MiB under
        // the budget.
        static Common::Nx2::SessionValue<u32> guard_mib{[] {
            const std::string_view text = Common::Nx2::Setting("EDEN_NX2_TC_BUDGET_GUARD_MIB", "0");
            u32 result = 0;
            std::from_chars(text.data(), text.data() + text.size(), result);
            return result;
        }};
        const u64 guard = u64{guard_mib.Get()} << 20;
        if (!enabled.Get() && guard == 0) {
            slack_offset = 0;
            return;
        }
        const u64 headroom = enabled.Get() ? margin : (std::min)(guard, margin);
        const u64 slack = runtime.GetDeviceMemorySlack();
        const u64 usage = runtime.GetDeviceMemoryUsage() + slack;
        const u64 line = budget - headroom;
        if (usage > line) {
            // Measured from the last step, so a slow climb still counts.
            const u64 moved = usage > slack_check_usage ? usage - slack_check_usage
                                                        : slack_check_usage - usage;
            if (slack_offset == 0 || moved >= headroom / 8) {
                slack_offset += (std::min)(usage - line, headroom / 2);
                slack_check_usage = usage;
            }
        } else {
            if (usage + headroom / 2 < line) {
                slack_offset -= (std::min)(slack_offset, headroom / 4);
            }
            slack_check_usage = usage;
        }
        // Past the slack the blocks in use fit under the line; and keep half the budget.
        slack_offset = (std::min)({slack_offset, slack, budget / 2 - margin});
    }
}

template <class P>
void TextureCache<P>::TickFrame() {
    // Upload buffers grown for one huge image (title 1's 341 MiB terrain layer pools) are freed
    // rather than kept for the rest of the session.
    for (auto* const scratch : {&swizzle_data_buffer, &unswizzle_data_buffer}) {
        if (scratch->capacity() > LargeScratchCapacity) {
            scratch->release();
        }
    }
    if constexpr (HAS_DEVICE_MEMORY_INFO) {
        // Switch 2 titles (native tiling) stream far more texture data than Eden's thresholds
        // were tuned for: title 1's attract loop alone keeps ~5.3 GB resident, so the collector
        // sat in aggressive mode evicting decoded ASTC (and decoding it again). Total use is
        // measured from the driver, so collect only near the device budget instead.
        if (!native_thresholds_applied && gpu_memory != nullptr && gpu_memory->UsesNativeTiling()) {
            native_thresholds_applied = true;
            const u64 device_local = runtime.GetDeviceLocalMemory();
            critical_memory = (std::max)(critical_memory, device_local - 256_MiB);
            expected_memory = (std::max)(expected_memory, device_local - 1280_MiB);
            ApplyThresholdOverrides();
            native_critical_memory = critical_memory;
            native_expected_memory = expected_memory;
        }
        // Follow the driver's budget (re-read now and then): it shrinks when other programs use
        // VRAM (another game: 7.0 -> 6.4 GB), and collecting only past it means the driver pages
        // textures out. It is also the room to use: Eden's device memory figure is capped near
        // 7 GB, which on a 12 GB card (11.2 GB budget) kept title 1 evicting textures with 3 GB
        // unused. Both thresholds stay under it with a margin (1/8 of it, at most 1 GiB): past it
        // allocations fail and images end up unbacked (garbage textures), and Windows shrinks the
        // budget as the card fills (16 GB card: 13.5 GB left mid-race), hence the re-read every 30
        // frames. EDEN_NX2_TC_*_MIB overrides can raise the thresholds up to this budget.
        if constexpr (requires { runtime.GetDeviceMemoryBudget(); }) {
            if (native_thresholds_applied && budget_check_frames++ % 30 == 0) {
                const u64 budget = runtime.GetDeviceMemoryBudget();
                if (budget > 2_GiB) {
                    const u64 old_critical = critical_memory;
                    const u64 margin = (std::min)(budget / 8, 1_GiB);
                    UpdateSlackOffset(budget, margin);
                    critical_memory = budget - margin - slack_offset;
                    expected_memory = critical_memory - margin;
                    ApplyThresholdOverrides(false);
                    // Log the first values and real changes (the budget moves by a few MiB all
                    // the time). Compared once clamped: the budget's figure is over the cap on
                    // most cards, which logged the same thresholds every 30 frames.
                    const u64 change = critical_memory > old_critical
                                           ? critical_memory - old_critical
                                           : old_critical - critical_memory;
                    if (budget_check_frames == 1 || change >= 256_MiB) {
                        LogThresholds();
                    }
                }
            }
        }
    }
    if constexpr (requires { runtime.CurrentTick(); }) {
        while (!gpu_sentenced_images.empty() &&
               runtime.IsFree(gpu_sentenced_images.front().tick)) {
            gpu_sentenced_bytes -= gpu_sentenced_images.front().bytes;
            if constexpr (requires(Image& image) { image.Recycle(); }) {
                gpu_sentenced_images.front().image.Recycle();
            }
            gpu_sentenced_images.pop_front();
        }
        CompleteCollectorDownloads();
    }
    // If we can obtain the memory info, use it instead of the estimate.
    if (runtime.CanReportMemoryUsage()) {
        total_used_memory = runtime.GetDeviceMemoryUsage();
        // Images the collector deleted keep their memory for TICKS_TO_DESTROY frames: counting
        // it kept the collector in aggressive mode, deleting 40+ images a frame for frames after
        // it had freed enough (title 1: frames over 100 ms for seconds at a time).
        for (const u64 bytes : sentenced_image_bytes) {
            total_used_memory -= (std::min)(total_used_memory, bytes);
        }
        total_used_memory -= (std::min)(total_used_memory, gpu_sentenced_bytes);
        total_used_memory -= (std::min)(total_used_memory, collector_download_bytes);
    }
    // Every 600 frames: device memory in use against the collector's thresholds, and what the
    // images themselves account for (VRAM pressure makes the collector thrash).
    if (static u64 frames{}; ++frames % 600 == 0) {
        u64 image_bytes{};
        u64 costly_bytes{};
        u64 gpu_written_bytes{};
        size_t images{};
        std::vector<std::pair<u64, const Image*>> largest;
        std::map<u32, std::pair<u64, u32>> by_format;
        for (const auto [image_id, image] : slot_images) {
            const u64 bytes = ImageMemoryBytes(*image) +
                              (image->HasScaled() ? GetScaledImageSizeBytes(*image) : 0);
            image_bytes += bytes;
            costly_bytes += True(image->flags & ImageFlagBits::CostlyLoad) ? bytes : 0;
            gpu_written_bytes += True(image->flags & ImageFlagBits::GpuModified) ? bytes : 0;
            ++images;
            largest.emplace_back(bytes, image);
            auto& format = by_format[static_cast<u32>(image->info.format)];
            format.first += bytes;
            ++format.second;
        }
        u64 slack{};
        if constexpr (requires { runtime.GetDeviceMemorySlack(); }) {
            slack = runtime.GetDeviceMemorySlack();
        }
        LOG_INFO(HW_GPU, "Texture cache: device memory {} MiB + {} MiB VMA slack (expected {} MiB, critical {} MiB, slack offset {} MiB), {} images {} MiB",
                 total_used_memory >> 20, slack >> 20, expected_memory >> 20,
                 critical_memory >> 20, slack_offset >> 20, images, image_bytes >> 20);
        // A working set over the critical memory leaves the collector only images the next
        // frames load again (Mario Kart World's later races on a 6 GB card).
        u64 idle_bytes{};
        if (frame_tick >= 60) {
            lru_cache.ForEachItemBelow(frame_tick - 60, [&](ImageId image_id) {
                const Image& image = slot_images[image_id];
                idle_bytes += ImageMemoryBytes(image) +
                              (image.HasScaled() ? GetScaledImageSizeBytes(image) : 0);
            });
        }
        LOG_INFO(HW_GPU,
                 "Texture cache images: {} MiB used in the last 60 frames, {} MiB not; {} MiB "
                 "costly to load again, {} MiB written by the GPU",
                 (image_bytes - (std::min)(image_bytes, idle_bytes)) >> 20, idle_bytes >> 20,
                 costly_bytes >> 20, gpu_written_bytes >> 20);
        std::partial_sort(largest.begin(), largest.begin() + std::min<size_t>(12, largest.size()),
                          largest.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < std::min<size_t>(12, largest.size()); ++i) {
            const Image& image = *largest[i].second;
            LOG_DEBUG(HW_GPU, "  {} MiB: {:#x} {}x{}x{} layers {} levels {} format {} type {} flags {:#x}",
                     largest[i].first >> 20, image.gpu_addr, image.info.size.width, image.info.size.height,
                     image.info.size.depth, image.info.resources.layers, image.info.resources.levels,
                     static_cast<u32>(image.info.format), static_cast<u32>(image.info.type),
                     static_cast<u32>(image.flags));
        }
        std::vector<std::pair<u64, u32>> formats;
        for (const auto& [format, total] : by_format) formats.emplace_back(total.first, format);
        std::sort(formats.rbegin(), formats.rend());
        for (size_t i = 0; i < std::min<size_t>(8, formats.size()); ++i) {
            LOG_DEBUG(HW_GPU, "  format {}: {} MiB in {} images", formats[i].second, formats[i].first >> 20,
                     by_format[formats[i].second].second);
        }
        if constexpr (requires { runtime.LogMemoryStatistics(); }) {
            runtime.LogMemoryStatistics();
        }
        if constexpr (requires { runtime.LogImagePool(); }) {
            runtime.LogImagePool();
        }
        if constexpr (requires(const Image& image) { image.MemoryBlock(); }) {
            // The device memory blocks the cache's images share: a block goes back to the driver
            // only once none is left in it (EDEN_NX2_TC_COMPACT empties the sparse ones).
            std::unordered_map<u64, std::pair<u64, u64>> blocks;
            for (const auto [image_id, image] : slot_images) {
                const auto block = image->MemoryBlock();
                if (block.block != 0) {
                    auto& [block_size, bytes] = blocks[block.block];
                    block_size = block.block_size;
                    bytes += block.size;
                }
            }
            u64 block_bytes{};
            u64 held_bytes{};
            u64 sparse_blocks{};
            u64 sparse_block_bytes{};
            u64 sparse_held_bytes{};
            for (const auto& [block, entry] : blocks) {
                block_bytes += entry.first;
                held_bytes += entry.second;
                if (entry.second * 2 <= entry.first) {
                    ++sparse_blocks;
                    sparse_block_bytes += entry.first;
                    sparse_held_bytes += entry.second;
                }
            }
            LOG_INFO(HW_GPU,
                     "Texture cache image blocks: {} blocks {} MiB hold {} MiB of images; {} under "
                     "half full ({} MiB of blocks, {} MiB of images). Compaction last 600 frames: "
                     "{} images moved ({} MiB), {} blocks emptied of them, {} failed",
                     blocks.size(), block_bytes >> 20, held_bytes >> 20, sparse_blocks,
                     sparse_block_bytes >> 20, sparse_held_bytes >> 20, compact_moved_images,
                     compact_moved_bytes >> 20, compact_emptied_blocks, compact_failures);
            compact_moved_images = compact_moved_bytes = 0;
            compact_emptied_blocks = compact_failures = 0;
        }
        LOG_INFO(HW_GPU, "Texture cache last 600 frames: created {} images, deleted {} (collector {}, {} costly, {} downloaded), {} async decodes, ASTC {} opaque / {} alpha",
                 images_created, images_deleted, gc_deletes + gc_costly_deletes, gc_costly_deletes,
                 gc_download_deletes,
                 async_decodes_queued, astc_opaque_images, astc_alpha_images);
        LOG_INFO(HW_GPU, "Texture cache last 600 frames: {} uploads from guest memory, {} MiB (largest {} MiB, {} by layer), {} without memory cleared ({} MiB)",
                 images_uploaded, uploaded_bytes >> 20, largest_upload >> 20, layer_uploads,
                 tc_zero_clears, tc_zero_clear_bytes >> 20);
        tc_zero_clears = tc_zero_clear_bytes = 0;
        if (SkipStaleUploads()) {
            LOG_INFO(HW_GPU,
                     "Texture cache last 600 frames: stale uploads skipped {} ({} MiB), {} "
                     "uploaded for buffer-written pages; {} MiB of pages marked stale",
                     tc_stale_skips, tc_stale_skip_bytes >> 20, tc_stale_gpu_written,
                     (stale_page_count << STALE_PAGE_BITS) >> 20);
        }
        tc_stale_skips = tc_stale_skip_bytes = tc_stale_gpu_written = 0;
        LOG_INFO(HW_GPU,
                 "Texture cache last 600 frames: {} new render targets ({} uploaded, {} started "
                 "empty by EDEN_NX2_RT_FRESH)",
                 render_targets_created, render_targets_uploaded, fresh_render_targets);
        render_targets_created = render_targets_uploaded = fresh_render_targets = 0;
        images_uploaded = uploaded_bytes = largest_upload = layer_uploads = 0;
        astc_opaque_images = astc_alpha_images = 0;
        std::vector<std::pair<u32, u64>> shapes;
        for (const auto& [key, value] : created_shapes) shapes.emplace_back(value.first, key);
        std::sort(shapes.rbegin(), shapes.rend());
        for (size_t i = 0; i < std::min<size_t>(10, shapes.size()); ++i) {
            const u64 key = shapes[i].second;
            LOG_DEBUG(HW_GPU, "  created {}x: format {} type {} levels {} layers {} {}x{} (last at {:#x})",
                     shapes[i].first, key >> 56, (key >> 52) & 0xF, (key >> 48) & 0xF,
                     (key >> 36) & 0xFFF, (key >> 18) & 0x3FFFF, key & 0x3FFFF,
                     created_shapes[key].second);
        }
        created_shapes.clear();
        gc_deletes = gc_costly_deletes = gc_download_deletes = async_decodes_queued = 0;
        images_created = images_deleted = 0;
    }
    // An image that could not be made at all: the driver is out of memory whatever the counts
    // say, so collect as at the critical threshold.
    bool out_of_memory = false;
    if constexpr (requires { runtime.FailedImages(); }) {
        const u32 failed = runtime.FailedImages();
        out_of_memory = failed != seen_failed_images;
        seen_failed_images = failed;
    }
    if constexpr (requires { runtime.TrimImagePool(u64{}); }) {
        // Pooled images (EDEN_NX2_TC_IMAGE_POOL) go before the collector evicts images in use to
        // get under the critical line, and all of them once an image could not be made.
        if (out_of_memory || total_used_memory >= critical_memory) {
            const u64 excess = out_of_memory ? ~0ULL : total_used_memory - critical_memory + 1;
            total_used_memory -= (std::min)(total_used_memory, runtime.TrimImagePool(excess));
        }
    }
    if (out_of_memory || total_used_memory > minimum_memory) {
        RunGarbageCollector(out_of_memory);
    }
    if (out_of_memory) {
        // Textures left out for lack of memory are made again on their next read, once a frame
        // while images keep failing.
        for (size_t c : active_channel_ids) {
            channel_storage[c].graphics_image_table.Invalidate();
            channel_storage[c].compute_image_table.Invalidate();
        }
    } else {
        CompactImageBlocks();
    }
    sentenced_images.Tick();
    sentenced_image_index = (sentenced_image_index + 1) % TICKS_TO_DESTROY;
    sentenced_image_bytes[sentenced_image_index] = 0;
    sentenced_framebuffers.Tick();
    sentenced_image_view.Tick();
    TickAsyncDecode();
    TickAsyncUnswizzle();
    if (frame_tick % 600 == 0 && !uploaded_ranges.empty()) {
        for (const auto& [address, size] : uploaded_ranges) {
            device_memory.HintCold(address, size);
        }
        uploaded_ranges.clear();
    }

    runtime.TickFrame();
    ++frame_tick;

    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        for (auto& buffer : async_buffers_death_ring) {
            runtime.FreeDeferredStagingBuffer(buffer);
        }
        async_buffers_death_ring.clear();
    }
}

template <class P>
const typename P::ImageView& TextureCache<P>::GetImageView(ImageViewId id) const noexcept {
    return slot_image_views[id];
}

template <class P>
typename P::ImageView& TextureCache<P>::GetImageView(ImageViewId id) noexcept {
    return slot_image_views[id];
}

template <class P>
typename P::ImageView& TextureCache<P>::GetImageView(u32 index) noexcept {
    // Not compute!
    const auto image_view_id = VisitImageView(index, false);
    return slot_image_views[image_view_id];
}

template <class P>
void TextureCache<P>::MarkModification(ImageId id) noexcept {
    MarkModification(slot_images[id]);
}

template <class P>
bool TextureCache<P>::PublishComputeCollisionImage(u32 index) {
    const ImageViewId view_id = VisitImageView(index, true);
    if (!view_id || !slot_image_views[view_id].image_id) {
        return false;
    }
    const ImageId image_id = slot_image_views[view_id].image_id;
    const Image& image = slot_images[image_id];
    const ImageInfo& info = image.info;
    // A CPU-readable 256x256 pitch-linear SNORM height map.
    if (info.type != ImageType::Linear || info.format != PixelFormat::R16G16B16A16_SNORM ||
        info.size.width != 256 || info.size.height != 256 || info.size.depth != 1 ||
        info.pitch != 2048 || info.resources.layers != 1 || info.resources.levels != 1 ||
        !IsDownloadable(image) ||
        !gpu_memory->IsFullyMappedRange(image.gpu_addr, image.guest_size_bytes)) {
        return false;
    }
    // Downloaded with the next fence's flushes and written back once the GPU reaches that fence,
    // as a preemptively downloaded render target is, instead of waiting for the GPU here.
    uncommitted_downloads.emplace_back(PendingDownload{true, 0, image_id});
    return true;
}

template <class P>
void TextureCache<P>::FillImageViews(std::span<ImageViewInOut> views, bool compute, bool blacklist,
                                     bool recheck_feedback) {
    // Buffer views have no image: scaling down the null image in their place is skipped.
    // EDEN_NX2_MESH_PUSH_LEGACY=1 (the switch of the change this came with) does it again.
    static const bool scale_null_image = [] {
        const char* value = std::getenv("EDEN_NX2_MESH_PUSH_LEGACY");
        return value != nullptr && value[0] == '1';
    }();
    feedback_sampling = !compute;
    feedback_recheck = !compute && recheck_feedback;
    feedback_rechecks.clear();
    SCOPE_EXIT {
        feedback_sampling = false;
        feedback_recheck = false;
    };
    bool has_blacklisted = false;
    do {
        has_deleted_images = false;
        if (blacklist) {
            has_blacklisted = false;
        }
        for (ImageViewInOut& view : views) {
            view.id = VisitImageView(view.index, compute);
            if (blacklist) {
                if (view.blacklist && view.id != NULL_IMAGE_VIEW_ID) {
                    const ImageViewBase& image_view = slot_image_views[view.id];
                    if (image_view.image_id == NULL_IMAGE_ID && !scale_null_image) {
                        continue;
                    }
                    auto& image = slot_images[image_view.image_id];
                    has_blacklisted |= ScaleDown(image);
                    image.scale_rating = 0;
                }
            }
        }
    } while (has_deleted_images || (blacklist && has_blacklisted));
}

template <class P>
bool TextureCache<P>::SamplesRenderTarget(std::span<const ImageViewInOut> views) {
    if (rt_active_mask == 0) {
        return false;
    }
    const bool depth_active = (rt_active_mask & (1u << NUM_RT)) != 0;
    // An image the cache keeps apart from a render target over the same guest memory (another
    // format, size or view of it) still reads what the open render pass wrote there.
    const auto overlaps = [this](const ImageBase& sampled, ImageId rt_id) {
        if (!rt_id) {
            return false;
        }
        const ImageBase& rt = slot_images[rt_id];
        return sampled.gpu_addr < rt.gpu_addr + rt.guest_size_bytes &&
               rt.gpu_addr < sampled.gpu_addr + sampled.guest_size_bytes;
    };
    for (const auto& view : views) {
        if (!view.id) {
            continue;
        }
        const ImageId image_id = slot_image_views[view.id].image_id;
        if (!image_id) {
            continue;
        }
        if (depth_active && image_id == rt_depth_image_id) {
            return true;
        }
        for (size_t i = 0; i < NUM_RT; ++i) {
            if ((rt_active_mask & (1u << i)) != 0 && image_id == rt_image_id[i]) {
                return true;
            }
        }
        const ImageBase& sampled = slot_images[image_id];
        if (depth_active && overlaps(sampled, rt_depth_image_id)) {
            return true;
        }
        for (size_t i = 0; i < NUM_RT; ++i) {
            if ((rt_active_mask & (1u << i)) != 0 && overlaps(sampled, rt_image_id[i])) {
                return true;
            }
        }
    }
    return false;
}

template <class P>
bool TextureCache<P>::SampledViewsChange(std::span<const ImageViewInOut> views) const noexcept {
    return std::ranges::any_of(views, [this](const ImageViewInOut& view) {
        // As in PrepareImageView: the null view and buffer views prepare no image.
        if (!view.id || view.id == NULL_IMAGE_VIEW_ID) {
            return false;
        }
        const ImageViewBase& image_view = slot_image_views[view.id];
        if (image_view.IsBuffer()) {
            return false;
        }
        const ImageBase& image = slot_images[image_view.image_id];
        return True(image.flags & ImageFlagBits::CpuModified) ||
               (image.alias_maybe_newer && !image.aliased_images.empty());
    });
}

template <class P>
void TextureCache<P>::CheckFeedbackLoop(std::span<const ImageViewInOut> views) {
    if (!Settings::values.barrier_feedback_loops.GetValue()) {
        return;
    }

    if (render_targets_serial == last_feedback_loop_serial &&
        texture_bindings_serial == last_feedback_texture_serial) {
        if (last_feedback_loop_result) {
            runtime.BarrierFeedbackLoop();
        }
        return;
    }

    if (rt_active_mask == 0) {
        last_feedback_loop_serial = render_targets_serial;
        last_feedback_texture_serial = texture_bindings_serial;
        last_feedback_loop_result = false;
        return;
    }
    const u32 depth_bit = 1u << NUM_RT;
    const bool depth_active = (rt_active_mask & depth_bit) != 0;

    const bool requires_barrier = [&] {
        for (const auto& view : views) {
            if (!view.id) {
                continue;
            }

            {
                bool is_continue = false;
                for (size_t i = 0; i < 8; ++i)
                    is_continue |= (rt_active_mask & (1u << i)) && view.id == render_targets.color_buffer_ids[i];
                if (is_continue)
                    continue;
            }

            if (depth_active && view.id == render_targets.depth_buffer_id)
                continue;

            const ImageId view_image_id = slot_image_views[view.id].image_id;
            {
                bool is_continue = false;
                for (size_t i = 0; i < 8; ++i)
                    is_continue |= (rt_active_mask & (1u << i)) && view_image_id == rt_image_id[i];
                if (is_continue)
                    continue;
            }
            if (depth_active && view_image_id == rt_depth_image_id) {
                return true;
            }
        }

        return false;
    }();

    last_feedback_loop_serial = render_targets_serial;
    last_feedback_texture_serial = texture_bindings_serial;
    last_feedback_loop_result = requires_barrier;
    if (requires_barrier) {
        runtime.BarrierFeedbackLoop();
    }
}

template <class P>
typename P::Sampler* TextureCache<P>::GetSampler(u32 index, bool compute) {
    return &slot_samplers[GetSamplerId(index, compute)];
}

template <class P>
SamplerId TextureCache<P>::GetSamplerId(u32 index, bool compute) {
    auto& table = compute ? channel_state->compute_sampler_table : channel_state->graphics_sampler_table;
    if (index > table.current_limit) {
        LOG_TRACE(HW_GPU, "Invalid sampler index={}", index);
        return NULL_SAMPLER_ID;
    }
    auto const [descriptor, is_new] = table.Read(*gpu_memory, index);
    if (is_new) {
        auto const id = FindSampler(descriptor, compute);
        table.bound_ids[index] = id;
        return id;
    }
    return table.bound_ids[index];
}

template <class P>
const typename P::Sampler& TextureCache<P>::GetSampler(SamplerId id) const noexcept {
    return slot_samplers[id];
}

template <class P>
typename P::Sampler& TextureCache<P>::GetSampler(SamplerId id) noexcept {
    return slot_samplers[id];
}

template <class P>
void TextureCache<P>::SynchronizeDescriptors(bool compute) {
    if (compute) {
        const bool linked_tsc = kepler_compute->launch_description.linked_tsc;
        const u32 tic_limit = kepler_compute->regs.tic.limit;
        const u32 tsc_limit = linked_tsc ? tic_limit : kepler_compute->regs.tsc.limit;
        bool bindings_changed = false;
        if (channel_state->compute_sampler_table.Synchronize(kepler_compute->regs.tsc.Address(), tsc_limit))
            bindings_changed = true;
        if (channel_state->compute_image_table.Synchronize(kepler_compute->regs.tic.Address(), tic_limit))
            bindings_changed = true;
        if (bindings_changed) {
            ++texture_bindings_serial;
        }
    } else {
        const bool linked_tsc = maxwell3d->regs.sampler_binding == Tegra::Engines::Maxwell3D::Regs::SamplerBinding::ViaHeaderBinding;
        const u32 tic_limit = maxwell3d->regs.tex_header.limit;
        const u32 tsc_limit = linked_tsc ? tic_limit : maxwell3d->regs.tex_sampler.limit;
        bool bindings_changed = false;
        if (channel_state->graphics_sampler_table.Synchronize(maxwell3d->regs.tex_sampler.Address(), tsc_limit))
            bindings_changed = true;
        if (channel_state->graphics_image_table.Synchronize(maxwell3d->regs.tex_header.Address(), tic_limit))
            bindings_changed = true;
        if (bindings_changed) {
            ++texture_bindings_serial;
        }
    }
}

template <class P>
bool TextureCache<P>::RescaleRenderTargets() {
    auto& flags = maxwell3d->dirty.flags;
    u32 scale_rating = 0;
    bool rescaled = false;
    std::array<ImageId, NUM_RT> tmp_color_images{};
    ImageId tmp_depth_image{};
    do {
        flags[Dirty::RenderTargets] = false;

        has_deleted_images = false;
        // Render target control is used on all render targets, so force look ups when this one is
        // up
        const bool force = flags[Dirty::RenderTargetControl];
        flags[Dirty::RenderTargetControl] = false;

        scale_rating = 0;
        bool any_rescaled = false;
        bool can_rescale = true;
        const auto check_rescale = [&](ImageViewId view_id, ImageId& id_save) {
            if (view_id != NULL_IMAGE_VIEW_ID && view_id != ImageViewId{}) {
                const auto& view = slot_image_views[view_id];
                const auto image_id = view.image_id;
                id_save = image_id;
                auto& image = slot_images[image_id];
                can_rescale &= ImageCanRescale(image);
                any_rescaled |= True(image.flags & ImageFlagBits::Rescaled) ||
                                GetFormatType(image.info.format) != SurfaceType::ColorTexture;
                scale_rating = std::max<u32>(scale_rating, image.scale_tick <= frame_tick
                                                               ? image.scale_rating + 1U
                                                               : image.scale_rating);
            } else {
                id_save = CORRUPT_ID;
            }
        };
        for (size_t index = 0; index < NUM_RT; ++index) {
            ImageViewId& color_buffer_id = render_targets.color_buffer_ids[index];
            if (flags[Dirty::ColorBuffer0 + index] || force) {
                flags[Dirty::ColorBuffer0 + index] = false;
                BindRenderTarget(&color_buffer_id, FindColorBuffer(index));
            }
            check_rescale(color_buffer_id, tmp_color_images[index]);
        }
        if (flags[Dirty::ZetaBuffer] || force) {
            flags[Dirty::ZetaBuffer] = false;
            BindRenderTarget(&render_targets.depth_buffer_id, FindDepthBuffer());
        }
        check_rescale(render_targets.depth_buffer_id, tmp_depth_image);

        if (can_rescale) {
            rescaled = any_rescaled || scale_rating >= 2;
            const auto scale_up = [this](ImageId image_id) {
                if (image_id != CORRUPT_ID) {
                    Image& image = slot_images[image_id];
                    ScaleUp(image);
                }
            };
            if (rescaled) {
                for (size_t index = 0; index < NUM_RT; ++index) {
                    scale_up(tmp_color_images[index]);
                }
                scale_up(tmp_depth_image);
                scale_rating = 2;
            }
        } else {
            rescaled = false;
            const auto scale_down = [this](ImageId image_id) {
                if (image_id != CORRUPT_ID) {
                    Image& image = slot_images[image_id];
                    ScaleDown(image);
                }
            };
            for (size_t index = 0; index < NUM_RT; ++index) {
                scale_down(tmp_color_images[index]);
            }
            scale_down(tmp_depth_image);
            scale_rating = 1;
        }
    } while (has_deleted_images);
    const auto set_rating = [this, scale_rating](ImageId image_id) {
        if (image_id != CORRUPT_ID) {
            Image& image = slot_images[image_id];
            image.scale_rating = scale_rating;
            if (image.scale_tick <= frame_tick) {
                image.scale_tick = frame_tick + 1;
            }
        }
    };
    for (size_t index = 0; index < NUM_RT; ++index) {
        set_rating(tmp_color_images[index]);
    }
    set_rating(tmp_depth_image);

    return rescaled;
}

template <class P>
void TextureCache<P>::UpdateRenderTargets(bool is_clear) {
    using namespace VideoCommon::Dirty;
    auto& flags = maxwell3d->dirty.flags;
    if (!flags[Dirty::RenderTargets]) {
        marking_render_targets = !is_clear;
        for (size_t index = 0; index < NUM_RT; ++index) {
            ImageViewId& color_buffer_id = render_targets.color_buffer_ids[index];
            PrepareImageView(color_buffer_id, true, is_clear && IsFullClear(color_buffer_id, index));
        }
        const ImageViewId depth_buffer_id = render_targets.depth_buffer_id;
        PrepareImageView(depth_buffer_id, true, is_clear && IsFullClear(depth_buffer_id, std::nullopt));
        marking_render_targets = false;
        return;
    }

    const VideoCommon::RenderTargets previous_render_targets = render_targets;
    // New targets created for a clear leave their upload to here (JoinImages), so it can be
    // skipped when the clear overwrites the whole image. EDEN_NX2_CLEAR_SKIP_UPLOAD=0 disables it.
    static const bool skip_cleared_uploads = [] {
        const char* value = std::getenv("EDEN_NX2_CLEAR_SKIP_UPLOAD");
        return value == nullptr || value[0] != '0';
    }();
    clear_deferred_uploads.clear();
    clearing_render_targets = is_clear && skip_cleared_uploads;
    const bool rescaled = RescaleRenderTargets();
    clearing_render_targets = false;
    if (is_rescaling != rescaled) {
        flags[Dirty::RescaleViewports] = true;
        flags[Dirty::RescaleScissors] = true;
        is_rescaling = rescaled;
    }

    marking_render_targets = !is_clear;
    for (size_t index = 0; index < NUM_RT; ++index) {
        ImageViewId& color_buffer_id = render_targets.color_buffer_ids[index];
        PrepareImageView(color_buffer_id, true,
                         is_clear && IsFullClear(color_buffer_id, index) &&
                             ClearOverwritesDeferredUpload(color_buffer_id, false, index));
    }
    const ImageViewId depth_buffer_id = render_targets.depth_buffer_id;

    PrepareImageView(depth_buffer_id, true,
                     is_clear && IsFullClear(depth_buffer_id, std::nullopt) &&
                         ClearOverwritesDeferredUpload(depth_buffer_id, true, 0));
    marking_render_targets = false;
    clear_deferred_uploads.clear();

    rt_active_mask = 0;
    rt_image_id = {};
    for (size_t i = 0; i < rt_image_id.size(); ++i) {
        if (ImageViewId const view = render_targets.color_buffer_ids[i]; view) {
            rt_active_mask |= 1u << i;
            rt_image_id[i] = slot_image_views[view].image_id;
        }
    }
    if (depth_buffer_id) {
        rt_active_mask |= (1u << NUM_RT);
        rt_depth_image_id = slot_image_views[depth_buffer_id].image_id;
    } else {
        rt_depth_image_id = ImageId{};
    }

    for (size_t index = 0; index < NUM_RT; ++index) {
        render_targets.draw_buffers[index] = static_cast<u8>(maxwell3d->regs.rt_control.Map(index));
    }
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (is_rescaling) {
        up_scale = Settings::values.resolution_info.up_scale;
        down_shift = Settings::values.resolution_info.down_shift;
    }
    render_targets.size = Extent2D{
        (maxwell3d->regs.surface_clip.width * up_scale) >> down_shift,
        (maxwell3d->regs.surface_clip.height * up_scale) >> down_shift,
    };
    render_targets.is_rescaled = is_rescaling;

    if (render_targets != previous_render_targets) {
        ++render_targets_serial;
    }

    flags[Dirty::DepthBiasGlobal] = true;
}

template <class P>
typename P::Framebuffer* TextureCache<P>::GetFramebuffer() {
    if (last_framebuffer_id && last_framebuffer_serial == render_targets_serial) {
        return &slot_framebuffers[last_framebuffer_id];
    }
    const FramebufferId framebuffer_id = GetFramebufferId(render_targets);
    last_framebuffer_id = framebuffer_id;
    last_framebuffer_serial = render_targets_serial;
    return &slot_framebuffers[framebuffer_id];
}

template <class P>
ImageViewId TextureCache<P>::VisitImageView(u32 index, bool compute) {
    auto& table = compute ? channel_state->compute_image_table : channel_state->graphics_image_table;
    index = Tegra::Texture::TextureHeaderIndex(index, table.current_limit,
                                               gpu_memory->UsesNativeTiling());
    if (index > table.current_limit) {
        LOG_TRACE(HW_GPU, "Invalid image view index={}", index);
        return NULL_IMAGE_VIEW_ID;
    }
    // Is new (on the tegra engine side)?
    auto const [descriptor, is_new] = table.Read(*gpu_memory, index);
    if (is_new) {
        // A descriptor with an address but format 0 is no texture the game made: likely a table
        // entry read from guest memory while its newest words were still GPU-held.
        if (descriptor.format == Tegra::Texture::TextureFormat{} && descriptor.Address() != 0)
            [[unlikely]] {
            static std::atomic<u32> reported{};
            if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
                const GPUVAddr entry = table.current_gpu_addr + u64{index} * sizeof(TICEntry);
                const auto device = gpu_memory->GpuToCpuAddress(entry);
                LOG_WARNING(HW_GPU,
                            "Format 0 {} descriptor {} at {:#x} (image {:#x}): table page GPU "
                            "written {}, query page {}",
                            compute ? "compute" : "graphics", index, entry, descriptor.Address(),
                            device && Tegra::Nx2IsGpuWritten(*device),
                            device && Tegra::Nx2IsQueryPage(*device));
            }
        }
        if (IsValidEntry(*gpu_memory, descriptor)) {
            // Is new (registered view) on the texture cache side?
            const auto [pair, is_new_tc] = channel_state->image_views.try_emplace(descriptor);
            if (is_new_tc) {
                const auto failed_images = [&]() -> u32 {
                    if constexpr (requires { runtime.FailedImages(); }) {
                        return runtime.FailedImages();
                    } else {
                        return 0;
                    }
                };
                const u32 failed_before = failed_images();
                pair->second = CreateImageView(descriptor);
                if (pair->second == NULL_IMAGE_VIEW_ID && failed_images() != failed_before) {
                    // No memory for its image: TickFrame invalidates the table after collecting,
                    // and the descriptor's next read makes the image again.
                    channel_state->image_views.erase(pair);
                    table.bound_ids[index] = NULL_IMAGE_VIEW_ID;
                    return NULL_IMAGE_VIEW_ID;
                }
                if (pair->second != NULL_IMAGE_VIEW_ID) {
                    image_view_descriptors[pair->second].emplace_back(current_channel_id,
                                                                      descriptor);
                }
            }
            PrepareImageView(pair->second, false, false);
            table.bound_ids[index] = pair->second;
            return pair->second;
        }
        table.bound_ids[index] = NULL_IMAGE_VIEW_ID;
        return NULL_IMAGE_VIEW_ID;
    }
    const ImageViewId id = table.bound_ids[index];
    if (id != NULL_IMAGE_VIEW_ID)
        PrepareImageView(id, false, false);
    return id;
}

template <class P>
FramebufferId TextureCache<P>::GetFramebufferId(const RenderTargets& key) {
    const auto [pair, is_new] = framebuffers.try_emplace(key);
    FramebufferId& framebuffer_id = pair->second;
    if (!is_new) {
        return framebuffer_id;
    }
    std::array<ImageView*, NUM_RT> color_buffers;
    std::ranges::transform(key.color_buffer_ids, color_buffers.begin(), [this](ImageViewId id) {
        return id ? &slot_image_views[id] : nullptr;
    });
    ImageView* const depth_buffer = key.depth_buffer_id ? &slot_image_views[key.depth_buffer_id] : nullptr;
    runtime.FlushDeferredClear();
    framebuffer_id = slot_framebuffers.insert(runtime, color_buffers, depth_buffer, key);
    return framebuffer_id;
}

template <class P>
void TextureCache<P>::WriteMemory(DAddr cpu_addr, size_t size) {
    ClearStale(cpu_addr, size);
    ForEachImageInRegion(cpu_addr, size, [this, cpu_addr, size](ImageId image_id, Image& image) {
        VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::Invalidation);
        if (True(image.flags & ImageFlagBits::UnwrittenSparse)) {
            // Written at last: a backed image replaces it where it is used next.
            image.flags |= ImageFlagBits::CpuModified | ImageFlagBits::Remapped;
            return;
        }
        if (True(image.flags & (ImageFlagBits::SparseTiles | ImageFlagBits::RangeUploads))) {
            MarkRangeWritten(image, cpu_addr, size);
            return;
        }
        if (UploadsByLayer(image)) {
            MarkLayersWritten(image, cpu_addr, size);
            return;
        }
        if (True(image.flags & ImageFlagBits::CpuModified)) {
            return;
        }
        image.flags |= ImageFlagBits::CpuModified;
        if (True(image.flags & ImageFlagBits::Tracked)) {
            UntrackImage(image, image_id);
        }
    });
}

template <class P>
u64 TextureCache<P>::CleanSerial(DAddr cpu_addr, size_t size) {
    bool touched = false;
    ForEachCPUPage(cpu_addr, size, [&](u64 page) {
        if (touched) {
            return;
        }
        const auto it = page_table.find(page);
        touched = it != page_table.end() &&
                  std::ranges::any_of(it->second, [&](ImageMapId map_id) {
                      return slot_map_views[map_id].Overlaps(cpu_addr, size);
                  });
    });
    return touched ? 0 : page_serial.load(std::memory_order_relaxed);
}

template <class P>
bool TextureCache<P>::IsDownloadable(const ImageBase& image) const noexcept {
    if (!image.IsSafeGpuCopy() || True(image.flags & (ImageFlagBits::Unbacked | ImageFlagBits::TrimmedLevels))) {
        return false;
    }
    if (image.info.num_samples == 1) {
        return true;
    }
    if constexpr (P::HAS_MSAA_DOWNLOADS) {
        return runtime.CanDownloadMsaa(image.info);
    } else {
        return false;
    }
}

template <class P>
void TextureCache<P>::DownloadMemory(DAddr cpu_addr, size_t size) {
    boost::container::small_vector<ImageId, 16> images;
    ForEachImageInRegion(cpu_addr, size, [this, &images](ImageId image_id, ImageBase& image) {
        if (!IsDownloadable(image)) {
            return;
        }
        image.flags &= ~ImageFlagBits::GpuModified;
        images.push_back(image_id);
    });
    if (images.empty()) {
        return;
    }
    std::ranges::sort(images, [this](ImageId lhs, ImageId rhs) {
        return slot_images[lhs].modification_tick < slot_images[rhs].modification_tick;
    });
    // Record the copies of a batch, wait for the GPU once, then swizzle in modification order (the
    // copies only read, so they do not depend on each other). Each copy takes a deferred staging
    // buffer, so the pool does not hand the same one out twice before the wait (OpenGL download
    // buffers have no fence). A batch is capped so the staging memory held at once stays bounded.
    constexpr size_t MaxBatchBytes = 64_MiB;
    struct PendingImageDownload {
        ImageId image_id;
        AsyncBuffer map;
        boost::container::small_vector<BufferImageCopy, 16> copies;
    };
    boost::container::small_vector<PendingImageDownload, 4> pending;
    size_t pending_bytes = 0;
    u64 pending_guest_bytes = 0;
    std::chrono::steady_clock::duration recording{};
    const auto flush_pending = [&] {
        if (pending.empty()) {
            return;
        }
        const auto finish_begin = std::chrono::steady_clock::now();
        runtime.Finish();
        const auto swizzle_begin = std::chrono::steady_clock::now();
        for (PendingImageDownload& entry : pending) {
            Image& image = slot_images[entry.image_id];
            SwizzleImage(*gpu_memory, image.gpu_addr, image.info, entry.copies,
                         entry.map.mapped_span, swizzle_data_buffer);
            runtime.FreeDeferredStagingBuffer(entry.map);
            ClearStaleImage(image);
        }
        static std::atomic<u32> logged_flush_downloads{};
        ReportLongImageDownload(logged_flush_downloads, "flush",
                                slot_images[pending.front().image_id].gpu_addr,
                                pending_guest_bytes, recording, swizzle_begin - finish_begin,
                                std::chrono::steady_clock::now() - swizzle_begin);
        pending.clear();
        pending_bytes = 0;
        pending_guest_bytes = 0;
        recording = {};
    };
    for (const ImageId image_id : images) {
        Image& image = slot_images[image_id];
        if (!pending.empty() && pending_bytes + image.unswizzled_size_bytes > MaxBatchBytes) {
            flush_pending();
        }
        const auto download_begin = std::chrono::steady_clock::now();
        auto map = runtime.DownloadStagingBuffer(image.unswizzled_size_bytes, true);
        auto copies = FullDownloadCopies(image.info);
        image.DownloadMemory(map, copies);
        recording += std::chrono::steady_clock::now() - download_begin;
        pending_bytes += image.unswizzled_size_bytes;
        pending_guest_bytes += image.guest_size_bytes;
        pending.push_back(PendingImageDownload{image_id, map, std::move(copies)});
    }
    flush_pending();
}

template <class P>
std::optional<VideoCore::RasterizerDownloadArea> TextureCache<P>::GetFlushArea(DAddr cpu_addr,
                                                                               u64 size) {
    std::optional<VideoCore::RasterizerDownloadArea> area{};
    ForEachImageInRegion(cpu_addr, size, [&](ImageId, ImageBase& image) {
        if (False(image.flags & ImageFlagBits::GpuModified)) {
            return;
        }
        if (!area) {
            area.emplace();
            area->start_address = cpu_addr;
            area->end_address = cpu_addr + size;
            area->preemtive = true;
        }
        area->start_address = (std::min)(area->start_address, image.cpu_addr);
        area->end_address = (std::max)(area->end_address, image.cpu_addr_end);
        for (auto image_view_id : image.image_view_ids) {
            auto& image_view = slot_image_views[image_view_id];
            image_view.flags |= ImageViewFlagBits::PreemtiveDownload;
        }
        area->preemtive &= image.info.forced_flushed;
        image.info.forced_flushed = true;
    });
    return area;
}

template <class P>
void TextureCache<P>::UnmapMemory(DAddr cpu_addr, size_t size) {
    boost::container::small_vector<ImageId, 16> deleted_images;
    ForEachImageInRegion(cpu_addr, size, [&](ImageId id, Image&) { deleted_images.push_back(id); });
    for (const ImageId id : deleted_images) {
        Image& image = slot_images[id];
        if (True(image.flags & ImageFlagBits::Tracked)) {
            UntrackImage(image, id);
        }
        UnregisterImage(id);
        DeleteImage(id);
    }
    ClearStale(cpu_addr, size);
}

template <class P>
void TextureCache<P>::UnmapGPUMemory(size_t as_id, GPUVAddr gpu_addr, size_t size) {
    boost::container::small_vector<ImageId, 16> deleted_images;
    ForEachImageInRegionGPU(as_id, gpu_addr, size,
                            [&](ImageId id, Image&) { deleted_images.push_back(id); });
    if (!deleted_images.empty()) {
        VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::Map);
    }
    for (const ImageId id : deleted_images) {
        Image& image = slot_images[id];
        if (False(image.flags & ImageFlagBits::CpuModified)) {
            image.flags |= ImageFlagBits::CpuModified;
            if (True(image.flags & ImageFlagBits::Tracked)) {
                UntrackImage(image, id);
            }
        }
        image.dirty_layers.clear(); // all of it
        if (True(image.flags & ImageFlagBits::Remapped)) {
            continue;
        }
        image.flags |= ImageFlagBits::Remapped;
    }
}

template <class P>
bool TextureCache<P>::BlitImage(const Tegra::Engines::Fermi2D::Surface& dst,
                                const Tegra::Engines::Fermi2D::Surface& src,
                                const Tegra::Engines::Fermi2D::Config& copy) {
    // A deferred clear parked before this copy has to land before it: realizing it afterwards
    // starts its clear pass after the copy and wipes what the copy wrote. That is what made
    // Ocarina of Time draw its terrain layer over the scene (#101): the game clears the
    // occlusion depth, blits the scene depth over it, and reads it back during the layer draw.
    // GetFramebufferId only flushes when it creates a framebuffer, so the bug starts on the
    // second frame. Texture cache copies bypass render passes and keep stream order here.
    // Only when the copy proceeds: a rejected blit writes nothing and must not realize the
    // clear ahead of the next draw that consumes it as its load-op. GetBlitImages cannot
    // invalidate the parked clear: framebuffer removal flushes first (RemoveFramebuffers).
    const auto result = GetBlitImages(dst, src, copy);
    if (!result) {
        return false;
    }
    runtime.FlushDeferredClear();
    const BlitImages images = *result;
    const ImageId dst_id = images.dst_id;
    const ImageId src_id = images.src_id;

    PrepareImage(src_id, false, false);
    PrepareImage(dst_id, true, false);

    Image& dst_image = slot_images[dst_id];
    Image& src_image = slot_images[src_id];
    if (True(dst_image.flags & ImageFlagBits::Unbacked)) {
        static u32 logged{};
        if (logged++ < 64) {
            LOG_INFO(HW_GPU, "2D blit into unbacked image {:#x}: dst {:#x} {}x{} from {:#x} {}x{}",
                     dst_image.gpu_addr, dst.Address(), dst.width, dst.height, src.Address(),
                     src.width, src.height);
        }
    }
    bool is_src_rescaled = True(src_image.flags & ImageFlagBits::Rescaled);
    bool is_dst_rescaled = True(dst_image.flags & ImageFlagBits::Rescaled);

    const bool is_resolve = src_image.info.num_samples != 1 && dst_image.info.num_samples == 1;
    if (is_src_rescaled != is_dst_rescaled) {
        if (ImageCanRescale(src_image)) {
            ScaleUp(src_image);
            is_src_rescaled = True(src_image.flags & ImageFlagBits::Rescaled);
            if (is_resolve) {
                dst_image.info.rescaleable = true;
                for (const auto& alias : dst_image.aliased_images) {
                    Image& other_image = slot_images[alias.id];
                    other_image.info.rescaleable = true;
                }
            }
        }
        if (ImageCanRescale(dst_image)) {
            ScaleUp(dst_image);
            is_dst_rescaled = True(dst_image.flags & ImageFlagBits::Rescaled);
        }
    }
    if (is_resolve && (is_src_rescaled != is_dst_rescaled)) {
        // A resolve requires both images to be the same dimensions. Resize down if needed.
        ScaleDown(src_image);
        ScaleDown(dst_image);
        is_src_rescaled = True(src_image.flags & ImageFlagBits::Rescaled);
        is_dst_rescaled = True(dst_image.flags & ImageFlagBits::Rescaled);
    }
    const auto& resolution = Settings::values.resolution_info;
    const auto scale_region = [&](Region2D& region) {
        region.start.x = resolution.ScaleUp(region.start.x);
        region.start.y = resolution.ScaleUp(region.start.y);
        region.end.x = resolution.ScaleUp(region.end.x);
        region.end.y = resolution.ScaleUp(region.end.y);
    };

    // TODO: Deduplicate
    const std::optional src_base = src_image.TryFindBase(src.Address());
    const SubresourceRange src_range{.base = src_base.value(), .extent = {1, 1}};
    const ImageViewInfo src_view_info(ImageViewType::e2D, images.src_format, src_range);
    // RGB9E5 can't be a color attachment, and a Vulkan blit only samples its source.
    const auto [src_framebuffer_id, src_view_id] =
        !FRAMEBUFFER_BLITS && images.src_format == PixelFormat::E5B9G9R9_FLOAT
            ? std::pair{FramebufferId{}, FindOrEmplaceImageView(src_id, src_view_info)}
            : RenderTargetFromImage(src_id, src_view_info);
    const auto [src_samples_x, src_samples_y] = SamplesLog2(src_image.info.num_samples);
    Region2D src_region{
        Offset2D{.x = copy.src_x0 >> src_samples_x, .y = copy.src_y0 >> src_samples_y},
        Offset2D{.x = copy.src_x1 >> src_samples_x, .y = copy.src_y1 >> src_samples_y},
    };
    if (is_src_rescaled) {
        scale_region(src_region);
    }

    const std::optional dst_base = dst_image.TryFindBase(dst.Address());
    const SubresourceRange dst_range{.base = dst_base.value(), .extent = {1, 1}};
    const ImageViewInfo dst_view_info(ImageViewType::e2D, images.dst_format, dst_range);
    const auto [dst_framebuffer_id, dst_view_id] = RenderTargetFromImage(dst_id, dst_view_info);
    const auto [dst_samples_x, dst_samples_y] = SamplesLog2(dst_image.info.num_samples);
    Region2D dst_region{
        Offset2D{.x = copy.dst_x0 >> dst_samples_x, .y = copy.dst_y0 >> dst_samples_y},
        Offset2D{.x = copy.dst_x1 >> dst_samples_x, .y = copy.dst_y1 >> dst_samples_y},
    };
    if (is_dst_rescaled) {
        scale_region(dst_region);
    }

    // Always call this after src_framebuffer_id was queried, as the address might be invalidated.
    Framebuffer* const dst_framebuffer = &slot_framebuffers[dst_framebuffer_id];
    if constexpr (FRAMEBUFFER_BLITS) {
        // OpenGL blits from framebuffers, not images
        Framebuffer* const src_framebuffer = &slot_framebuffers[src_framebuffer_id];
        runtime.BlitFramebuffer(dst_framebuffer, src_framebuffer, dst_region, src_region,
                                copy.filter, copy.operation);
    } else {
        // Vulkan can blit images, but it lacks format reinterpretations
        // Provide a framebuffer in case it's necessary
        ImageView& dst_view = slot_image_views[dst_view_id];
        ImageView& src_view = slot_image_views[src_view_id];
        runtime.BlitImage(dst_framebuffer, dst_view, src_view, dst_region, src_region, copy.filter,
                          copy.operation);
    }
    return true;
}

template <class P>
bool TextureCache<P>::NvidiaDlss(const Tegra::Engines::Fermi2D::Surface& dst,
                                 const Tegra::Engines::Fermi2D::Surface& src,
                                 const Tegra::Engines::Fermi2D::Surface& motion_vectors,
                                 const Tegra::Texture::TICEntry& depth, std::array<f32, 2> jitter) {
    if constexpr (FRAMEBUFFER_BLITS) {
        return false;
    } else {
        const Tegra::Engines::Fermi2D::Config copy{
            .operation = Tegra::Engines::Fermi2D::Operation::SrcCopy,
            .filter = Tegra::Engines::Fermi2D::Filter::Bilinear,
            .must_accelerate = true,
            .dst_x0 = 0,
            .dst_y0 = 0,
            .dst_x1 = static_cast<s32>(dst.width),
            .dst_y1 = static_cast<s32>(dst.height),
            .src_x0 = 0,
            .src_y0 = 0,
            .src_x1 = static_cast<s32>(src.width),
            .src_y1 = static_cast<s32>(src.height),
        };
        const auto result = GetBlitImages(dst, src, copy);
        if (!result) {
            return false;
        }
        const ImageInfo motion_info(motion_vectors);
        has_deleted_images = false;
        const ImageId motion_id =
            FindOrInsertImage(motion_info, motion_vectors.Address(), RelaxedOptions{});
        ImageId depth_id{};
        if (motion_id) {
            depth_id = FindOrInsertImage(ImageInfo(depth), depth.Address(), RelaxedOptions{});
        }
        if (has_deleted_images || !motion_id || !depth_id) {
            return false; // the colour and output ids may be stale; the caller blits instead
        }
        const ImageId dst_id = result->dst_id;
        const ImageId src_id = result->src_id;
        PrepareImage(src_id, false, false);
        PrepareImage(motion_id, false, false);
        PrepareImage(depth_id, false, false);
        PrepareImage(dst_id, true, false);
        if (slot_images[depth_id].info.format != PixelFormat::D32_FLOAT) {
            return false;
        }
        for (const ImageId id : {src_id, motion_id, depth_id, dst_id}) {
            if (slot_images[id].info.num_samples != 1) {
                return false;
            }
        }
        // With resolution scaling, bring the images that can follow to the same scale, as
        // BlitImage does. DLSS reads colour, motion and depth texel for texel: all at one scale.
        const auto rescaled = [&](ImageId id) {
            return True(slot_images[id].flags & ImageFlagBits::Rescaled);
        };
        if (rescaled(src_id) || rescaled(motion_id) || rescaled(depth_id) || rescaled(dst_id)) {
            for (const ImageId id : {src_id, motion_id, depth_id, dst_id}) {
                if (!rescaled(id) && ImageCanRescale(slot_images[id])) {
                    ScaleUp(slot_images[id]);
                }
            }
        }
        if (rescaled(motion_id) != rescaled(src_id) || rescaled(depth_id) != rescaled(src_id)) {
            return false;
        }
        const Extent3D src_size = slot_images[src_id].info.size;
        const auto& resolution = Settings::values.resolution_info;
        const std::array<u32, 2> render_size =
            rescaled(src_id) ? std::array{resolution.ScaleUp(src_size.width),
                                          resolution.ScaleUp(src_size.height)}
                             : std::array{src_size.width, src_size.height};
        // The jitter is in the input's guest pixels.
        if (rescaled(src_id)) {
            jitter = {jitter[0] * resolution.up_factor, jitter[1] * resolution.up_factor};
        }
        const auto view = [&](ImageId id, GPUVAddr address, PixelFormat format)
            -> std::optional<std::pair<FramebufferId, ImageViewId>> {
            const std::optional base = slot_images[id].TryFindBase(address);
            if (!base) {
                return std::nullopt;
            }
            const SubresourceRange range{.base = *base, .extent = {1, 1}};
            return RenderTargetFromImage(id, ImageViewInfo(ImageViewType::e2D, format, range));
        };
        const auto src_view = view(src_id, src.Address(), result->src_format);
        const auto motion_view = view(motion_id, motion_vectors.Address(), motion_info.format);
        const auto dst_view = view(dst_id, dst.Address(), result->dst_format);
        const std::optional depth_base = slot_images[depth_id].TryFindBase(depth.Address());
        if (!src_view || !motion_view || !dst_view || !depth_base) {
            return false;
        }
        const SubresourceRange depth_range{.base = *depth_base, .extent = {1, 1}};
        const ImageViewId depth_view = FindOrEmplaceImageView(
            depth_id, ImageViewInfo(ImageViewType::e2D, PixelFormat::D32_FLOAT, depth_range));
        // After the views: creating them may reallocate the framebuffer slots.
        Framebuffer* const dst_framebuffer = &slot_framebuffers[dst_view->first];
        return runtime.NvidiaDlss(dst_framebuffer, slot_image_views[dst_view->second],
                                  slot_image_views[src_view->second],
                                  slot_image_views[motion_view->second],
                                  slot_image_views[depth_view], render_size, jitter);
    }
}

template <class P>
std::optional<typename TextureCache<P>::OcarinaDlssImages> TextureCache<P>::OcarinaDlssViews(
    const Tegra::Engines::Fermi2D::Surface& dst, const Tegra::Engines::Fermi2D::Surface& src,
    GPUVAddr depth_address, const ImageInfo& depth_info) {
    if constexpr (FRAMEBUFFER_BLITS) {
        return std::nullopt;
    } else {
        const Tegra::Engines::Fermi2D::Config copy{
            .operation = Tegra::Engines::Fermi2D::Operation::SrcCopy,
            .filter = Tegra::Engines::Fermi2D::Filter::Bilinear,
            .must_accelerate = true,
            .dst_x0 = 0,
            .dst_y0 = 0,
            .dst_x1 = static_cast<s32>(dst.width),
            .dst_y1 = static_cast<s32>(dst.height),
            .src_x0 = 0,
            .src_y0 = 0,
            .src_x1 = static_cast<s32>(src.width),
            .src_y1 = static_cast<s32>(src.height),
        };
        const auto result = GetBlitImages(dst, src, copy);
        if (!result) {
            return std::nullopt;
        }
        has_deleted_images = false;
        const ImageId depth_id = FindOrInsertImage(depth_info, depth_address, RelaxedOptions{});
        if (has_deleted_images || !depth_id) {
            return std::nullopt;
        }
        PrepareImage(result->src_id, false, false);
        PrepareImage(depth_id, false, false);
        PrepareImage(result->dst_id, true, false);
        const auto view = [&](ImageId id, GPUVAddr address, PixelFormat format) {
            const std::optional base = slot_images[id].TryFindBase(address);
            if (!base) {
                return ImageViewId{};
            }
            const SubresourceRange range{.base = *base, .extent = {1, 1}};
            return FindOrEmplaceImageView(id, ImageViewInfo(ImageViewType::e2D, format, range));
        };
        const ImageViewId color = view(result->src_id, src.Address(), result->src_format);
        const ImageViewId target = view(result->dst_id, dst.Address(), result->dst_format);
        const ImageViewId depth = view(depth_id, depth_address, PixelFormat::D32_FLOAT);
        if (!color || !target || !depth) {
            return std::nullopt;
        }
        // After the views: creating them may reallocate the view slots.
        return OcarinaDlssImages{&slot_image_views[color], &slot_image_views[depth],
                                 &slot_image_views[target]};
    }
}

template <class P>
std::pair<typename P::ImageView*, bool> TextureCache<P>::TryFindFramebufferImageView(
    const Tegra::FramebufferConfig& config, DAddr cpu_addr) {
    // TODO: Properly implement this
    const auto it = page_table.find(cpu_addr >> YUZU_PAGEBITS);
    if (it == page_table.end()) {
        return {};
    }
    const auto view_format = [&]() {
        switch (config.pixel_format) {
        case Service::android::PixelFormat::Rgb565:
            return PixelFormat::R5G6B5_UNORM;
        case Service::android::PixelFormat::Bgra8888:
            return PixelFormat::B8G8R8A8_UNORM;
        case Service::android::PixelFormat::Rgba1010102:
            return PixelFormat::A2B10G10R10_UNORM;
        default:
            return PixelFormat::A8B8G8R8_UNORM;
        }
    }();

    const auto& image_map_ids = it->second;
    boost::container::small_vector<ImageId, 4> valid_image_ids;
    for (const ImageMapId map_id : image_map_ids) {
        const ImageMapView& map = slot_map_views[map_id];
        const ImageBase& image = slot_images[map.image_id];
        if (image.cpu_addr != cpu_addr) {
            continue;
        }
        if (image.image_view_ids.empty()) {
            continue;
        }
        if (!VideoCore::Surface::IsViewCompatible(image.info.format, view_format, false, true)) {
            static u32 rejected_alias_count{};
            if (NativeDrawDiagnostics() && rejected_alias_count++ < 8) {
                LOG_WARNING(HW_GPU, "Ignoring incompatible framebuffer alias: address={:#x}, image={}, view={}",
                            cpu_addr, image.info.format, view_format);
            }
            continue;
        }
        valid_image_ids.push_back(map.image_id);
    }

    const auto GetImageViewForFramebuffer = [&](ImageId image_id) {
        ImageViewInfo info{ImageViewType::e2D, view_format};
        if (config.blending == Tegra::BlendMode::Opaque) {
            info.x_source = static_cast<u8>(SwizzleSource::R);
            info.y_source = static_cast<u8>(SwizzleSource::G);
            info.z_source = static_cast<u8>(SwizzleSource::B);
            info.w_source = static_cast<u8>(SwizzleSource::OneFloat);
        }
        return std::make_pair(&slot_image_views[FindOrEmplaceImageView(image_id, info)],
                              slot_images[image_id].IsRescaled());
    };

    if (valid_image_ids.size() == 1) [[likely]] {
        return GetImageViewForFramebuffer(valid_image_ids.front());
    }

    if (valid_image_ids.size() > 0) [[unlikely]] {
        auto most_recent = std::ranges::max_element(valid_image_ids, [&](auto a, auto b) {
            return slot_images[a].modification_tick < slot_images[b].modification_tick;
        });
        return GetImageViewForFramebuffer(*most_recent);
    }

    return {};
}

template <class P>
bool TextureCache<P>::HasUncommittedFlushes() const noexcept {
    return !uncommitted_downloads.empty();
}

template <class P>
bool TextureCache<P>::HasAsyncFlushWork() const noexcept {
    return !uncommitted_downloads.empty() || !uncommitted_async_buffers.empty();
}

template <class P>
bool TextureCache<P>::ShouldWaitAsyncFlushes() const noexcept {
    return !committed_downloads.empty() && !committed_downloads.front().empty();
}

template <class P>
void TextureCache<P>::CommitAsyncFlushes() {
    // This is intentionally passing the value by copy
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        auto& download_ids = uncommitted_downloads;
        const size_t redundant_images = DeduplicatePendingImageDownloads(download_ids, slot_images);
        if (download_ids.empty()) {
            committed_downloads.emplace_back(std::move(uncommitted_downloads));
            uncommitted_downloads.clear();
            async_buffers.emplace_back(std::move(uncommitted_async_buffers));
            uncommitted_async_buffers.clear();
            return;
        }
        size_t total_size_bytes = 0;
        size_t last_async_buffer_id = uncommitted_async_buffers.size();
        bool any_none_dma = false;
        for (PendingDownload& download_info : download_ids) {
            if (download_info.is_swizzle) {
                total_size_bytes +=
                    Common::AlignUp(download_info.GetImage(slot_images).unswizzled_size_bytes, 64);
                any_none_dma = true;
                download_info.async_buffer_id = last_async_buffer_id;
            }
        }

        if (any_none_dma) {
            if (NativeDrawDiagnostics() && total_size_bytes >= 64_MiB) {
                LOG_WARNING(HW_GPU, "Large image readback batch: bytes={}, entries={}, duplicates_removed={}, submitted_batches={}, retained_buffers={}",
                            total_size_bytes, download_ids.size(), redundant_images,
                            committed_downloads.size(), async_buffers_death_ring.size());
                size_t traced_images = 0;
                for (const auto& download : download_ids) {
                    if (!download.is_swizzle || traced_images++ >= 8) continue;
                    const auto& image = download.GetImage(slot_images);
                    LOG_WARNING(HW_GPU, "Readback image: gpu={:#x}, cpu={:#x}, format={}, size={}x{}x{}, levels={}, layers={}, layer_stride={}, guest_bytes={}, download_bytes={}",
                                image.gpu_addr, image.cpu_addr, image.info.format,
                                image.info.size.width, image.info.size.height, image.info.size.depth,
                                image.info.resources.levels, image.info.resources.layers,
                                image.info.layer_stride, image.guest_size_bytes, image.unswizzled_size_bytes);
                }
            }
            auto download_map = runtime.DownloadStagingBuffer(total_size_bytes, true);
            for (const PendingDownload& download_info : download_ids) {
                if (download_info.is_swizzle) {
                    Image& image = download_info.GetImage(slot_images);
                    const auto copies = FullDownloadCopies(image.info);
                    image.DownloadMemory(download_map, copies);
                    download_map.offset += Common::AlignUp(image.unswizzled_size_bytes, 64);
                }
            }
            uncommitted_async_buffers.emplace_back(download_map);
        }

        async_buffers.emplace_back(std::move(uncommitted_async_buffers));
        uncommitted_async_buffers.clear();
    }
    committed_downloads.emplace_back(std::move(uncommitted_downloads));
    uncommitted_downloads.clear();
}

template <class P>
void TextureCache<P>::PopAsyncFlushes() {
    if (committed_downloads.empty()) {
        return;
    }
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        const auto& download_ids = committed_downloads.front();
        if (download_ids.empty()) {
            committed_downloads.pop_front();
            async_buffers.pop_front();
            return;
        }
        auto download_map = std::move(async_buffers.front());
        for (size_t i = download_ids.size(); i > 0; i--) {
            auto& download_info = download_ids[i - 1];
            auto& download_buffer = download_map[download_info.async_buffer_id];
            if (download_info.is_swizzle) {
                const ImageBase& image = download_info.GetImage(slot_images);
                const auto copies = FullDownloadCopies(image.info);
                download_buffer.offset -= Common::AlignUp(image.unswizzled_size_bytes, 64);
                std::span<u8> download_span =
                    download_buffer.mapped_span.subspan(download_buffer.offset);
                const auto swizzle_begin = std::chrono::steady_clock::now();
                SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, download_span, swizzle_data_buffer);
                ClearStaleImage(image);
                static std::atomic<u32> logged_async_swizzles{};
                ReportLongImageDownload(logged_async_swizzles, "async flush", image.gpu_addr,
                                        image.guest_size_bytes, {}, {},
                                        std::chrono::steady_clock::now() - swizzle_begin);
            } else {
                const BufferDownload& buffer_info = slot_buffer_downloads[download_info.object_id];
                std::span<u8> download_span =
                    download_buffer.mapped_span.subspan(download_buffer.offset);
                gpu_memory->WriteBlockUnsafe(buffer_info.address, download_span.data(),
                                             buffer_info.size);
                ClearStaleGpu(buffer_info.address, buffer_info.size);
                slot_buffer_downloads.erase(download_info.object_id);
            }
        }
        for (auto& download_buffer : download_map) {
            async_buffers_death_ring.emplace_back(download_buffer);
        }
        committed_downloads.pop_front();
        async_buffers.pop_front();
    } else {
        const auto& download_ids = committed_downloads.front();
        if (download_ids.empty()) {
            committed_downloads.pop_front();
            return;
        }
        size_t total_size_bytes = 0;
        for (const PendingDownload& download_info : download_ids) {
            if (download_info.is_swizzle) {
                total_size_bytes += download_info.GetImage(slot_images).unswizzled_size_bytes;
            }
        }
        auto download_map = runtime.DownloadStagingBuffer(total_size_bytes);
        const size_t original_offset = download_map.offset;
        for (const PendingDownload& download_info : download_ids) {
            if (!download_info.is_swizzle) {
                continue;
            }
            Image& image = download_info.GetImage(slot_images);
            const auto copies = FullDownloadCopies(image.info);
            image.DownloadMemory(download_map, copies);
            download_map.offset += image.unswizzled_size_bytes;
        }
        // Wait for downloads to finish
        runtime.Finish();
        download_map.offset = original_offset;
        std::span<u8> download_span = download_map.mapped_span;
        for (const PendingDownload& download_info : download_ids) {
            if (!download_info.is_swizzle) {
                continue;
            }
            const ImageBase& image = download_info.GetImage(slot_images);
            const auto copies = FullDownloadCopies(image.info);
            SwizzleImage(*gpu_memory, image.gpu_addr, image.info, copies, download_span, swizzle_data_buffer);
            ClearStaleImage(image);
            download_map.offset += image.unswizzled_size_bytes;
            download_span = download_span.subspan(image.unswizzled_size_bytes);
        }
        committed_downloads.pop_front();
    }
}

template <class P>
ImageId TextureCache<P>::DmaImageId(const Tegra::DMA::ImageOperand& operand, bool is_upload) {
    const ImageInfo dst_info(operand);
    const ImageId dst_id = FindDMAImage(dst_info, operand.address);
    if (!dst_id) {
        return NULL_IMAGE_ID;
    }
    auto& image = slot_images[dst_id];
    if (False(image.flags & ImageFlagBits::GpuModified)) {
        // No need to waste time on an image that's synced with guest
        return NULL_IMAGE_ID;
    }
    if (image.info.type == ImageType::e3D) {
        // Don't accelerate 3D images.
        return NULL_IMAGE_ID;
    }
    if (!is_upload && !image.info.dma_downloaded) {
        // Force a full sync.
        image.info.dma_downloaded = true;
        return NULL_IMAGE_ID;
    }
    const auto base = image.TryFindBase(operand.address);
    if (!base) {
        return NULL_IMAGE_ID;
    }
    return dst_id;
}

template <class P>
bool TextureCache<P>::IsRescaling() const noexcept {
    return is_rescaling;
}

template <class P>
bool TextureCache<P>::IsRescaling(const ImageViewBase& image_view) const noexcept {
    if (image_view.type == ImageViewType::Buffer) {
        return false;
    }
    const ImageBase& image = slot_images[image_view.image_id];
    return True(image.flags & ImageFlagBits::Rescaled);
}

template <class P>
bool TextureCache<P>::IsRegionGpuModified(DAddr addr, size_t size) {
    bool is_modified = false;
    ForEachImageInRegion(addr, size, [&is_modified](ImageId, ImageBase& image) {
        if (False(image.flags & ImageFlagBits::GpuModified)) {
            return false;
        }
        is_modified = true;
        return true;
    });
    return is_modified;
}

template <class P>
bool TextureCache<P>::UsesLegacySectorLayout(GPUVAddr gpu_addr) {
    const std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
    if (!cpu_addr) {
        return false;
    }
    // Legacy only when every image here with a known layout says so: overlapping images that
    // disagree leave the order ambiguous, and the native order stays.
    bool legacy = false;
    bool native = false;
    ForEachImageInRegion(*cpu_addr, 1, [&](ImageId, ImageBase& image) {
        if (image.info.sector_layout_known) {
            (image.info.legacy_sector_layout ? legacy : native) = true;
        }
        return legacy && native;
    });
    return legacy && !native;
}

template <class P>
std::pair<typename TextureCache<P>::Image*, BufferImageCopy> TextureCache<P>::DmaBufferImageCopy(
    const Tegra::DMA::ImageCopy& copy_info, const Tegra::DMA::BufferOperand& buffer_operand,
    const Tegra::DMA::ImageOperand& image_operand, ImageId image_id, bool modifies_image) {
    const auto [level, base] = PrepareDmaImage(image_id, image_operand.address, modifies_image);
    auto* image = &slot_images[image_id];
    const u32 buffer_size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    const u32 bpp = VideoCore::Surface::BytesPerBlock(image->info.format);
    const auto convert = [old_bpp = image_operand.bytes_per_pixel, bpp](u32 value) {
        return (old_bpp * value) / bpp;
    };
    const u32 base_x = convert(image_operand.params.origin.x.Value());
    const u32 base_y = image_operand.params.origin.y.Value();
    const u32 length_x = convert(copy_info.length_x);
    const u32 length_y = copy_info.length_y;

    const BufferImageCopy copy{
        .buffer_offset = 0,
        .buffer_size = buffer_size,
        .buffer_row_length = convert(buffer_operand.pitch),
        .buffer_image_height = buffer_operand.height,
        .image_subresource =
            {
                .base_level = static_cast<s32>(level),
                .base_layer = static_cast<s32>(base),
                .num_layers = 1,
            },
        .image_offset =
            {
                .x = static_cast<s32>(base_x),
                .y = static_cast<s32>(base_y),
                .z = 0,
            },
        .image_extent =
            {
                .width = length_x,
                .height = length_y,
                .depth = 1,
            },
    };
    return {image, copy};
}

template <class P>
void TextureCache<P>::DownloadImageIntoBuffer(typename TextureCache<P>::Image* image,
                                              typename TextureCache<P>::BufferType buffer,
                                              size_t buffer_offset,
                                              std::span<const VideoCommon::BufferImageCopy> copies,
                                              GPUVAddr address, size_t size) {
    if constexpr (IMPLEMENTS_ASYNC_DOWNLOADS) {
        const BufferDownload new_buffer_download{address, size};
        auto slot = slot_buffer_downloads.insert(new_buffer_download);
        const PendingDownload new_download{false, uncommitted_async_buffers.size(), slot};
        uncommitted_downloads.emplace_back(new_download);
        auto download_map = runtime.DownloadStagingBuffer(size, true);
        uncommitted_async_buffers.emplace_back(download_map);
        std::array buffers{
            buffer,
            download_map.buffer,
        };
        std::array<size_t, 2> buffer_offsets{
            buffer_offset,
            download_map.offset,
        };
        image->DownloadMemory(buffers, buffer_offsets, copies);
    } else {
        image->DownloadMemory(buffer, buffer_offset, copies);
    }
}

template <class P>
void TextureCache<P>::RefreshContents(Image& image, ImageId image_id) {
    if (False(image.flags & ImageFlagBits::CpuModified)) {
        // Only upload modified images
        return;
    }
    VideoCommon::Nx2Profile::Scope profile_scope{VideoCommon::Nx2Profile::Zone::TextureUpload};

    // A refresh that holds the GPU thread (50 ms or more, or 64 MiB uploaded) is logged with the
    // path it took, 64 lines.
    const auto refresh_begin = std::chrono::steady_clock::now();
    const u64 uploaded_before = uploaded_bytes;
    const char* refresh_path = "full upload";
    bool full_upload = false;
    SCOPE_EXIT {
        const auto refresh_time = std::chrono::steady_clock::now() - refresh_begin;
        const u64 refresh_bytes = uploaded_bytes - uploaded_before;
        static std::atomic<u32> logged_refreshes{};
        if ((refresh_time < std::chrono::milliseconds{50} && refresh_bytes < 64_MiB) ||
            logged_refreshes.fetch_add(1, std::memory_order_relaxed) >= 64) {
            return;
        }
        std::string backing;
        if (full_upload && refresh_bytes >= 64_MiB && False(image.flags & ImageFlagBits::Sparse)) {
            // Pages without memory read zero: an upload of only those uploads nothing.
            constexpr u64 PageSize = 4_KiB;
            const GPUVAddr first_page = Common::AlignDown(image.gpu_addr, PageSize);
            const GPUVAddr end_page =
                Common::AlignUp(image.gpu_addr + image.guest_size_bytes, PageSize);
            u64 backed_pages = 0;
            for (GPUVAddr page = first_page; page < end_page; page += PageSize) {
                backed_pages += gpu_memory->GetPointer(page) != nullptr ? 1 : 0;
            }
            backing = fmt::format(", {} of {} pages with memory", backed_pages,
                                  (end_page - first_page) / PageSize);
        }
        LOG_INFO(HW_GPU,
                 "Long GPU-thread operation: image refresh ({}) {:.1f} ms, {} MiB uploaded: {:#x} "
                 "{} MiB {}x{}x{} format {} levels {} layers {}{}{}{}{}",
                 refresh_path, std::chrono::duration<double, std::milli>(refresh_time).count(),
                 refresh_bytes >> 20, image.gpu_addr, image.guest_size_bytes >> 20,
                 image.info.size.width, image.info.size.height, image.info.size.depth,
                 static_cast<u32>(image.info.format), image.info.resources.levels,
                 image.info.resources.layers,
                 True(image.flags & ImageFlagBits::Sparse) ? ", sparse" : "",
                 True(image.flags & ImageFlagBits::GpuModified) ? ", GPU-modified" : "",
                 True(image.flags & ImageFlagBits::Unbacked) ? ", unbacked" : "", backing);
    };

    image.flags &= ~ImageFlagBits::CpuModified;
    ++image.other_writes;

    // Images uploaded by layer stay tracked while some of their layers are dirty.
    if (False(image.flags & ImageFlagBits::Tracked)) {
        TrackImage(image, image_id);
    }

    // Made where a dropped GPU-written image left stale bytes (MarkStale): nothing to upload.
    // Pages the buffer cache wrote on the GPU may hold data written back since: upload those.
    if (SkipStaleUploads() && image.dirty_layers.empty() &&
        False(image.flags & (ImageFlagBits::Sparse | ImageFlagBits::SparseLayers |
                             ImageFlagBits::SparseTiles | ImageFlagBits::RangeUploads |
                             ImageFlagBits::Unbacked)) &&
        IsAllStale(image.cpu_addr, image.guest_size_bytes)) {
        if (!Tegra::Nx2MayBeGpuWritten(image.cpu_addr, image.guest_size_bytes)) {
            ++tc_stale_skips;
            tc_stale_skip_bytes += image.guest_size_bytes;
            refresh_path = "stale, skipped";
            runtime.TransitionImageLayout(image);
            return;
        }
        ++tc_stale_gpu_written;
    }

    if (True(image.flags & ImageFlagBits::SparseLayers)) {
        refresh_path = "sparse layers";
        RefreshSparseLayers(image);
        return;
    }
    if (True(image.flags & ImageFlagBits::SparseTiles)) {
        refresh_path = "sparse tiles";
        RefreshSparseTiles(image);
        return;
    }
    if (True(image.flags & ImageFlagBits::RangeUploads) && UploadWrittenRanges(image)) {
        refresh_path = "written ranges";
        return;
    }
    image.dirty_ranges.clear();
    if (!image.dirty_layers.empty() && UploadWrittenLayers(image)) {
        refresh_path = "written layers";
        runtime.InsertUploadMemoryBarrier();
        return;
    }
    image.dirty_layers.clear();

    if (True(image.flags & ImageFlagBits::Unbacked)) {
        // Nothing to upload into (and staging it could take tens of GB).
        refresh_path = "unbacked";
        runtime.TransitionImageLayout(image);
        return;
    }

    if ((image.guest_size_bytes >> 32) != 0) {
        // Staging sizes, MapSizeBytes and the unswizzled sizes are 32-bit, so a whole upload of
        // a guest image of 4 GiB or more reads and writes past its buffers. Such images only get
        // their written layers and sparse pieces above (on Vulkan, images over 2^28 host texels
        // are unbacked anyway, so this is only reached with trimmed host levels).
        static std::atomic<bool> logged{};
        if (!logged.exchange(true, std::memory_order_relaxed)) {
            LOG_WARNING(HW_GPU, "Not uploading the whole of a {} MiB image at {:#x}",
                        image.guest_size_bytes >> 20, image.gpu_addr);
        }
        refresh_path = "4 GiB or more, not uploaded";
        runtime.TransitionImageLayout(image);
        return;
    }

    if (image.info.num_samples > 1 && !runtime.CanUploadMSAA()) {
        LOG_WARNING(HW_GPU, "MSAA image uploads are not implemented");
        refresh_path = "MSAA, not uploaded";
        runtime.TransitionImageLayout(image);
        return;
    }
    if (True(image.flags & ImageFlagBits::AsynchronousDecode)) {
        refresh_path = "async decode";
        QueueAsyncDecode(image, image_id);
        return;
    }

    const bool gpu_unswizzle_enabled = Settings::values.gpu_unswizzle_enabled.GetValue();

    if (gpu_unswizzle_enabled &&
        IsPixelFormatBCn(image.info.format) &&
        image.info.type == ImageType::e3D &&
        image.info.resources.levels == 1 &&
        image.info.resources.layers == 1 &&
        MapSizeBytes(image) >= gpu_unswizzle_maxsize &&
        False(image.flags & ImageFlagBits::GpuModified)) {

        refresh_path = "GPU unswizzle";
        QueueAsyncUnswizzle(image, image_id);
        return;
    }
    // A colour image none of whose guest pages has memory reads all zeros: title 1 loads several
    // of 95-341 MiB that way, 60-230 ms each of reading, swizzling and uploading zeros. Those are
    // cleared on the GPU instead; EDEN_NX2_TC_ZERO_CLEAR=0 uploads them.
    // EDEN_NX2_ZERO_CLEAR=1 (opt-in) also clears colour and depth images of 1 MiB or more whose
    // pages do have memory but read all zero (targets not drawn into yet): Splatoon Raiders
    // uploaded seven 6400x6400 targets (936 MiB) in ~480 ms at every raid entry. Fresh memory
    // faults in page by page as it is read, so images of 16 MiB or more are read in up to 8 parts
    // at once.
    static const bool zero_clear = [] {
        const char* value = std::getenv("EDEN_NX2_TC_ZERO_CLEAR");
        return value == nullptr || value[0] != '0';
    }();
    static const bool zero_memory = [] {
        const char* value = std::getenv("EDEN_NX2_ZERO_CLEAR");
        return value != nullptr && value[0] == '1';
    }();
    const SurfaceType surface_type = GetFormatType(image.info.format);
    const bool clearable_type =
        surface_type == SurfaceType::ColorTexture ||
        (zero_memory &&
         (surface_type == SurfaceType::Depth || surface_type == SurfaceType::DepthStencil));
    const auto reads_zero = [&] {
        constexpr u64 PageSize = 4_KiB;
        static constexpr std::array<u8, PageSize> zeros{};
        std::atomic<bool> nonzero{false};
        const auto check = [&](GPUVAddr first, GPUVAddr last) {
            for (GPUVAddr page = Common::AlignDown(first, PageSize);
                 page < last && !nonzero.load(std::memory_order_relaxed); page += PageSize) {
                const u8* const pointer = gpu_memory->GetPointer(page);
                if (pointer == nullptr) {
                    continue;
                }
                const GPUVAddr from = (std::max)(page, first);
                const u64 bytes = (std::min)(page + PageSize, last) - from;
                if (!zero_memory ||
                    std::memcmp(pointer + (from - page), zeros.data(), bytes) != 0) {
                    nonzero.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        };
        const GPUVAddr begin = image.gpu_addr;
        const GPUVAddr end = begin + image.guest_size_bytes;
        // Most images that are not zero say so in their first page.
        check(begin, (std::min)(begin + PageSize, end));
        if (nonzero.load(std::memory_order_relaxed)) {
            return false;
        }
        constexpr u64 PartBytes = 16_MiB;
        const u64 parts =
            zero_memory ? (std::min<u64>)(Common::DivCeil(image.guest_size_bytes, PartBytes), 8)
                        : 1;
        const u64 step = Common::AlignUp(Common::DivCeil(image.guest_size_bytes, parts), PageSize);
        std::vector<std::future<void>> jobs;
        for (u64 part = 1; part < parts && begin + part * step < end; ++part) {
            const GPUVAddr first = begin + part * step;
            jobs.push_back(
                std::async(std::launch::async, check, first, (std::min)(first + step, end)));
        }
        check(begin, (std::min)(begin + step, end));
        for (auto& job : jobs) {
            job.wait();
        }
        return !nonzero.load(std::memory_order_relaxed);
    };
    if (zero_clear && runtime.CanClearImages() &&
        image.guest_size_bytes >= (zero_memory ? 1_MiB : 16_MiB) && image.info.num_samples == 1 &&
        clearable_type && !VideoCore::Surface::IsPixelFormatASTC(image.info.format) &&
        !IsPixelFormatBCn(image.info.format) &&
        False(image.flags & (ImageFlagBits::Sparse | ImageFlagBits::Converted |
                             ImageFlagBits::AcceleratedUpload)) &&
        !Tegra::Nx2MayBeGpuWritten(image.cpu_addr, image.guest_size_bytes) && reads_zero()) {
        refresh_path = zero_memory ? "no memory or all zero, cleared" : "no memory, cleared";
        ++tc_zero_clears;
        tc_zero_clear_bytes += image.guest_size_bytes;
        runtime.ClearImageToZero(image);
        return;
    }

    full_upload = true;
    ++images_uploaded;
    uploaded_bytes += image.guest_size_bytes;
    largest_upload = (std::max<u64>)(largest_upload, image.guest_size_bytes);
    auto staging = runtime.UploadStagingBuffer(MapSizeBytes(image));
    UploadImageContents(image, staging);
    runtime.InsertUploadMemoryBarrier();
    // The host GPU has its own copy now and the guest rarely reads this memory again: TickFrame
    // marks it cold, so the OS pages it out before anything else when it needs RAM (title 1
    // streams several GB of textures through it).
    constexpr u64 ColdHintMinBytes = 64_KiB;
    if (image.guest_size_bytes >= ColdHintMinBytes &&
        False(image.flags & ImageFlagBits::GpuModified)) {
        if (True(image.flags & ImageFlagBits::Sparse)) {
            // Non-contiguous: each mapped piece (title 1 maps its own memory into these).
            ForEachSparseSegment(image, [&](GPUVAddr, DAddr address, size_t size) {
                uploaded_ranges.emplace_back(address, size);
            });
        } else {
            uploaded_ranges.emplace_back(image.cpu_addr, image.guest_size_bytes);
        }
    }
}

template <class P>
bool TextureCache<P>::UploadsByLayer(const ImageBase& image) const noexcept {
    // Title 1's terrain arrays (8192x8192x68 ASTC, 1.45 GB each) get a few layers streamed in at a
    // time; re-decoding all of them for each took a 2 GiB staging buffer and stalled the GPU.
    if constexpr (requires { &Runtime::AccelerateImageUploadLayers; }) {
        if (True(image.flags & ImageFlagBits::SparseLayers)) {
            return true;
        }
        return True(image.flags & ImageFlagBits::AcceleratedUpload) &&
               image.info.type == ImageType::e2D && image.info.resources.layers > 1 &&
               image.guest_size_bytes >= LayerUploadMinSize &&
               False(image.flags & (ImageFlagBits::Unbacked | ImageFlagBits::Sparse |
                                    ImageFlagBits::Remapped));
    } else {
        return false;
    }
}

template <class P>
void TextureCache<P>::MarkLayersWritten(Image& image, DAddr cpu_addr, size_t size) {
    if (True(image.flags & ImageFlagBits::CpuModified) && image.dirty_layers.empty()) {
        return; // all of it is dirty already
    }
    const u32 layers = static_cast<u32>(image.info.resources.layers);
    if (False(image.flags & ImageFlagBits::CpuModified)) {
        image.flags |= ImageFlagBits::CpuModified;
        image.dirty_layers.assign(Common::DivCeil(layers, 64U), 0);
    }
    const DAddr begin = (std::max<DAddr>)(cpu_addr, image.cpu_addr) - image.cpu_addr;
    const DAddr end = (std::min<DAddr>)(cpu_addr + size, image.cpu_addr + image.guest_size_bytes) -
                      image.cpu_addr;
    const u32 stride = image.info.layer_stride;
    if (end <= begin || stride == 0) {
        return;
    }
    const u32 last = (std::min)(static_cast<u32>((end - 1) / stride), layers - 1);
    for (u32 layer = static_cast<u32>(begin / stride); layer <= last; ++layer) {
        image.dirty_layers[layer / 64] |= u64{1} << (layer % 64);
    }
}

template <class P>
void TextureCache<P>::MarkRangeWritten(Image& image, DAddr cpu_addr, size_t size) {
    if (True(image.flags & ImageFlagBits::CpuModified) && image.dirty_ranges.empty()) {
        return; // all of it is dirty already
    }
    const DAddr begin = (std::max<DAddr>)(cpu_addr, image.cpu_addr);
    const DAddr end = (std::min<DAddr>)(cpu_addr + size, image.cpu_addr + image.guest_size_bytes);
    if (end <= begin) {
        return;
    }
    image.flags |= ImageFlagBits::CpuModified;
    image.dirty_ranges.emplace_back(begin - image.cpu_addr, end - begin);
}

template <class P>
void TextureCache<P>::RefreshSparseTiles(Image& image) {
    if constexpr (requires { &Image::BindTiles; }) {
        // The ranges written since the last upload, or, for a new image, all of it that can hold
        // data (the tiles the game maps there, or the blocks written).
        std::vector<std::pair<u64, u64>> ranges = std::move(image.dirty_ranges);
        image.dirty_ranges.clear();
        if (ranges.empty()) {
            ranges = Tegra::Nx2SparseDataRanges(image.gpu_addr, image.guest_size_bytes);
        }
        const std::vector<LevelRect> rects =
            SparseTileRects(image.info, ranges, image.SparseTileWidth(), image.SparseTileHeight(),
                            image.SparseTailLevel());
        if (rects.empty() || !BindSparseTiles(image, rects)) {
            runtime.TransitionImageLayout(image);
            return;
        }
        const size_t bytes = UnswizzledRectsBytes(image.info, rects);
        auto staging = runtime.UploadStagingBuffer(bytes);
        const std::vector<BufferImageCopy> copies =
            UnswizzleImageRects(*gpu_memory, image.gpu_addr, image.info, rects, staging.mapped_span);
        image.UploadMemory(staging, copies);
        runtime.InsertUploadMemoryBarrier();
        ++images_uploaded;
        uploaded_bytes += bytes;
    }
}

template <class P>
bool TextureCache<P>::BindSparseTiles(Image& image, std::span<const LevelRect> rects) {
    if constexpr (requires { &Image::BindTiles; }) {
        const u64 bound_bytes = ImageMemoryBytes(image);
        const bool bound = image.BindTiles(rects);
        // RegisterImage counts what an image has bound by then; later binds are added here.
        if (True(image.flags & ImageFlagBits::Registered)) {
            total_used_memory += ImageMemoryBytes(image) - bound_bytes;
        }
        return bound;
    } else {
        return false;
    }
}

template <class P>
bool TextureCache<P>::UploadWrittenLayers(Image& image) {
    if constexpr (requires { &Runtime::AccelerateImageUploadLayers; }) {
        const u32 num_layers = static_cast<u32>(image.info.resources.layers);
        std::vector<u32> layers;
        for (u32 layer = 0; layer < num_layers; ++layer) {
            if ((image.dirty_layers[layer / 64] >> (layer % 64)) & 1) {
                layers.push_back(layer);
            }
        }
        image.dirty_layers.clear();
        if (layers.empty() || layers.size() == num_layers) {
            return false;
        }
        UploadLayers(image, layers);
        return true;
    } else {
        return false;
    }
}

template <class P>
bool TextureCache<P>::UploadWrittenRanges(Image& image) {
    const std::vector<std::pair<u64, u64>> ranges = std::move(image.dirty_ranges);
    image.dirty_ranges.clear();
    if (ranges.empty()) {
        return false;
    }
    // 128 texels on a side are whole GOB columns at any size of these one-texel blocks.
    constexpr u32 TileSize = 128;
    const std::vector<LevelRect> rects =
        SparseTileRects(image.info, ranges, TileSize, TileSize, image.info.resources.levels);
    if (rects.empty()) {
        return true;
    }
    const size_t bytes = UnswizzledRectsBytes(image.info, rects);
    auto staging = runtime.UploadStagingBuffer(bytes);
    const std::vector<BufferImageCopy> copies =
        UnswizzleImageRects(*gpu_memory, image.gpu_addr, image.info, rects, staging.mapped_span);
    image.UploadMemory(staging, copies);
    runtime.InsertUploadMemoryBarrier();
    ++images_uploaded;
    uploaded_bytes += bytes;
    return true;
}

template <class P>
void TextureCache<P>::RefreshSparseLayers(Image& image) {
    if constexpr (requires { &Image::BindLayers; }) {
        // The layers written since the last upload, or, for a new image, every layer any of whose
        // blocks has been written.
        const u32 num_layers = static_cast<u32>(image.info.resources.layers);
        const u64 stride = image.info.layer_stride;
        std::vector<u32> layers;
        for (u32 layer = 0; layer < num_layers; ++layer) {
            const bool written =
                image.dirty_layers.empty()
                    ? !Tegra::Nx2SparseUnwritten(image.gpu_addr + layer * stride, stride)
                    : ((image.dirty_layers[layer / 64] >> (layer % 64)) & 1) != 0;
            if (written) {
                layers.push_back(layer);
            }
        }
        image.dirty_layers.clear();
        const std::vector<u32> bound =
            layers.empty() ? std::vector<u32>{} : BindSparseLayers(image, layers);
        if (bound.empty()) {
            runtime.TransitionImageLayout(image);
            return;
        }
        UploadLayers(image, bound);
        runtime.InsertUploadMemoryBarrier();
    }
}

template <class P>
std::vector<u32> TextureCache<P>::BindSparseLayers(Image& image, std::span<const u32> layers) {
    if constexpr (requires { &Image::BindLayers; }) {
        const u64 bound_bytes = ImageMemoryBytes(image);
        std::vector<u32> bound = image.BindLayers(layers);
        // RegisterImage counts what an image has bound by then; later binds are added here.
        if (True(image.flags & ImageFlagBits::Registered)) {
            total_used_memory += ImageMemoryBytes(image) - bound_bytes;
        }
        return bound;
    } else {
        return {};
    }
}

template <class P>
void TextureCache<P>::UploadLayers(Image& image, std::span<const u32> layers) {
    if constexpr (requires { &Runtime::AccelerateImageUploadLayers; }) {
        if (False(image.flags & ImageFlagBits::AcceleratedUpload)) {
            // Plain formats: each layer unswizzled on its own into its layer of the image.
            ImageInfo layer_info = image.info;
            layer_info.resources.layers = 1;
            const size_t layer_size = CalculateUnswizzledSizeBytes(layer_info);
            auto staging = runtime.UploadStagingBuffer(layer_size * layers.size());
            std::vector<BufferImageCopy> copies;
            for (size_t i = 0; i < layers.size(); ++i) {
                const GPUVAddr layer_addr = image.gpu_addr + u64{layers[i]} * image.info.layer_stride;
                Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::UnsafeRead>
                    swizzle_data(*gpu_memory, layer_addr, image.info.layer_stride,
                                 &swizzle_data_buffer);
                auto layer_copies =
                    UnswizzleImage(*gpu_memory, layer_addr, layer_info, swizzle_data,
                                   staging.mapped_span.subspan(i * layer_size, layer_size));
                for (BufferImageCopy copy : layer_copies) {
                    copy.buffer_offset += i * layer_size;
                    copy.image_subresource.base_layer = static_cast<s32>(layers[i]);
                    copies.push_back(copy);
                }
            }
            image.UploadMemory(staging, copies);
            ++images_uploaded;
            uploaded_bytes += u64{image.info.layer_stride} * layers.size();
            ++layer_uploads;
            return;
        }
        // Each layer's mip chain from the first level the host keeps on (as UploadImageContents).
        auto uploads = FullUploadSwizzles(image.info);
        const size_t level_offset = uploads[image.host_level_offset].buffer_offset;
        const size_t tail = image.info.layer_stride - level_offset;
        auto staging = runtime.UploadStagingBuffer(tail * layers.size());
        for (size_t i = 0; i < layers.size(); ++i) {
            gpu_memory->ReadBlock(image.gpu_addr + u64{layers[i]} * image.info.layer_stride +
                                      level_offset,
                                  staging.mapped_span.data() + i * tail, tail,
                                  VideoCommon::CacheType::NoTextureCache);
        }
        uploads.erase(uploads.begin(), uploads.begin() + image.host_level_offset);
        for (auto& upload : uploads) {
            upload.buffer_offset -= level_offset;
        }
        runtime.AccelerateImageUploadLayers(image, staging, uploads, layers);
        ++images_uploaded;
        uploaded_bytes += tail * layers.size();
        ++layer_uploads;
    }
}

template <class P>
template <typename StagingBuffer>
void TextureCache<P>::UploadImageContents(Image& image, StagingBuffer& staging) {
    const std::span<u8> mapped_span = staging.mapped_span;
    const GPUVAddr gpu_addr = image.gpu_addr;

    if (True(image.flags & ImageFlagBits::AcceleratedUpload) && image.host_level_offset != 0) {
        // Only levels >= host_level_offset exist on the host: read each layer's tail of the mip
        // chain into a compact buffer (layer stride = that tail's size) and decode those levels.
        auto uploads = FullUploadSwizzles(image.info);
        const size_t level_offset = uploads[image.host_level_offset].buffer_offset;
        const size_t tail = image.info.layer_stride - level_offset;
        for (s32 layer = 0; layer < image.info.resources.layers; ++layer) {
            gpu_memory->ReadBlock(gpu_addr + layer * image.info.layer_stride + level_offset,
                                  mapped_span.data() + layer * tail, tail,
                                  VideoCommon::CacheType::NoTextureCache);
        }
        uploads.erase(uploads.begin(), uploads.begin() + image.host_level_offset);
        for (auto& upload : uploads) {
            upload.buffer_offset -= level_offset;
        }
        runtime.AccelerateImageUpload(image, staging, uploads, 0, 0);
        return;
    }
    if (True(image.flags & ImageFlagBits::AcceleratedUpload)) {
        // Reused staging buffers can be larger than the image; their extra capacity is not
        // guest texture data. EDEN_NX2_UPLOAD_READ_BOUNDS=0 restores the whole-buffer read.
        static const bool bound_read = [] {
            const char* value = std::getenv("EDEN_NX2_UPLOAD_READ_BOUNDS");
            return value == nullptr || value[0] != '0';
        }();
        // The staging buffer was sized by MapSizeBytes, which is 32-bit.
        const size_t read_size =
            bound_read ? static_cast<u32>(image.guest_size_bytes) : mapped_span.size_bytes();
        ASSERT(read_size <= mapped_span.size_bytes());
        gpu_memory->ReadBlock(gpu_addr, mapped_span.data(), read_size,
                              VideoCommon::CacheType::NoTextureCache);
        const auto uploads = FullUploadSwizzles(image.info);
        runtime.AccelerateImageUpload(image, staging, uploads, 0, 0);
        return;
    }

    Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::UnsafeRead> swizzle_data(
        *gpu_memory, gpu_addr, image.guest_size_bytes, &swizzle_data_buffer);
    if (True(image.flags & ImageFlagBits::Converted)) {
        unswizzle_data_buffer.resize_destructive(image.unswizzled_size_bytes);
        auto copies = UnswizzleImage(*gpu_memory, gpu_addr, image.info, swizzle_data,
                                     unswizzle_data_buffer, &unswizzle_worker);
        ConvertImage(unswizzle_data_buffer, image.info, mapped_span, copies);
        image.UploadMemory(staging, copies);
    } else {
        const auto copies = UnswizzleImage(*gpu_memory, gpu_addr, image.info, swizzle_data,
                                           mapped_span, &unswizzle_worker);
        image.UploadMemory(staging, copies);
    }
}

template <class P>
ImageViewId TextureCache<P>::CreateImageView(const TICEntry& raw_config) {
    const bool native = gpu_memory->UsesNativeTiling();
    if (native && raw_config.BaseLayer() && NativeDrawDiagnostics()) {
        static u32 texture_view_traces{};
        if (texture_view_traces++ < 8) {
            LOG_WARNING(HW_GPU, "Native texture view header: address={:#x}, dimensions={}x{}x{}, type={}, legacy_layer={}, raw={:016x}/{:016x}/{:016x}/{:016x}",
                        raw_config.Address(), raw_config.Width(), raw_config.Height(), raw_config.Depth(),
                        static_cast<u32>(raw_config.texture_type.Value()), raw_config.BaseLayer(),
                        raw_config.raw[0], raw_config.raw[1], raw_config.raw[2], raw_config.raw[3]);
        }
    }
    const TICEntry config = TextureViewHeader(raw_config, native);
    const ImageInfo info(config);
    if (info.type == ImageType::Buffer) {
        const ImageViewInfo view_info(config, 0);
        return slot_image_views.insert(runtime, info, view_info, config.Address());
    }
    const u32 layer_offset = config.BaseLayer() * info.layer_stride;
    const GPUVAddr image_gpu_addr = config.Address() - layer_offset;
    const ImageId image_id = FindOrInsertImage(info, image_gpu_addr);
    if (!image_id) {
        return NULL_IMAGE_VIEW_ID;
    }
    ImageBase& image = slot_images[image_id];
    const SubresourceBase base = image.TryFindBase(config.Address()).value();
    ASSERT(base.level == 0);
    const ImageViewInfo view_info(config, base.layer);
    const ImageViewId image_view_id = FindOrEmplaceImageView(image_id, view_info);
    ImageViewBase& image_view = slot_image_views[image_view_id];
    image_view.flags |= ImageViewFlagBits::Strong;
    image.flags |= ImageFlagBits::Strong;
    return image_view_id;
}

template <class P>
ImageId TextureCache<P>::FindOrInsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                           RelaxedOptions options) {
    if (const ImageId image_id = FindImage(info, gpu_addr, options); image_id) {
        return image_id;
    }
    return InsertImage(info, gpu_addr, options);
}

template <class P>
ImageId TextureCache<P>::FindImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                   RelaxedOptions options) {
    ImageInfo storage_info = info;
    storage_info.native_tiling = info.UsesNativeSectorLayout(gpu_memory->UsesNativeTiling());
    const std::optional<DAddr> cpu_addr = ImageCpuAddress(gpu_addr, CalculateGuestSizeInBytes(info));
    if (!cpu_addr) {
        return ImageId{};
    }
    const bool broken_views =
        runtime.HasBrokenTextureViewFormats() || True(options & RelaxedOptions::ForceBrokenViews);
    const bool native_bgr = runtime.HasNativeBgr();
    const bool flexible_formats = True(options & RelaxedOptions::Format);
    ImageId image_id{};
    boost::container::small_vector<ImageId, 8> image_ids;
    const auto lambda = [&](ImageId existing_image_id, ImageBase& existing_image) {
        if (True(existing_image.flags & ImageFlagBits::Remapped)) {
            return false;
        }
        if (info.type == ImageType::Linear || existing_image.info.type == ImageType::Linear)
            [[unlikely]] {
            const bool strict_size = False(options & RelaxedOptions::Size) &&
                                     True(existing_image.flags & ImageFlagBits::Strong);
            const ImageInfo& existing = existing_image.info;
            if (existing_image.gpu_addr == gpu_addr && existing.type == info.type &&
                existing.pitch == info.pitch &&
                IsPitchLinearSameSize(existing, info, strict_size) &&
                IsViewCompatible(existing.format, info.format, broken_views, native_bgr)) {
                image_id = existing_image_id;
                image_ids.push_back(existing_image_id);
                return !flexible_formats && existing.format == info.format;
            }
        } else if (IsSubresource(storage_info, existing_image, gpu_addr, options, broken_views,
                                 native_bgr)) {
            // An image of unknown layout (a render target) viewed through a descriptor that
            // names one decodes guest memory in that order from now on. Images holding GPU-written
            // data keep theirs, so their uploads and writebacks stay in one order.
            if (False(existing_image.flags & ImageFlagBits::GpuModified) &&
                existing_image.info.AdoptSectorLayout(storage_info)) {
                existing_image.flags |= ImageFlagBits::CpuModified;
                existing_image.dirty_layers.clear();
                existing_image.dirty_ranges.clear();
                if (True(existing_image.flags & ImageFlagBits::Tracked)) {
                    UntrackImage(existing_image, existing_image_id);
                }
            }
            image_id = existing_image_id;
            image_ids.push_back(existing_image_id);
            return !flexible_formats && existing_image.info.format == info.format;
        }
        return false;
    };
    ForEachImageInRegion(*cpu_addr, CalculateGuestSizeInBytes(info), lambda);
    if (image_ids.size() <= 1) [[likely]] {
        return image_id;
    }
    auto image_ids_compare = [this](ImageId a, ImageId b) {
        auto& image_a = slot_images[a];
        auto& image_b = slot_images[b];
        return image_a.modification_tick < image_b.modification_tick;
    };
    return *std::ranges::max_element(image_ids, image_ids_compare);
}

template <class P>
bool TextureCache<P>::ImageCanRescale(ImageBase& image) {
    if (!image.info.rescaleable) {
        return false;
    }
    // EDEN_NX2_NO_RESCALE=<W>x<H>[,<W>x<H>...]: images of these guest sizes stay at 1x under a
    // resolution scale, and so do the render passes they are bound in. At 2x Splatoon Raiders'
    // 2048x2048 shadow maps became 4096x4096 and its 896x880 ink map 1792x1760.
    static const std::vector<std::pair<u32, u32>> no_rescale = [] {
        std::vector<std::pair<u32, u32>> sizes;
        const char* const value = std::getenv("EDEN_NX2_NO_RESCALE");
        if (value == nullptr) {
            return sizes;
        }
        std::string_view list{value};
        while (!list.empty()) {
            const size_t comma = list.find(',');
            const std::string_view item = list.substr(0, comma);
            list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
            const size_t x = item.find_first_of("xX");
            if (x == std::string_view::npos) {
                continue;
            }
            u32 width = 0;
            u32 height = 0;
            std::from_chars(item.data(), item.data() + x, width);
            std::from_chars(item.data() + x + 1, item.data() + item.size(), height);
            if (width != 0 && height != 0) {
                sizes.emplace_back(width, height);
            }
        }
        std::string names;
        for (const auto& [width, height] : sizes) {
            names += fmt::format("{}{}x{}", names.empty() ? "" : ", ", width, height);
        }
        LOG_INFO(HW_GPU, "Resolution scale leaves images of {} at 1x (EDEN_NX2_NO_RESCALE)",
                 names.empty() ? "no size" : names);
        return sizes;
    }();
    for (const auto& [width, height] : no_rescale) {
        if (image.info.size.width == width && image.info.size.height == height) {
            return false;
        }
    }
    // EDEN_NX2_NO_RESCALE_ABOVE=<n>: images wider or taller than n texels stay at 1x (4096 for
    // Splatoon Raiders). Its raid draws into 6400x6400 targets (78-156 MiB each at 1x); at 2x they
    // became 12800x12800, pushed a 6 GB card to 7.8 GB of video memory and lost the device.
    static Common::Nx2::SessionValue<u32> no_rescale_above{[] {
        const std::string_view text = Common::Nx2::Setting("EDEN_NX2_NO_RESCALE_ABOVE", "0");
        u32 limit = 0;
        std::from_chars(text.data(), text.data() + text.size(), limit);
        if (limit != 0) {
            LOG_INFO(HW_GPU,
                     "Resolution scale leaves images over {} texels wide or tall at 1x "
                     "(EDEN_NX2_NO_RESCALE_ABOVE)",
                     limit);
        }
        return limit;
    }};
    const u32 above = no_rescale_above.Get();
    if (above != 0 && (image.info.size.width > above || image.info.size.height > above)) {
        return false;
    }
    if (Settings::values.resolution_info.downscale && !image.info.downscaleable) {
        return false;
    }
    if (True(image.flags & (ImageFlagBits::Rescaled | ImageFlagBits::CheckingRescalable))) {
        return true;
    }
    if (True(image.flags & ImageFlagBits::IsRescalable)) {
        return true;
    }
    image.flags |= ImageFlagBits::CheckingRescalable;
    for (const auto& alias : image.aliased_images) {
        Image& other_image = slot_images[alias.id];
        if (!ImageCanRescale(other_image)) {
            image.flags &= ~ImageFlagBits::CheckingRescalable;
            return false;
        }
    }
    image.flags &= ~ImageFlagBits::CheckingRescalable;
    image.flags |= ImageFlagBits::IsRescalable;
    return true;
}

template <class P>
void TextureCache<P>::InvalidateScale(Image& image) {
    if (image.scale_tick <= frame_tick) {
        image.scale_tick = frame_tick + 1;
    }
    DropImageViews(image);
}

template <class P>
void TextureCache<P>::DropImageViews(Image& image) {
    // The image's views are deleted below, and delta draws may still hold one of them.
    VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::Invalidation);
    const std::span<const ImageViewId> image_view_ids = image.image_view_ids;
    auto& dirty = maxwell3d->dirty.flags;
    dirty[Dirty::RenderTargets] = true;
    dirty[Dirty::ZetaBuffer] = true;
    for (size_t rt = 0; rt < NUM_RT; ++rt) {
        dirty[Dirty::ColorBuffer0 + rt] = true;
    }
    for (const ImageViewId image_view_id : image_view_ids) {
        std::ranges::replace(render_targets.color_buffer_ids, image_view_id, ImageViewId{});
        if (render_targets.depth_buffer_id == image_view_id) {
            render_targets.depth_buffer_id = ImageViewId{};
        }
    }
    RemoveImageViewReferences(image_view_ids);
    RemoveFramebuffers(image_view_ids);
    for (const ImageViewId image_view_id : image_view_ids) {
        sentenced_image_view.Push(std::move(slot_image_views[image_view_id]));
        slot_image_views.erase(image_view_id);
    }
    image.image_view_ids.clear();
    image.image_view_infos.clear();
    for (size_t c : active_channel_ids) {
        auto& channel_info = channel_storage[c];

#ifndef NDEBUG
        // Every id the image tables hold, for each deleted image: debug builds only. The tables
        // are invalidated below, so each entry is written again before it is read.
        if constexpr (ENABLE_VALIDATION) {
            std::ranges::fill(channel_info.graphics_image_table.bound_ids, CORRUPT_ID);
            std::ranges::fill(channel_info.compute_image_table.bound_ids, CORRUPT_ID);
        }
#endif
        channel_info.graphics_image_table.Invalidate();
        channel_info.compute_image_table.Invalidate();
    }
    has_deleted_images = true;
}

template <class P>
void TextureCache<P>::CompactImageBlocks() {
    if constexpr (requires(Image& image) { image.Relocate(std::span<const u64>{}); }) {
        // Evicting images frees no memory while other images keep their blocks: near the budget
        // a 6 GB card held 0.4-1 GB of VMA blocks no image used (Mario Kart World), and lower
        // thresholds only made more holes. Instead, move the images out of blocks the cache's
        // images fill at most halfway, emptiest first, into free space in fuller blocks (never
        // into a new block); an emptied block goes back to the driver. At most 32 MiB every 10
        // frames; a block that can't be emptied is left alone for 600 frames.
        static constexpr u64 Interval = 10;
        static constexpr u64 MaxBytes = 32_MiB;
        static constexpr u64 MinSlack = 128_MiB;
        static constexpr u64 RetryFrames = 600;
        if (frame_tick % Interval != 0 || !runtime.CompactsImageBlocks()) {
            return;
        }
        // Only within a quarter of the driver's budget: below it, new images fill the holes.
        const u64 budget = runtime.GetDeviceMemoryBudget();
        const u64 slack = runtime.GetDeviceMemorySlack();
        if (budget == 0 || slack < MinSlack ||
            runtime.GetDeviceMemoryUsage() + slack + budget / 4 < budget) {
            return;
        }
        struct BlockImages {
            u64 block_size{};
            u64 bytes{};
            std::vector<ImageId> images;
        };
        std::unordered_map<u64, BlockImages> blocks;
        for (const auto [image_id, image] : slot_images) {
            const auto block = image->MemoryBlock();
            if (block.block == 0 || block.block_size == 0) {
                continue;
            }
            BlockImages& entry = blocks[block.block];
            entry.block_size = block.block_size;
            entry.bytes += block.size;
            entry.images.push_back(image_id);
        }
        std::erase_if(compact_failed_blocks, [this](const auto& failed) {
            return frame_tick - failed.second >= RetryFrames;
        });
        std::vector<u64> avoid;
        std::vector<std::pair<double, u64>> candidates;
        for (const auto& [block, entry] : blocks) {
            if (entry.bytes * 2 > entry.block_size) {
                continue;
            }
            avoid.push_back(block);
            if (!compact_failed_blocks.contains(block)) {
                candidates.emplace_back(static_cast<double>(entry.bytes) /
                                            static_cast<double>(entry.block_size),
                                        block);
            }
        }
        std::ranges::sort(avoid);
        std::ranges::sort(candidates);
        const auto movable = [](const Image& image) {
            return True(image.flags & ImageFlagBits::Registered) &&
                   False(image.flags &
                         (ImageFlagBits::IsDecoding | ImageFlagBits::CollectorDownload |
                          ImageFlagBits::Rescaled | ImageFlagBits::Unbacked |
                          ImageFlagBits::UnwrittenSparse | ImageFlagBits::SparseLayers |
                          ImageFlagBits::SparseTiles)) &&
                   !image.HasScaled() && image.info.num_samples == 1;
        };
        bool cleared = false;
        u64 moved = 0;
        for (const auto& [fill, block] : candidates) {
            if (moved >= MaxBytes) {
                break;
            }
            const BlockImages& entry = blocks.at(block);
            if (!std::ranges::all_of(entry.images,
                                     [&](ImageId id) { return movable(slot_images[id]); })) {
                compact_failed_blocks.emplace(block, frame_tick);
                continue;
            }
            if (!cleared) {
                // A deferred clear realized after the copy would clear the old image.
                runtime.FlushDeferredClear();
                cleared = true;
            }
            bool emptied = true;
            for (const ImageId image_id : entry.images) {
                Image& image = slot_images[image_id];
                const u64 bytes = image.MemoryBlock().size;
                if (!image.Relocate(avoid)) {
                    ++compact_failures;
                    compact_failed_blocks.emplace(block, frame_tick);
                    emptied = false;
                    break;
                }
                DropImageViews(image);
                moved += bytes;
                compact_moved_bytes += bytes;
                ++compact_moved_images;
            }
            if (emptied) {
                ++compact_emptied_blocks;
            }
        }
    }
}

template <class P>
u64 TextureCache<P>::GetScaledImageSizeBytes(const Image& image) const {
    const u64 scale_up = static_cast<u64>(Settings::values.resolution_info.up_scale *
                                          Settings::values.resolution_info.up_scale);
    const u64 down_shift = static_cast<u64>(Settings::values.resolution_info.down_shift +
                                            Settings::values.resolution_info.down_shift);
    const u64 tentative_size = (ImageMemoryBytes(image) * scale_up) >> down_shift;
    const u64 fitted_size = Common::AlignUp(tentative_size, 1024);
    return fitted_size;
}

template <class P>
u64 TextureCache<P>::ImageMemoryBytes(const Image& image) const {
    if (True(image.flags & ImageFlagBits::Unbacked)) {
        // No device memory but what BindLayers gave a SparseLayers image: at its full size (GBs
        // for title 1's sparse terrain images), deleting it would look to the collector like
        // freeing that much.
        if constexpr (requires { image.SparseBoundBytes(); }) {
            return image.SparseBoundBytes();
        }
        return 0;
    }
    // The real allocation: the estimate below counts opaque ASTC (BC1) at the BC3 size and
    // ignores alignment, so evictions were credited with up to twice what they freed.
    if constexpr (requires { image.HostMemoryBytes(); }) {
        if (const u64 bytes = image.HostMemoryBytes(); bytes != 0) {
            return bytes;
        }
    }
    u64 tentative_size = (std::max<u64>)(image.guest_size_bytes, image.unswizzled_size_bytes);
    if ((IsPixelFormatASTC(image.info.format) &&
         True(image.flags & ImageFlagBits::AcceleratedUpload)) ||
        True(image.flags & ImageFlagBits::Converted)) {
        tentative_size = TranscodedAstcSize(tentative_size, image.info.format);
    }
    return Common::AlignUp(tentative_size, 1024);
}

template <class P>
void TextureCache<P>::QueueAsyncDecode(Image& image, ImageId image_id) {
    UNIMPLEMENTED_IF(False(image.flags & ImageFlagBits::Converted));
    LOG_DEBUG(HW_GPU, "Queuing async texture decode {:#x}", image.gpu_addr);

    image.flags |= ImageFlagBits::IsDecoding;
    ++async_decodes_queued;
    auto decode = std::make_unique<AsyncDecodeContext>();
    auto* decode_ptr = decode.get();
    decode->image_id = image_id;
    async_decodes.push_back(std::move(decode));

    std::vector<u8> local_unswizzle_data_buffer(image.unswizzled_size_bytes, 0);
    Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::UnsafeRead> swizzle_data(*gpu_memory, image.gpu_addr, image.guest_size_bytes, &swizzle_data_buffer);
    auto copies = UnswizzleImage(*gpu_memory, image.gpu_addr, image.info, swizzle_data, local_unswizzle_data_buffer);
    const size_t out_size = MapSizeBytes(image);
    if (False(image.flags & ImageFlagBits::ZeroFilled)) {
        // Until the decode lands (a second or more while title 1 streams ~40 textures a second)
        // a new image showed whatever its memory last held, often other textures. Zero blocks
        // read as opaque black (BC1) or transparent (BC3) instead.
        image.flags |= ImageFlagBits::ZeroFilled;
        auto zero_copies = copies;
        ConvertedLayout(image.info, zero_copies);
        auto staging = runtime.UploadStagingBuffer(out_size);
        std::memset(staging.mapped_span.data(), 0, out_size);
        image.UploadMemory(staging, zero_copies);
        runtime.InsertUploadMemoryBarrier();
    }

    auto func = [out_size, copies, info = image.info,
                 input = std::move(local_unswizzle_data_buffer),
                 async_decode = decode_ptr]() mutable {
        async_decode->decoded_data.resize_destructive(out_size);
        std::span copies_span{copies.data(), copies.size()};
        ConvertImage(input, info, async_decode->decoded_data, copies_span);

        // TODO: Do we need this lock?
        std::unique_lock lock{async_decode->mutex};
        async_decode->copies = std::move(copies);
        async_decode->complete = true;
    };
    texture_decode_worker.QueueWork(std::move(func));
}

template <class P>
void TextureCache<P>::QueueAsyncUnswizzle(Image& image, ImageId image_id) {
    if (True(image.flags & ImageFlagBits::IsDecoding)) {
        return;
    }

    image.flags |= ImageFlagBits::IsDecoding;

    unswizzle_queue.push_back({
        .image_id = image_id,
        .info = image.info
    });
}

template <class P>
void TextureCache<P>::TickAsyncDecode() {
    bool has_uploads{};
    auto i = async_decodes.begin();
    while (i != async_decodes.end()) {
        auto* async_decode = i->get();
        {
            // Release the lock before the context is erased below: unlocking it afterwards wrote
            // into freed memory (heap corruption, crashes minutes into title 1).
            std::unique_lock lock{async_decode->mutex};
            if (!async_decode->complete) {
                ++i;
                continue;
            }
        }
        if (async_decode->cancelled) {
            i = async_decodes.erase(i);
            continue;
        }
        Image& image = slot_images[async_decode->image_id];
        if (async_decode->decoded_data.size() > MapSizeBytes(image)) {
            LOG_ERROR(HW_GPU, "Async decode of {} bytes for a {}-byte image at {:#x}, dropped",
                      async_decode->decoded_data.size(), MapSizeBytes(image), image.gpu_addr);
            i = async_decodes.erase(i);
            continue;
        }
        auto staging = runtime.UploadStagingBuffer(MapSizeBytes(image));
        std::memcpy(staging.mapped_span.data(), async_decode->decoded_data.data(),
                    async_decode->decoded_data.size());
        image.UploadMemory(staging, async_decode->copies);
        image.flags &= ~ImageFlagBits::IsDecoding;
        has_uploads = true;
        i = async_decodes.erase(i);
    }
    if (has_uploads) {
        runtime.InsertUploadMemoryBarrier();
    }
}

template <class P>
void TextureCache<P>::TickAsyncUnswizzle() {
    if (unswizzle_queue.empty()) {
        return;
    }

    if(current_unswizzle_frame > 0) {
        current_unswizzle_frame--;
        return;
    }

    PendingUnswizzle& task = unswizzle_queue.front();
    Image& image = slot_images[task.image_id];

    if (!task.initialized) {
        task.total_size = MapSizeBytes(image);
        task.staging_buffer = runtime.UploadStagingBuffer(task.total_size, true);

        const auto& info = image.info;
        const u32 bytes_per_block = BytesPerBlock(info.format);
        const u32 width_blocks = Common::DivCeil(info.size.width, 4u);
        const u32 height_blocks = Common::DivCeil(info.size.height, 4u);

        const u32 stride = width_blocks * bytes_per_block;
        const u32 aligned_height = height_blocks;
        task.bytes_per_slice = static_cast<size_t>(stride) * aligned_height;
        task.last_submitted_offset = 0;
        task.initialized = true;
    }

    // Read data
    if (task.current_offset < task.total_size) {
        const size_t remaining = task.total_size - task.current_offset;

        size_t copy_amount = (std::min)(swizzle_chunk_size, remaining);

        if (remaining > swizzle_chunk_size) {
            copy_amount = (copy_amount / task.bytes_per_slice) * task.bytes_per_slice;
            if (copy_amount == 0) copy_amount = task.bytes_per_slice;
        }

        gpu_memory->ReadBlock(image.gpu_addr + task.current_offset,
                              task.staging_buffer.mapped_span.data() + task.current_offset,
                              copy_amount);
        task.current_offset += copy_amount;
    }

    const bool is_final_batch = task.current_offset >= task.total_size;
    const size_t bytes_ready = task.current_offset - task.last_submitted_offset;
    const u32 complete_slices = static_cast<u32>(bytes_ready / task.bytes_per_slice);

    if (complete_slices >= swizzle_slices_per_batch || (is_final_batch && complete_slices > 0)) {
        const u32 z_start = static_cast<u32>(task.last_submitted_offset / task.bytes_per_slice);
        const u32 slices_to_process = (std::min)(complete_slices, swizzle_slices_per_batch);
        const u32 z_count = (std::min)(slices_to_process, image.info.size.depth - z_start);

        if (z_count > 0) {
            const auto uploads = FullUploadSwizzles(task.info);
            runtime.AccelerateImageUpload(image, task.staging_buffer, uploads, z_start, z_count);
            task.last_submitted_offset += (static_cast<size_t>(z_count) * task.bytes_per_slice);
        }
    }

    // Check if complete
    const u32 slices_submitted = static_cast<u32>(task.last_submitted_offset / task.bytes_per_slice);
    const bool all_slices_submitted = slices_submitted >= image.info.size.depth;

    if (is_final_batch && all_slices_submitted) {
        runtime.FreeDeferredStagingBuffer(task.staging_buffer);
        image.flags &= ~ImageFlagBits::IsDecoding;
        unswizzle_queue.pop_front();

        // Wait 4 frames to process the next entry
        current_unswizzle_frame = 4u;
    }
}

template <class P>
bool TextureCache<P>::ScaleUp(Image& image) {
    const bool has_copy = image.HasScaled();
    const bool rescaled = image.ScaleUp();
    if (!has_copy && image.HasScaled()) {
        total_used_memory += GetScaledImageSizeBytes(image);
    }
    if (!rescaled) {
        return false;
    }
    InvalidateScale(image);
    return true;
}

template <class P>
bool TextureCache<P>::ScaleDown(Image& image) {
    const bool rescaled = image.ScaleDown();
    if (!rescaled) {
        return false;
    }
    InvalidateScale(image);
    return true;
}

template <class P>
std::optional<DAddr> TextureCache<P>::ImageCpuAddress(GPUVAddr gpu_addr, size_t size) const {
    if (const std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr)) {
        return cpu_addr;
    }
    if (const std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr, size)) {
        return cpu_addr;
    }
    // No CPU backing (title 1 renders a bloom chain at GPU 0xf0000-0x15b600): a stand-in address
    // fixed by the GPU address, at or above ~(1 << 40) so TrackImage skips it. A fresh address per
    // lookup made every use create another image (thousands a minute, never found again).
    constexpr GPUVAddr StandInRange = 1ULL << 40;
    if (gpu_addr >= StandInRange || size > StandInRange - gpu_addr - 1) {
        return std::nullopt;
    }
    return ~(1ULL << 40ULL) + gpu_addr;
}

template <class P>
ImageId TextureCache<P>::InsertImage(const ImageInfo& info, GPUVAddr gpu_addr,
                                     RelaxedOptions options) {
    if (IsImpossibleImage(info)) {
        // Register state from a desynced command stream (title 1 after a 4 s GPU stall: a
        // 2173701686x2147550710 image with 22552 layers): creating it threw and ended the process.
        static u32 skipped{};
        if (skipped++ < 4) {
            LOG_ERROR(HW_GPU, "Skipping impossible image at {:#x}: {}x{}x{}, {} levels, {} layers, "
                              "format {}",
                      gpu_addr, info.size.width, info.size.height, info.size.depth,
                      info.resources.levels, info.resources.layers, static_cast<u32>(info.format));
        }
        return ImageId{};
    }
    std::optional<DAddr> cpu_addr = ImageCpuAddress(gpu_addr, CalculateGuestSizeInBytes(info));
    if (!cpu_addr) {
        const DAddr fake_addr = ~(1ULL << 40ULL) + virtual_invalid_space;
        virtual_invalid_space += Common::AlignUp(CalculateGuestSizeInBytes(info), 32);
        cpu_addr = std::optional<DAddr>(fake_addr);
    }
    ASSERT_MSG(cpu_addr, "Tried to insert an image to an invalid gpu_addr={:#x}", gpu_addr);
    ImageInfo storage_info = info;
    storage_info.native_tiling = info.UsesNativeSectorLayout(gpu_memory->UsesNativeTiling());
    if (storage_info.native_tiling && info.type == ImageType::e3D) {
        // Switch 2 3D images (lighting/fog volumes) are rare: log each one's shape.
        LOG_DEBUG(HW_GPU, "3D image at {:#x}: {}x{}x{} levels {} format {} block {}x{}x{}",
                 gpu_addr, info.size.width, info.size.height, info.size.depth, info.resources.levels,
                 static_cast<u32>(info.format), info.block.width, info.block.height, info.block.depth);
    }
    const ImageId image_id = JoinImages(storage_info, gpu_addr, *cpu_addr);
    if (!image_id) {
        return image_id;
    }
    const Image& image = slot_images[image_id];
    // Using "image.gpu_addr" instead of "gpu_addr" is important because it might be different
    const auto [it, is_new] = image_allocs_table.try_emplace(image.gpu_addr);
    if (is_new) {
        it->second = slot_image_allocs.insert();
    }
    slot_image_allocs[it->second].images.push_back(image_id);
    return image_id;
}

template <class P>
ImageId TextureCache<P>::JoinImages(const ImageInfo& info, GPUVAddr gpu_addr, DAddr cpu_addr) {
    ImageInfo new_info = info;
    const size_t size_bytes = CalculateGuestSizeInBytes(new_info);
    const bool broken_views = runtime.HasBrokenTextureViewFormats();
    const bool native_bgr = runtime.HasNativeBgr();
    join_overlap_ids.clear();
    join_overlaps_found.clear();
    join_left_aliased_ids.clear();
    join_right_aliased_ids.clear();
    join_ignore_textures.clear();
    join_bad_overlap_ids.clear();
    join_copies_to_do.clear();
    join_alias_indices.clear();
    const bool this_is_linear = info.type == ImageType::Linear;
    const auto region_check = [&](ImageId overlap_id, ImageBase& overlap) {
        if (True(overlap.flags & ImageFlagBits::Remapped)) {
            join_ignore_textures.insert(overlap_id);
            return;
        }
        const bool overlap_is_linear = overlap.info.type == ImageType::Linear;
        if (this_is_linear != overlap_is_linear) {
            return;
        }
        if (this_is_linear && overlap_is_linear) {
            if (info.pitch == overlap.info.pitch && gpu_addr == overlap.gpu_addr) {
                // Alias linear images with the same pitch
                join_left_aliased_ids.push_back(overlap_id);
            }
            return;
        }
        join_overlaps_found.insert(overlap_id);
        constexpr bool strict_size = true;
        const std::optional<OverlapResult> solution = ResolveOverlap(
            new_info, gpu_addr, cpu_addr, overlap, strict_size, broken_views, native_bgr);
        if (solution) {
            gpu_addr = solution->gpu_addr;
            cpu_addr = solution->cpu_addr;
            new_info.resources = solution->resources;
            join_overlap_ids.push_back(overlap_id);
            join_copies_to_do.emplace_back(JoinCopy{false, overlap_id});
            return;
        }
        constexpr auto options = RelaxedOptions::Size | RelaxedOptions::Format;
        const ImageBase new_image_base(new_info, gpu_addr, cpu_addr);
        if (IsSubresource(new_info, overlap, gpu_addr, options, broken_views, native_bgr)) {
            join_left_aliased_ids.push_back(overlap_id);
            overlap.flags |= ImageFlagBits::Alias;
            join_copies_to_do.emplace_back(JoinCopy{true, overlap_id});
        } else if (IsSubresource(overlap.info, new_image_base, overlap.gpu_addr, options,
                                 broken_views, native_bgr)) {
            join_right_aliased_ids.push_back(overlap_id);
            overlap.flags |= ImageFlagBits::Alias;
            join_copies_to_do.emplace_back(JoinCopy{true, overlap_id});
        } else {
            join_bad_overlap_ids.push_back(overlap_id);
        }
    };
    ForEachImageInRegion(cpu_addr, size_bytes, region_check);
    const auto region_check_gpu = [&](ImageId overlap_id, ImageBase& overlap) {
        if (!join_overlaps_found.contains(overlap_id)) {
            if (True(overlap.flags & ImageFlagBits::Remapped)) {
                join_ignore_textures.insert(overlap_id);
            }
            if (overlap.gpu_addr == gpu_addr && overlap.guest_size_bytes == size_bytes) {
                join_ignore_textures.insert(overlap_id);
            }
        }
    };
    ForEachSparseImageInRegion(channel_state->gpu_memory.GetID(), gpu_addr, size_bytes,
                               region_check_gpu);

    bool can_rescale = info.rescaleable;
    bool any_rescaled = false;
    for (const auto& copy : join_copies_to_do) {
        if (!can_rescale) {
            break;
        }
        Image& sibling = slot_images[copy.id];
        can_rescale &= ImageCanRescale(sibling);
        any_rescaled |= True(sibling.flags & ImageFlagBits::Rescaled);
    }

    can_rescale &= any_rescaled;

    if (can_rescale) {
        for (const auto& copy : join_copies_to_do) {
            Image& sibling = slot_images[copy.id];
            ScaleUp(sibling);
        }
    } else {
        for (const auto& copy : join_copies_to_do) {
            Image& sibling = slot_images[copy.id];
            ScaleDown(sibling);
        }
    }

    if constexpr (requires { runtime.UseOpaqueAstcFormat(); }) {
        // Opaque ASTC goes to BC1 rather than BC3 when it is re-compressed (the Vulkan runtime
        // decides). The guest data is already there when an image is first used.
        constexpr size_t MaxScanBytes = 64_MiB;
        if (IsPixelFormatASTC(new_info.format) && runtime.UseOpaqueAstcFormat()) {
            const size_t scan_bytes = CalculateGuestSizeInBytes(new_info);
            if (scan_bytes <= MaxScanBytes) {
                // Scanned where it is when it is in one piece; only copied (into the scratch
                // buffer) when the range is split.
                Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::UnsafeRead>
                    scan_data(*gpu_memory, gpu_addr, scan_bytes, &astc_scan_buffer);
                new_info.astc_opaque =
                    AstcDataLooksOpaque(std::span<const u8>{scan_data.data(), scan_data.size()});
                ++(new_info.astc_opaque ? astc_opaque_images : astc_alpha_images);
            }
        }
    }
    const ImageId new_image_id = slot_images.insert(runtime, new_info, gpu_addr, cpu_addr);
    if constexpr (requires(const Image& image) { image.OutOfMemory(); }) {
        if (slot_images[new_image_id].OutOfMemory()) {
            // Left out of the cache: lookups see no image (a null view) and make it again once
            // the collector has freed memory.
            slot_images.erase(new_image_id);
            static u32 logged{};
            if (logged++ < 32) {
                LOG_ERROR(HW_GPU,
                          "No memory for the {}x{}x{} image at {:#x} (format {}); skipping it",
                          new_info.size.width, new_info.size.height, new_info.size.depth, gpu_addr,
                          static_cast<u32>(new_info.format));
            }
            return ImageId{};
        }
    }
    Image& new_image = slot_images[new_image_id];

    new_image.allocation_tick = frame_tick;
    {
        // Which shapes are being created (periodic log): format, type, levels, layers, size.
        const u64 key = (u64{static_cast<u32>(new_info.format)} << 56) |
                        (u64{static_cast<u32>(new_info.type)} << 52) |
                        (u64{static_cast<u32>(new_info.resources.levels) & 0xF} << 48) |
                        (u64{static_cast<u32>(new_info.resources.layers) & 0xFFF} << 36) |
                        (u64{new_info.size.width & 0x3FFFF} << 18) | (new_info.size.height & 0x3FFFF);
        auto& shape = created_shapes[key];
        ++shape.first;
        shape.second = gpu_addr;
        ++images_created;
    }

    if (!gpu_memory->IsContinuousRange(new_image.gpu_addr, new_image.guest_size_bytes) &&
        new_info.is_sparse) {
        new_image.flags |= ImageFlagBits::Sparse;
    }

    for (const ImageId overlap_id : join_ignore_textures) {
        Image& overlap = slot_images[overlap_id];
        // Remapped images no longer describe the current guest memory: their GPU contents are
        // discarded rather than downloaded through the replacement mapping.
        if (True(overlap.flags & ImageFlagBits::GpuModified) &&
            False(overlap.flags & ImageFlagBits::Remapped)) {
            UNIMPLEMENTED();
        }
        if (True(overlap.flags & ImageFlagBits::Tracked)) {
            UntrackImage(overlap, overlap_id);
        }
        UnregisterImage(overlap_id);
        DeleteImage(overlap_id);
    }

    std::ranges::sort(join_copies_to_do, [this](const JoinCopy& lhs, const JoinCopy& rhs) {
        const ImageBase& lhs_image = slot_images[lhs.id];
        const ImageBase& rhs_image = slot_images[rhs.id];
        return lhs_image.modification_tick < rhs_image.modification_tick;
    });

    ImageBase& new_image_base = new_image;
    for (const ImageId aliased_id : join_right_aliased_ids) {
        ImageBase& aliased = slot_images[aliased_id];
        size_t alias_index = new_image_base.aliased_images.size();
        if (!AddImageAlias(new_image_base, aliased, new_image_id, aliased_id)) {
            continue;
        }
        join_alias_indices.emplace(aliased_id, alias_index);
        new_image.flags |= ImageFlagBits::Alias;
    }
    for (const ImageId aliased_id : join_left_aliased_ids) {
        ImageBase& aliased = slot_images[aliased_id];
        size_t alias_index = new_image_base.aliased_images.size();
        if (!AddImageAlias(aliased, new_image_base, aliased_id, new_image_id)) {
            continue;
        }
        join_alias_indices.emplace(aliased_id, alias_index);
        new_image.flags |= ImageFlagBits::Alias;
    }
    for (const ImageId aliased_id : join_bad_overlap_ids) {
        ImageBase& aliased = slot_images[aliased_id];
        aliased.overlapping_images.push_back(new_image_id);
        new_image.overlapping_images.push_back(aliased_id);
        if (aliased.info.resources.levels == 1 && aliased.info.block.depth == 0 &&
            aliased.overlapping_images.size() > 1) {
            aliased.flags |= ImageFlagBits::BadOverlap;
        }
        if (new_image.info.resources.levels == 1 && new_image.info.block.depth == 0 &&
            new_image.overlapping_images.size() > 1) {
            new_image.flags |= ImageFlagBits::BadOverlap;
        }
    }

    // The copy from an alias into the new image, or null when the loop below skips it.
    const auto alias_copy = [&](const JoinCopy& copy) -> const AliasedImage* {
        if (!copy.is_alias) {
            return nullptr;
        }
        const Image& overlap = slot_images[copy.id];
        if (!overlap.IsSafeGpuCopy() || overlap.info.num_samples != new_image.info.num_samples) {
            return nullptr;
        }
        const auto alias_pointer = join_alias_indices.find(copy.id);
        if (alias_pointer == join_alias_indices.end()) {
            return nullptr;
        }
        return &new_image.aliased_images[alias_pointer->second];
    };
    constexpr ImageFlagBits deferred_upload_excluded =
        ImageFlagBits::Sparse | ImageFlagBits::Unbacked | ImageFlagBits::UnwrittenSparse |
        ImageFlagBits::SparseLayers | ImageFlagBits::SparseTiles |
        ImageFlagBits::AsynchronousDecode | ImageFlagBits::TrimmedLevels;
    // Whether that copy writes every texel of the new image: a plain copy (same 2D format) of every
    // level and layer from its origin, between images with all their host memory. Alias copies
    // count a compressed extent in blocks but the backends copy it as texels, so a compressed
    // copy writes only part of the image.
    constexpr ImageFlagBits source_excluded =
        ImageFlagBits::Rescaled | ImageFlagBits::TrimmedLevels | ImageFlagBits::Unbacked |
        ImageFlagBits::UnwrittenSparse | ImageFlagBits::SparseLayers | ImageFlagBits::SparseTiles;
    const auto plain_copy_source = [&](const Image& source) {
        return new_image.info.type == ImageType::e2D && source.info.type == ImageType::e2D &&
               source.info.format == new_image.info.format &&
               source.info.astc_opaque == new_image.info.astc_opaque &&
               False(new_image.flags & (deferred_upload_excluded | ImageFlagBits::Rescaled)) &&
               False(source.flags & source_excluded) &&
               VideoCore::Surface::DefaultBlockWidth(new_image.info.format) == 1 &&
               VideoCore::Surface::DefaultBlockHeight(new_image.info.format) == 1;
    };
    const auto writes_whole_image = [&](const AliasedImage& aliased) {
        if (!plain_copy_source(slot_images[aliased.id])) {
            return false;
        }
        for (s32 level = 0; level < new_image.info.resources.levels; ++level) {
            const Extent3D size = MipSize(new_image.info.size, level);
            const bool covered = std::ranges::any_of(aliased.copies, [&](const ImageCopy& copy) {
                return copy.dst_subresource.base_level == level &&
                       copy.dst_subresource.base_layer == 0 &&
                       copy.dst_subresource.num_layers >= new_image.info.resources.layers &&
                       copy.dst_offset == Offset3D{} && copy.extent.width >= size.width &&
                       copy.extent.height >= size.height;
            });
            if (!covered) {
                return false;
            }
        }
        return true;
    };
    // Copies run oldest first. When one of them writes the whole image, the upload from guest
    // memory and the alias copies before it would be overwritten whole: neither is made. Ocarina
    // of Time resizes a shadow map every frame in parts of Kokiri Forest, at the same address:
    // each size is a new depth target and a new texture, both copied from older sizes, and their
    // uploads took up to 17% of the GPU back thread there. Only at 1x: a rescaled copy's extent is
    // the scaled guest mip size, which can fall short of the host mip (a 5 px wide image at 2x:
    // host mip 1 is 5 px, the copy 4).
    size_t first_copy = 0;
    bool copied_whole = false;
    for (size_t index = join_copies_to_do.size(); !can_rescale && index-- > 0;) {
        const AliasedImage* const aliased = alias_copy(join_copies_to_do[index]);
        if (aliased != nullptr && writes_whole_image(*aliased)) {
            first_copy = index;
            copied_whole = true;
            break;
        }
    }
    // EDEN_NX2_TC_JOIN_LAYERS=1: the copies from overlaps the GPU wrote (made below, after the
    // upload) can each write a part of the image, together all of it: counted per level and
    // layer, from the same plain copies. Mario Kart World renders each layer of its shadow map
    // and colour arrays as a target of its own, then samples the array, and the upload under
    // those copies took 13-31 ms an array in Lichii's logs.
    static Common::Nx2::SessionValue<bool> join_layers{
        [] { return Common::Nx2::Setting("EDEN_NX2_TC_JOIN_LAYERS", "0") == "1"; }};
    if (!can_rescale && !copied_whole && new_info.num_samples == 1 && join_layers.Get()) {
        const s32 levels = new_info.resources.levels;
        const s32 layers = new_info.resources.layers;
        std::vector<u8> written(static_cast<size_t>(levels * layers));
        for (const JoinCopy& join_copy : join_copies_to_do) {
            const Image& overlap = slot_images[join_copy.id];
            if (join_copy.is_alias || False(overlap.flags & ImageFlagBits::GpuModified) ||
                overlap.info.num_samples != 1 || !plain_copy_source(overlap)) {
                continue;
            }
            const SubresourceBase base = new_image.TryFindBase(overlap.gpu_addr).value();
            for (const ImageCopy& layer_copy :
                 MakeShrinkImageCopies(new_info, overlap.info, base, 1, 0)) {
                const s32 level = layer_copy.dst_subresource.base_level;
                if (level < 0 || level >= levels) {
                    continue;
                }
                const Extent3D size = MipSize(new_info.size, level);
                if (layer_copy.dst_offset != Offset3D{} || layer_copy.extent.width < size.width ||
                    layer_copy.extent.height < size.height) {
                    continue;
                }
                const s32 first = (std::max)(layer_copy.dst_subresource.base_layer, 0);
                const s32 last = (std::min)(layer_copy.dst_subresource.base_layer +
                                                layer_copy.dst_subresource.num_layers,
                                            layers);
                for (s32 layer = first; layer < last; ++layer) {
                    written[static_cast<size_t>(level * layers + layer)] = 1;
                }
            }
        }
        copied_whole = std::ranges::all_of(written, [](u8 whole) { return whole != 0; });
    }
    // EDEN_NX2_RT_FRESH: a new render target starts undefined instead of loading guest memory.
    // Ocarina of Time places its post-processing targets at new addresses as the camera moves,
    // where guest memory only holds stale bytes, and its passes draw the targets whole.
    // 1: only targets nothing cached overlaps. 2: also those overlapping older images, whose
    // data is still copied in below.
    const bool fresh_render_target = [&] {
        static const int mode = [] {
            const char* value = std::getenv("EDEN_NX2_RT_FRESH");
            return value != nullptr ? std::atoi(value) : 0;
        }();
        if (mode == 0 || !creating_render_target || new_info.type != ImageType::e2D ||
            new_info.resources.levels != 1 || new_info.resources.layers != 1 ||
            new_info.num_samples != 1 || True(new_image.flags & deferred_upload_excluded)) {
            return false;
        }
        return mode >= 2 || (join_copies_to_do.empty() && join_bad_overlap_ids.empty());
    }();
    if (creating_render_target) {
        ++render_targets_created;
    }
    if (clearing_render_targets && join_copies_to_do.empty() &&
        new_info.type == ImageType::e2D && new_info.resources.levels == 1 &&
        new_info.resources.layers == 1 && new_info.num_samples == 1 &&
        False(new_image.flags & deferred_upload_excluded)) {
        // A new render target of the clear being set up: it stays CpuModified, and
        // UpdateRenderTargets uploads it unless the clear overwrites all of it.
        runtime.TransitionImageLayout(new_image);
        clear_deferred_uploads.push_back(new_image_id);
    } else if (copied_whole || fresh_render_target) {
        if (!copied_whole) {
            ++fresh_render_targets;
        }
        // What RefreshContents does, without the upload.
        new_image.flags &= ~ImageFlagBits::CpuModified;
        if (False(new_image.flags & ImageFlagBits::Tracked)) {
            TrackImage(new_image, new_image_id);
        }
        new_image.dirty_layers.clear();
        runtime.TransitionImageLayout(new_image);
    } else {
        if (creating_render_target && True(new_image.flags & ImageFlagBits::CpuModified)) {
            ++render_targets_uploaded;
        }
        // TODO: Only upload what we need
        RefreshContents(new_image, new_image_id);
    }

    if (can_rescale) {
        ScaleUp(new_image);
    } else {
        ScaleDown(new_image);
    }

    for (size_t index = 0; index < join_copies_to_do.size(); ++index) {
        const JoinCopy& copy_object = join_copies_to_do[index];
        Image& overlap = slot_images[copy_object.id];
        if (copy_object.is_alias) {
            const AliasedImage* const aliased =
                index >= first_copy ? alias_copy(copy_object) : nullptr;
            if (aliased == nullptr) {
                continue;
            }
            CopyImage(new_image_id, aliased->id, aliased->copies);
            new_image.modification_tick = overlap.modification_tick;
            continue;
        }
        if (True(overlap.flags & ImageFlagBits::GpuModified)) {
            new_image.flags |= ImageFlagBits::GpuModified;
            const auto& resolution = Settings::values.resolution_info;
            const SubresourceBase base = new_image.TryFindBase(overlap.gpu_addr).value();
            const u32 up_scale = can_rescale ? resolution.up_scale : 1;
            const u32 down_shift = can_rescale ? resolution.down_shift : 0;
            auto copies = MakeShrinkImageCopies(new_info, overlap.info, base, up_scale, down_shift);
            if (overlap.info.num_samples != new_image.info.num_samples) {
                runtime.CopyImageMSAA(new_image, overlap, copies);
            } else {
                runtime.CopyImage(new_image, overlap, copies);
            }
            new_image.modification_tick = overlap.modification_tick;
        }
        if (True(overlap.flags & ImageFlagBits::Tracked)) {
            UntrackImage(overlap, copy_object.id);
        }
        UnregisterImage(copy_object.id);
        DeleteImage(copy_object.id);
    }

    RegisterImage(new_image_id);
    return new_image_id;
}

template <class P>
std::optional<typename TextureCache<P>::BlitImages> TextureCache<P>::GetBlitImages(
    const Tegra::Engines::Fermi2D::Surface& dst, const Tegra::Engines::Fermi2D::Surface& src,
    const Tegra::Engines::Fermi2D::Config& copy) {

    constexpr auto FIND_OPTIONS = RelaxedOptions::Samples;
    const GPUVAddr dst_addr = dst.Address();
    const GPUVAddr src_addr = src.Address();
    ImageInfo dst_info(dst);
    ImageInfo src_info(src);
    const bool can_be_depth_blit =
        dst_info.format == src_info.format && copy.filter == Tegra::Engines::Fermi2D::Filter::Point;
    ImageId dst_id;
    ImageId src_id;
    RelaxedOptions try_options = FIND_OPTIONS;
    // A packed RGB9E5 frame is written as R32_UINT storage: read it through an RGB9E5 view.
    if (src_info.format == PixelFormat::E5B9G9R9_FLOAT) {
        try_options |= RelaxedOptions::Format;
    }
    if (can_be_depth_blit) {
        try_options |= RelaxedOptions::Format;
    }
    do {
        has_deleted_images = false;
        src_id = FindImage(src_info, src_addr, try_options);
        dst_id = FindImage(dst_info, dst_addr, try_options);
        if (!copy.must_accelerate) {
            do {
                if (!src_id && !dst_id) {
                    return std::nullopt;
                }
                if (src_id && True(slot_images[src_id].flags & ImageFlagBits::GpuModified)) {
                    break;
                }
                if (dst_id && True(slot_images[dst_id].flags & ImageFlagBits::GpuModified)) {
                    break;
                }
                return std::nullopt;
            } while (false);
        }
        const ImageBase* const src_image = src_id ? &slot_images[src_id] : nullptr;
        if (src_image && src_image->info.num_samples > 1) {
            RelaxedOptions find_options{FIND_OPTIONS | RelaxedOptions::ForceBrokenViews};
            src_id = FindOrInsertImage(src_info, src_addr, find_options);
            dst_id = FindOrInsertImage(dst_info, dst_addr, find_options);
            if (has_deleted_images) {
                continue;
            }
            break;
        }
        if (can_be_depth_blit) {
            const ImageBase* const dst_image = dst_id ? &slot_images[dst_id] : nullptr;
            DeduceBlitImages(dst_info, src_info, dst_image, src_image);
            if (GetFormatType(dst_info.format) != GetFormatType(src_info.format)) {
                continue;
            }
        }
        if (!src_id) {
            src_id = InsertImage(src_info, src_addr, RelaxedOptions{});
        }
        if (!dst_id) {
            dst_id = InsertImage(dst_info, dst_addr, RelaxedOptions{});
        }
    } while (has_deleted_images);
    if (!src_id || !dst_id) {
        return std::nullopt;
    }
    const ImageBase& src_image = slot_images[src_id];
    const ImageBase& dst_image = slot_images[dst_id];
    const bool native_bgr = runtime.HasNativeBgr();
    if (GetFormatType(dst_info.format) != GetFormatType(dst_image.info.format) ||
        GetFormatType(src_info.format) != GetFormatType(src_image.info.format) ||
        !VideoCore::Surface::IsViewCompatible(dst_info.format, dst_image.info.format, false,
                                              native_bgr) ||
        !VideoCore::Surface::IsViewCompatible(src_info.format, src_image.info.format, false,
                                              native_bgr)) {
        // Make sure the images match the expected format.
        do {
            has_deleted_images = false;
            src_id = FindOrInsertImage(src_info, src_addr, RelaxedOptions{});
            dst_id = FindOrInsertImage(dst_info, dst_addr, RelaxedOptions{});
        } while (has_deleted_images);
        if (!src_id || !dst_id) {
            return std::nullopt;
        }
    }
    return {BlitImages{
        .dst_id = dst_id,
        .src_id = src_id,
        .dst_format = dst_info.format,
        .src_format = src_info.format,
    }};
}

template <class P>
ImageId TextureCache<P>::FindDMAImage(const ImageInfo& info, GPUVAddr gpu_addr) {
    const std::optional<DAddr> cpu_addr = ImageCpuAddress(gpu_addr, CalculateGuestSizeInBytes(info));
    if (!cpu_addr) {
        return ImageId{};
    }
    ImageId image_id{};
    boost::container::small_vector<ImageId, 8> image_ids;
    const auto lambda = [&](ImageId existing_image_id, ImageBase& existing_image) {
        if (True(existing_image.flags & ImageFlagBits::Remapped)) {
            return false;
        }
        if (info.type == ImageType::Linear || existing_image.info.type == ImageType::Linear)
            [[unlikely]] {
            const bool strict_size = True(existing_image.flags & ImageFlagBits::Strong);
            const ImageInfo& existing = existing_image.info;
            if (existing_image.gpu_addr == gpu_addr && existing.type == info.type &&
                existing.pitch == info.pitch &&
                IsPitchLinearSameSize(existing, info, strict_size) &&
                IsViewCompatible(existing.format, info.format, false, true)) {
                image_id = existing_image_id;
                image_ids.push_back(existing_image_id);
                return true;
            }
        } else if (IsSubCopy(info, existing_image, gpu_addr)) {
            if (True(existing_image.flags & ImageFlagBits::Unbacked)) {
                static u32 logged{};
                if (logged++ < 64) {
                    LOG_INFO(HW_GPU, "DMA copy at {:#x} ({}x{}) into unbacked image {:#x}", gpu_addr,
                             info.size.width, info.size.height, existing_image.gpu_addr);
                }
            }
            image_id = existing_image_id;
            image_ids.push_back(existing_image_id);
            return true;
        }
        return false;
    };
    ForEachImageInRegion(*cpu_addr, CalculateGuestSizeInBytes(info), lambda);
    if (image_ids.size() <= 1) [[likely]] {
        return image_id;
    }
    auto image_ids_compare = [this](ImageId a, ImageId b) {
        auto& image_a = slot_images[a];
        auto& image_b = slot_images[b];
        return image_a.modification_tick < image_b.modification_tick;
    };
    return *std::ranges::max_element(image_ids, image_ids_compare);
}

template <class P>
std::pair<u32, u32> TextureCache<P>::PrepareDmaImage(ImageId dst_id, GPUVAddr base_addr,
                                                     bool mark_as_modified) {
    // As in BlitImage: land a parked deferred clear before the DMA write, but only when the
    // write happens (mark_as_modified); a read-only access writes nothing.
    if (mark_as_modified) {
        runtime.FlushDeferredClear();
    }
    const auto& image = slot_images[dst_id];
    const auto base = image.TryFindBase(base_addr);
    PrepareImage(dst_id, mark_as_modified, false);
    const auto& new_image = slot_images[dst_id];
    lru_cache.Touch(new_image.lru_index, frame_tick);
    return std::make_pair(base->level, base->layer);
}

template <class P>
SamplerId TextureCache<P>::FindSampler(const TSCEntry& config, bool compute) {
    if (std::ranges::all_of(config.raw, [](u64 value) { return value == 0; })) {
        return NULL_SAMPLER_ID;
    }
    const auto [pair, is_new] = channel_state->samplers.try_emplace(config);
    if (is_new) {
        pair->second = slot_samplers.insert(runtime, config);
    }
    return pair->second;
}

template <class P>
ImageViewId TextureCache<P>::FindColorBuffer(size_t index) {
    const auto& regs = maxwell3d->regs;
    if (index >= regs.rt_control.count) {
        return ImageViewId{};
    }
    const auto& rt = regs.rt[index];
    const GPUVAddr gpu_addr = rt.Address();
    if (gpu_addr == 0) {
        return ImageViewId{};
    }
    if (rt.format == Tegra::RenderTargetFormat::NONE) {
        return ImageViewId{};
    }
    const ImageInfo info(regs.rt[index], regs.anti_alias_samples_mode);
    return FindRenderTargetView(info, gpu_addr);
}

template <class P>
ImageViewId TextureCache<P>::FindDepthBuffer() {
    const auto& regs = maxwell3d->regs;
    if (!regs.zeta_enable) {
        return ImageViewId{};
    }
    const GPUVAddr gpu_addr = regs.zeta.Address();
    if (gpu_addr == 0) {
        return ImageViewId{};
    }
    const ImageInfo info(regs.zeta, regs.zeta_size, regs.anti_alias_samples_mode);
    return FindRenderTargetView(info, gpu_addr);
}

template <class P>
ImageViewId TextureCache<P>::FindRenderTargetView(const ImageInfo& info, GPUVAddr gpu_addr) {
    // Render target registers filled by garbage commands (sizes like 2147487260x2147485323, or
    // 0x0 with 36232 layers): creating the image would abort. 16384 is the hardware's largest
    // texture, and what AMD drivers accept.
    static constexpr u32 MaxRenderTargetSize = 16384;
    static constexpr s32 MaxRenderTargetLayers = 2048;
    if (info.size.width == 0 || info.size.height == 0 || info.size.width > MaxRenderTargetSize ||
        info.size.height > MaxRenderTargetSize || info.resources.layers > MaxRenderTargetLayers) {
        // The registers stay garbage for every draw until the next valid bind.
        static std::atomic<u32> reported{};
        if (reported.fetch_add(1, std::memory_order_relaxed) < 16) {
            LOG_WARNING(HW_GPU, "Ignoring {}x{} render target with {} layers at {:#x}",
                        info.size.width, info.size.height, info.resources.layers, gpu_addr);
        }
        return ImageViewId{}; // No render target: the null view has no format to size
    }
    ImageId image_id{};
    bool delete_state = has_deleted_images;
    creating_render_target = true;
    runtime.creating_render_target = true;
    do {
        has_deleted_images = false;
        image_id = FindOrInsertImage(info, gpu_addr);
        delete_state |= has_deleted_images;
    } while (has_deleted_images);
    creating_render_target = false;
    runtime.creating_render_target = false;
    has_deleted_images = delete_state;
    if (!image_id) {
        // No attachment: the null view has no host image to render into.
        return ImageViewId{};
    }
    Image& image = slot_images[image_id];
    const ImageViewType view_type = RenderTargetImageViewType(info);
    SubresourceBase base;
    if (image.info.type == ImageType::Linear) {
        base = SubresourceBase{.level = 0, .layer = 0};
    } else {
        base = image.TryFindBase(gpu_addr).value();
    }
    const s32 layers = image.info.type == ImageType::e3D ? info.size.depth : info.resources.layers;
    if (True(image.flags & ImageFlagBits::Unbacked)) {
        static u32 logged{};
        if (logged++ < 64) {
            LOG_INFO(HW_GPU, "Render target in unbacked image {:#x} at {:#x}: {}x{} level {} layer {}",
                     image.gpu_addr, gpu_addr, info.size.width, info.size.height, base.level, base.layer);
        }
    }
    const SubresourceRange range{
        .base = base,
        .extent = {.levels = 1, .layers = layers},
    };
    return FindOrEmplaceImageView(image_id, ImageViewInfo(view_type, info.format, range));
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachImageInRegion(DAddr cpu_addr, size_t size, Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
    constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
    boost::container::small_vector<ImageId, 32> images;
    boost::container::small_vector<ImageMapId, 32> maps;
    ForEachCPUPage(cpu_addr, size, [this, &images, &maps, cpu_addr, size, func](u64 page) {
        const auto it = page_table.find(page);
        if (it == page_table.end()) {
            if constexpr (BOOL_BREAK) {
                return false;
            } else {
                return;
            }
        }
        for (const ImageMapId map_id : it->second) {
            ImageMapView& map = slot_map_views[map_id];
            if (map.picked) {
                continue;
            }
            if (!map.Overlaps(cpu_addr, size)) {
                continue;
            }
            map.picked = true;
            maps.push_back(map_id);
            Image& image = slot_images[map.image_id];
            if (True(image.flags & ImageFlagBits::Picked)) {
                continue;
            }
            image.flags |= ImageFlagBits::Picked;
            images.push_back(map.image_id);
            if constexpr (BOOL_BREAK) {
                if (func(map.image_id, image)) {
                    return true;
                }
            } else {
                func(map.image_id, image);
            }
        }
        if constexpr (BOOL_BREAK) {
            return false;
        }
    });
    for (const ImageId image_id : images) {
        slot_images[image_id].flags &= ~ImageFlagBits::Picked;
    }
    for (const ImageMapId map_id : maps) {
        slot_map_views[map_id].picked = false;
    }
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachImageInRegionGPU(size_t as_id, GPUVAddr gpu_addr, size_t size,
                                              Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
    constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
    boost::container::small_vector<ImageId, 8> images;
    auto storage_id = getStorageID(as_id);
    if (!storage_id) {
        return;
    }
    auto& gpu_page_table = gpu_page_table_storage[*storage_id * 2];
    ForEachGPUPage(gpu_addr, size,
                   [this, &gpu_page_table, &images, gpu_addr, size, func](u64 page) {
                       const auto it = gpu_page_table.find(page);
                       if (it == gpu_page_table.end()) {
                           if constexpr (BOOL_BREAK) {
                               return false;
                           } else {
                               return;
                           }
                       }
                       for (const ImageId image_id : it->second) {
                           Image& image = slot_images[image_id];
                           if (True(image.flags & ImageFlagBits::Picked)) {
                               continue;
                           }
                           if (!image.OverlapsGPU(gpu_addr, size)) {
                               continue;
                           }
                           image.flags |= ImageFlagBits::Picked;
                           images.push_back(image_id);
                           if constexpr (BOOL_BREAK) {
                               if (func(image_id, image)) {
                                   return true;
                               }
                           } else {
                               func(image_id, image);
                           }
                       }
                       if constexpr (BOOL_BREAK) {
                           return false;
                       }
                   });
    for (const ImageId image_id : images) {
        slot_images[image_id].flags &= ~ImageFlagBits::Picked;
    }
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachSparseImageInRegion(size_t as_id, GPUVAddr gpu_addr, size_t size,
                                                 Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, ImageId, Image&>::type;
    constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
    boost::container::small_vector<ImageId, 8> images;
    auto storage_id = getStorageID(as_id);
    if (!storage_id) {
        return;
    }
    auto& sparse_page_table = gpu_page_table_storage[*storage_id * 2 + 1];
    ForEachGPUPage(gpu_addr, size,
                   [this, &sparse_page_table, &images, gpu_addr, size, func](u64 page) {
                       const auto it = sparse_page_table.find(page);
                       if (it == sparse_page_table.end()) {
                           if constexpr (BOOL_BREAK) {
                               return false;
                           } else {
                               return;
                           }
                       }
                       for (const ImageId image_id : it->second) {
                           Image& image = slot_images[image_id];
                           if (True(image.flags & ImageFlagBits::Picked)) {
                               continue;
                           }
                           if (!image.OverlapsGPU(gpu_addr, size)) {
                               continue;
                           }
                           image.flags |= ImageFlagBits::Picked;
                           images.push_back(image_id);
                           if constexpr (BOOL_BREAK) {
                               if (func(image_id, image)) {
                                   return true;
                               }
                           } else {
                               func(image_id, image);
                           }
                       }
                       if constexpr (BOOL_BREAK) {
                           return false;
                       }
                   });
    for (const ImageId image_id : images) {
        slot_images[image_id].flags &= ~ImageFlagBits::Picked;
    }
}

template <class P>
template <typename Func>
void TextureCache<P>::ForEachSparseSegment(ImageBase& image, Func&& func) {
    using FuncReturn = typename std::invoke_result<Func, GPUVAddr, DAddr, size_t>::type;
    constexpr bool RETURNS_BOOL = std::is_same_v<FuncReturn, bool>;
    const auto segments = gpu_memory->GetSubmappedRange(image.gpu_addr, image.guest_size_bytes);
    for (const auto& [gpu_addr, size] : segments) {
        std::optional<DAddr> cpu_addr = gpu_memory->GpuToCpuAddress(gpu_addr);
        ASSERT(cpu_addr);
        if constexpr (RETURNS_BOOL) {
            if (func(gpu_addr, *cpu_addr, size)) {
                break;
            }
        } else {
            func(gpu_addr, *cpu_addr, size);
        }
    }
}

template <class P>
ImageViewId TextureCache<P>::FindOrEmplaceImageView(ImageId image_id, const ImageViewInfo& info) {
    Image& image = slot_images[image_id];
    if (const ImageViewId image_view_id = image.FindView(info); image_view_id) {
        return image_view_id;
    }
    if (True(image.flags & ImageFlagBits::Unbacked) &&
        False(image.flags & ImageFlagBits::SparseLayers)) {
        // How the game uses an image too large to back (e.g. a sparse virtual texture).
        LOG_INFO(HW_GPU, "View of unbacked image {:#x}: type {} format {} levels {}+{} layers {}+{}",
                 image.gpu_addr, static_cast<u32>(info.type), static_cast<u32>(info.format),
                 info.range.base.level, info.range.extent.levels, info.range.base.layer,
                 info.range.extent.layers);
    }
    const ImageViewId image_view_id = slot_image_views.insert(runtime, info, image_id, image, slot_images);
    image.InsertView(info, image_view_id);
    return image_view_id;
}

template <class P>
void TextureCache<P>::RegisterImage(ImageId image_id) {
    ImageBase& image = slot_images[image_id];
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered),
               "Trying to register an already registered image");
    image.flags |= ImageFlagBits::Registered;
    page_serial.fetch_add(1, std::memory_order_release);
    total_used_memory += ImageMemoryBytes(slot_images[image_id]);
    image.lru_index = lru_cache.Insert(image_id, frame_tick);

    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes, [this, image_id](u64 page) {
        (*channel_state->gpu_page_table)[page].push_back(image_id);
    });
    if (False(image.flags & ImageFlagBits::Sparse)) {
        auto map_id =
            slot_map_views.insert(image.gpu_addr, image.cpu_addr, image.guest_size_bytes, image_id);
        page_filter.Mark(image.cpu_addr, image.guest_size_bytes);
        ForEachCPUPage(image.cpu_addr, image.guest_size_bytes,
                       [this, map_id](u64 page) { page_table[page].push_back(map_id); });
        image.map_view_id = map_id;
        return;
    }
    boost::container::small_vector<ImageViewId, 16> sparse_maps;
    ForEachSparseSegment(
        image, [this, image_id, &sparse_maps](GPUVAddr gpu_addr, DAddr cpu_addr, size_t size) {
            auto map_id = slot_map_views.insert(gpu_addr, cpu_addr, size, image_id);
            page_filter.Mark(cpu_addr, size);
            ForEachCPUPage(cpu_addr, size,
                           [this, map_id](u64 page) { page_table[page].push_back(map_id); });
            sparse_maps.push_back(map_id);
        });
    sparse_views.emplace(image_id, std::move(sparse_maps));
    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes, [this, image_id](u64 page) {
        (*channel_state->sparse_page_table)[page].push_back(image_id);
    });
}

template <class P>
void TextureCache<P>::UnregisterImage(ImageId image_id) {
    Image& image = slot_images[image_id];
    ASSERT_MSG(True(image.flags & ImageFlagBits::Registered),
               "Trying to unregister an already registered image");
    image.flags &= ~ImageFlagBits::Registered;
    image.flags &= ~ImageFlagBits::BadOverlap;
    lru_cache.Free(image.lru_index);
    const auto& clear_page_table =
        [image_id](u64 page, ::Common::unordered_map<u64, std::vector<ImageId>, Common::IdentityHash<u64>>& selected_page_table) {
            const auto page_it = selected_page_table.find(page);
            if (page_it == selected_page_table.end()) {
                ASSERT_MSG(false, "Unregistering unregistered page={:#x}", page << YUZU_PAGEBITS);
                return;
            }
            std::vector<ImageId>& image_ids = page_it->second;
            const auto vector_it = std::ranges::find(image_ids, image_id);
            if (vector_it == image_ids.end()) {
                ASSERT_MSG(false, "Unregistering unregistered image in page={:#x}",
                           page << YUZU_PAGEBITS);
                return;
            }
            image_ids.erase(vector_it);
        };
    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes, [this, &clear_page_table](u64 page) {
        clear_page_table(page, (*channel_state->gpu_page_table));
    });
    if (False(image.flags & ImageFlagBits::Sparse)) {
        const auto map_id = image.map_view_id;
        ForEachCPUPage(image.cpu_addr, image.guest_size_bytes, [this, map_id](u64 page) {
            const auto page_it = page_table.find(page);
            if (page_it == page_table.end()) {
                ASSERT_MSG(false, "Unregistering unregistered page={:#x}", page << YUZU_PAGEBITS);
                return;
            }
            std::vector<ImageMapId>& image_map_ids = page_it->second;
            const auto vector_it = std::ranges::find(image_map_ids, map_id);
            if (vector_it == image_map_ids.end()) {
                ASSERT_MSG(false, "Unregistering unregistered image in page={:#x}",
                           page << YUZU_PAGEBITS);
                return;
            }
            image_map_ids.erase(vector_it);
        });
        slot_map_views.erase(map_id);
        return;
    }
    ForEachGPUPage(image.gpu_addr, image.guest_size_bytes, [this, &clear_page_table](u64 page) {
        clear_page_table(page, (*channel_state->sparse_page_table));
    });
    auto it = sparse_views.find(image_id);
    ASSERT(it != sparse_views.end());
    auto& sparse_maps = it->second;
    for (auto& map_view_id : sparse_maps) {
        const auto& map_range = slot_map_views[map_view_id];
        const DAddr cpu_addr = map_range.cpu_addr;
        const std::size_t size = map_range.size;
        ForEachCPUPage(cpu_addr, size, [this, image_id](u64 page) {
            const auto page_it = page_table.find(page);
            if (page_it == page_table.end()) {
                ASSERT_MSG(false, "Unregistering unregistered page={:#x}", page << YUZU_PAGEBITS);
                return;
            }
            std::vector<ImageMapId>& image_map_ids = page_it->second;
            auto vector_it = image_map_ids.begin();
            while (vector_it != image_map_ids.end()) {
                ImageMapView& map = slot_map_views[*vector_it];
                if (map.image_id != image_id) {
                    vector_it++;
                    continue;
                }
                if (!map.picked) {
                    map.picked = true;
                }
                vector_it = image_map_ids.erase(vector_it);
            }
        });
        slot_map_views.erase(map_view_id);
    }
    sparse_views.erase(it);
}

template <class P>
void TextureCache<P>::TrackImage(ImageBase& image, ImageId image_id) {
    ASSERT(False(image.flags & ImageFlagBits::Tracked));
    image.flags |= ImageFlagBits::Tracked;
    if (False(image.flags & ImageFlagBits::Sparse)) {
        if (image.cpu_addr < ~(1ULL << 40)) {
            device_memory.UpdatePagesCachedCount(image.cpu_addr, image.guest_size_bytes, 1);
        }
        return;
    }
    if (True(image.flags & ImageFlagBits::Registered)) {
        auto it = sparse_views.find(image_id);
        ASSERT(it != sparse_views.end());
        auto& sparse_maps = it->second;
        for (auto& map_view_id : sparse_maps) {
            const auto& map = slot_map_views[map_view_id];
            const DAddr cpu_addr = map.cpu_addr;
            const std::size_t size = map.size;
            device_memory.UpdatePagesCachedCount(cpu_addr, size, 1);
        }
        return;
    }
    ForEachSparseSegment(image,
                         [this]([[maybe_unused]] GPUVAddr gpu_addr, DAddr cpu_addr, size_t size) {
                             device_memory.UpdatePagesCachedCount(cpu_addr, size, 1);
                         });
}

template <class P>
void TextureCache<P>::UntrackImage(ImageBase& image, ImageId image_id) {
    ASSERT(True(image.flags & ImageFlagBits::Tracked));
    image.flags &= ~ImageFlagBits::Tracked;
    if (False(image.flags & ImageFlagBits::Sparse)) {
        if (image.cpu_addr < ~(1ULL << 40)) {
            device_memory.UpdatePagesCachedCount(image.cpu_addr, image.guest_size_bytes, -1);
        }
        return;
    }
    ASSERT(True(image.flags & ImageFlagBits::Registered));
    auto it = sparse_views.find(image_id);
    ASSERT(it != sparse_views.end());
    auto& sparse_maps = it->second;
    for (auto& map_view_id : sparse_maps) {
        const auto& map = slot_map_views[map_view_id];
        const DAddr cpu_addr = map.cpu_addr;
        const std::size_t size = map.size;
        device_memory.UpdatePagesCachedCount(cpu_addr, size, -1);
    }
}

template <class P>
void TextureCache<P>::DeleteImage(ImageId image_id, bool immediate_delete) {
    ImageBase& image = slot_images[image_id];
    ++images_deleted;
    VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::Invalidation);
    if (True(image.flags & ImageFlagBits::CollectorDownload)) {
        CancelCollectorDownload(image_id);
    }
    // Drop every decode still queued for it, whatever IsDecoding says: an image re-decoded while
    // a decode was in flight has two, and the first to finish clears the flag. A decode left
    // behind was uploaded into the next image to take this slot (another texture's data: garbage
    // textures, and a staging overrun when that image was smaller).
    for (auto& async_decode : async_decodes) {
        if (async_decode->image_id == image_id) {
            async_decode->cancelled = true;
        }
    }
    if (image.HasScaled()) {
        total_used_memory -=
            (std::min)(total_used_memory, GetScaledImageSizeBytes(slot_images[image_id]));
    }
    // TickFrame sets the total from the driver, which can be under the sum of these estimates.
    total_used_memory -= (std::min)(total_used_memory, ImageMemoryBytes(slot_images[image_id]));
    const u64 freed_bytes =
        (image.HasScaled() ? GetScaledImageSizeBytes(slot_images[image_id]) : 0) +
        ImageMemoryBytes(slot_images[image_id]);
    const GPUVAddr gpu_addr = image.gpu_addr;
    const auto alloc_it = image_allocs_table.find(gpu_addr);
    if (alloc_it == image_allocs_table.end()) {
        ASSERT_MSG(false, "Trying to delete an image alloc that does not exist in address {:#x}",
                   gpu_addr);
        return;
    }
    const ImageAllocId alloc_id = alloc_it->second;
    std::vector<ImageId>& alloc_images = slot_image_allocs[alloc_id].images;
    const auto alloc_image_it = std::ranges::find(alloc_images, image_id);
    if (alloc_image_it == alloc_images.end()) {
        ASSERT_MSG(false, "Trying to delete an image that does not exist");
        return;
    }
    ASSERT_MSG(False(image.flags & ImageFlagBits::Tracked), "Image was not untracked");
    ASSERT_MSG(False(image.flags & ImageFlagBits::Registered), "Image was not unregistered");

    // Mark render targets as dirty
    auto& dirty = maxwell3d->dirty.flags;
    dirty[Dirty::RenderTargets] = true;
    dirty[Dirty::ZetaBuffer] = true;
    for (size_t rt = 0; rt < NUM_RT; ++rt) {
        dirty[Dirty::ColorBuffer0 + rt] = true;
    }
    const std::span<const ImageViewId> image_view_ids = image.image_view_ids;
    for (const ImageViewId image_view_id : image_view_ids) {
        std::ranges::replace(render_targets.color_buffer_ids, image_view_id, ImageViewId{});
        if (render_targets.depth_buffer_id == image_view_id) {
            render_targets.depth_buffer_id = ImageViewId{};
        }
    }
    RemoveImageViewReferences(image_view_ids);
    RemoveFramebuffers(image_view_ids);

    for (const AliasedImage& alias : image.aliased_images) {
        ImageBase& other_image = slot_images[alias.id];
        [[maybe_unused]] const size_t num_removed_aliases =
            std::erase_if(other_image.aliased_images, [image_id](const AliasedImage& other_alias) {
                return other_alias.id == image_id;
            });
        other_image.CheckAliasState();
        ASSERT_MSG(num_removed_aliases == 1, "Invalid number of removed aliases: {}",
                   num_removed_aliases);
    }
    for (const ImageId overlap_id : image.overlapping_images) {
        ImageBase& other_image = slot_images[overlap_id];
        [[maybe_unused]] const size_t num_removed_overlaps = std::erase_if(
            other_image.overlapping_images,
            [image_id](const ImageId other_overlap_id) { return other_overlap_id == image_id; });
        other_image.CheckBadOverlapState();
        ASSERT_MSG(num_removed_overlaps == 1, "Invalid number of removed overlapps: {}",
                   num_removed_overlaps);
    }
    for (const ImageViewId image_view_id : image_view_ids) {
        if (!immediate_delete) {
            sentenced_image_view.Push(std::move(slot_image_views[image_view_id]));
        }
        slot_image_views.erase(image_view_id);
    }
    const bool retained = RetainPendingImageDownloads(image_id, slot_images[image_id],
                                                       uncommitted_downloads, committed_downloads);
    if (retained) {
        LOG_INFO(Render_Vulkan, "Retained image for queued download: id={}", image_id.Value());
    } else if (!immediate_delete) {
        if constexpr (requires { runtime.CurrentTick(); }) {
            gpu_sentenced_images.push_back(
                {runtime.CurrentTick(), freed_bytes, std::move(slot_images[image_id])});
            gpu_sentenced_bytes += freed_bytes;
        } else {
            sentenced_images.Push(std::move(slot_images[image_id]));
            sentenced_image_bytes[sentenced_image_index] += freed_bytes;
        }
    }
    slot_images.erase(image_id);

    alloc_images.erase(alloc_image_it);
    if (alloc_images.empty()) {
        image_allocs_table.erase(alloc_it);
        slot_image_allocs.erase(alloc_id);
    }
    for (size_t c : active_channel_ids) {
        auto& channel_info = channel_storage[c];
#ifndef NDEBUG
        // Every id the image tables hold, for each deleted image: debug builds only. The tables
        // are invalidated below, so each entry is written again before it is read.
        if constexpr (ENABLE_VALIDATION) {
            std::ranges::fill(channel_info.graphics_image_table.bound_ids, CORRUPT_ID);
            std::ranges::fill(channel_info.compute_image_table.bound_ids, CORRUPT_ID);
        }
#endif
        channel_info.graphics_image_table.Invalidate();
        channel_info.compute_image_table.Invalidate();
    }
    has_deleted_images = true;
}

template <class P>
void TextureCache<P>::RemoveImageViewReferences(std::span<const ImageViewId> removed_views) {
    for (const ImageViewId removed : removed_views) {
        const auto refs = image_view_descriptors.find(removed);
        if (refs == image_view_descriptors.end()) {
            continue;
        }
        for (const auto& [channel, descriptor] : refs->second) {
            // The channel may be gone, or its entry point to another view since.
            if (std::ranges::find(active_channel_ids, channel) == active_channel_ids.end()) {
                continue;
            }
            auto& image_views = channel_storage[channel].image_views;
            if (const auto it = image_views.find(descriptor);
                it != image_views.end() && it->second == removed) {
                image_views.erase(it);
            }
        }
        image_view_descriptors.erase(refs);
    }
}

template <class P>
void TextureCache<P>::RemoveFramebuffers(std::span<const ImageViewId> removed_views) {
    runtime.FlushDeferredClear();
    auto it = framebuffers.begin();
    while (it != framebuffers.end()) {
        if (it->first.Contains(removed_views)) {
            auto framebuffer_id = it->second;
            ASSERT(framebuffer_id);
            if (framebuffer_id == last_framebuffer_id) {
                last_framebuffer_id = {};
                last_framebuffer_serial = 0;
            }
            sentenced_framebuffers.Push(std::move(slot_framebuffers[framebuffer_id]));
            slot_framebuffers.erase(framebuffer_id);
            it = framebuffers.erase(it);
        } else {
            ++it;
        }
    }
}

template <class P>
void TextureCache<P>::MarkModification(ImageBase& image) noexcept {
    image.flags |= ImageFlagBits::GpuModified;
    image.modification_tick = ++modification_tick;
    if (!marking_render_targets) {
        ++image.other_writes;
    }
    // This write is newer than every alias: they copy it when next used.
    for (const AliasedImage& alias : image.aliased_images) {
        slot_images[alias.id].alias_maybe_newer = true;
    }
}

template <class P>
void TextureCache<P>::RecheckDeferredFeedback() {
    if (feedback_rechecks.empty()) {
        return;
    }
    const boost::container::small_vector<ImageId, 4> rechecks = feedback_rechecks;
    feedback_rechecks.clear();
    for (const ImageId image_id : rechecks) {
        // UpdateRenderTargets may have deleted the image (a join); a reused slot only syncs.
        if (!slot_images.contains(image_id) ||
            slot_images[image_id].feedback_rt_serial == render_targets_serial) {
            continue;
        }
        // The targets did change: copy what the deferral skipped, without coalescing.
        slot_images[image_id].alias_maybe_newer = true;
        SynchronizeAliases(image_id);
    }
}

template <class P>
void TextureCache<P>::SynchronizeAliases(ImageId image_id) {
    // EDEN_NX2_ALIAS_WALK_LEGACY=1 walks the aliases on every use, as before.
    static const bool walk_always = [] {
        const char* value = std::getenv("EDEN_NX2_ALIAS_WALK_LEGACY");
        return value != nullptr && value[0] == '1';
    }();
    // A draw that samples an image whose newer alias is one of its own colour targets (a feedback
    // read: the target's memory viewed at another size or format) copies that target at most once
    // per guest texture barrier and render-target set. Without a barrier the guest's texture reads
    // are not ordered after its colour writes, so a copy before every draw gives more than the
    // hardware does, and it was a full-screen copy for every draw of Donkey Kong Bananza's
    // G-buffer at Ingot Isle (~1,600 a frame, against ~60 barriers). Any other write to the
    // target (copy, blit, clear, storage write, upload) is copied at the next draw.
    // EDEN_NX2_FEEDBACK_COPIES=exact restores a copy for every draw.
    static const bool coalesce_feedback = [] {
        const char* value = std::getenv("EDEN_NX2_FEEDBACK_COPIES");
        return value == nullptr || std::string_view{value} != "exact";
    }();
    boost::container::small_vector<const AliasedImage*, 8> aliased_images;
    Image& image = slot_images[image_id];
    // Nothing newer can exist unless an alias was modified, synchronized or linked since the last
    // walk (Bananza samples a texture with ~590 aliases thousands of times a frame).
    if (!image.alias_maybe_newer && !walk_always) {
        return;
    }
    image.alias_maybe_newer = false;
    const bool coalesce = coalesce_feedback && feedback_sampling && rt_active_mask != 0;
    // The textures are prepared before UpdateRenderTargets: with the targets dirty, this draw's
    // set may differ from rt_image_id and render_targets_serial, so nothing is deferred.
    const bool targets_current =
        coalesce && maxwell3d != nullptr && !maxwell3d->dirty.flags[Dirty::RenderTargets];
    const u64 barrier_epoch = texture_barrier_epoch.load(std::memory_order_relaxed);
    const auto is_color_target = [this](ImageId id) {
        for (size_t index = 0; index < NUM_RT; ++index) {
            if ((rt_active_mask & (1u << index)) != 0 && rt_image_id[index] == id) {
                return true;
            }
        }
        return false;
    };
    ImageId feedback_copied{};
    u64 feedback_copied_writes = 0;
    bool feedback_deferred = false;
    bool recheck_needed = false;
    bool any_rescaled = True(image.flags & ImageFlagBits::Rescaled);
    bool any_modified = True(image.flags & ImageFlagBits::GpuModified);
    u64 most_recent_tick = image.modification_tick;
    for (const AliasedImage& aliased : image.aliased_images) {
        ImageBase& aliased_image = slot_images[aliased.id];
        if (image.modification_tick < aliased_image.modification_tick) {
            if (coalesce && is_color_target(aliased.id)) {
                const bool same_feedback =
                    image.feedback_source == aliased.id &&
                    image.feedback_rt_serial == render_targets_serial &&
                    image.feedback_barrier_epoch == barrier_epoch &&
                    image.feedback_source_writes == aliased_image.other_writes;
                if (same_feedback && targets_current) {
                    feedback_deferred = true;
                    continue;
                }
                // EDEN_NX2_FEEDBACK_DIRTY_TARGETS=1: Splatoon Raiders' pier sets its targets again
                // before each of ~480 draws a frame that sample an 880-wide view of their 896-wide
                // target, so each was copied. The targets usually stay the same: whether they did
                // is checked after UpdateRenderTargets (RecheckDeferredFeedback). Only when both
                // images are scaled alike, so that check never rescales a view already bound.
                if (same_feedback && feedback_recheck &&
                    True(image.flags & ImageFlagBits::Rescaled) ==
                        True(aliased_image.flags & ImageFlagBits::Rescaled)) {
                    feedback_deferred = true;
                    recheck_needed = true;
                    continue;
                }
                feedback_copied = aliased.id;
                feedback_copied_writes = aliased_image.other_writes;
            }
            most_recent_tick = (std::max)(most_recent_tick, aliased_image.modification_tick);
            aliased_images.push_back(&aliased);
            any_rescaled |= True(aliased_image.flags & ImageFlagBits::Rescaled);
            any_modified |= True(aliased_image.flags & ImageFlagBits::GpuModified);
        }
    }
    if (feedback_deferred) {
        // Still older than the target: copied at the next barrier, target change or other write.
        image.alias_maybe_newer = true;
    }
    if (recheck_needed) {
        feedback_rechecks.push_back(image_id);
    }
    if (feedback_copied) {
        image.feedback_source = feedback_copied;
        image.feedback_barrier_epoch = barrier_epoch;
        image.feedback_rt_serial = render_targets_serial;
        image.feedback_source_writes = feedback_copied_writes;
    }
    if (aliased_images.empty()) {
        return;
    }
    const bool can_rescale = ImageCanRescale(image);
    if (any_rescaled) {
        if (can_rescale) {
            ScaleUp(image);
        } else {
            ScaleDown(image);
        }
    }
    image.modification_tick = most_recent_tick;
    // This image now holds newer contents than its older aliases.
    for (const AliasedImage& aliased : image.aliased_images) {
        slot_images[aliased.id].alias_maybe_newer = true;
    }
    if (any_modified) {
        image.flags |= ImageFlagBits::GpuModified;
    }
    std::ranges::sort(aliased_images, [this](const AliasedImage* lhs, const AliasedImage* rhs) {
        const ImageBase& lhs_image = slot_images[lhs->id];
        const ImageBase& rhs_image = slot_images[rhs->id];
        return lhs_image.modification_tick < rhs_image.modification_tick;
    });
    const auto& resolution = Settings::values.resolution_info;
    for (const AliasedImage* const aliased : aliased_images) {
        if (!resolution.active || !any_rescaled) {
            CopyImage(image_id, aliased->id, aliased->copies);
            continue;
        }
        Image& aliased_image = slot_images[aliased->id];
        if (!can_rescale) {
            ScaleDown(aliased_image);
            CopyImage(image_id, aliased->id, aliased->copies);
            continue;
        }
        ScaleUp(aliased_image);
        CopyImage(image_id, aliased->id, aliased->copies);
    }
}

template <class P>
void TextureCache<P>::PrepareImage(ImageId image_id, bool is_modification, bool invalidate) {
    // The null image has no memory to upload and no LRU entry.
    if (!image_id || image_id == NULL_IMAGE_ID) {
        return;
    }
    Image& image = slot_images[image_id];
    if (invalidate) {
        image.flags &= ~(ImageFlagBits::CpuModified | ImageFlagBits::GpuModified);
        if (False(image.flags & ImageFlagBits::Tracked)) {
            TrackImage(image, image_id);
        }
    } else {
        RefreshContents(image, image_id);
        if (!image.aliased_images.empty()) {
            SynchronizeAliases(image_id);
        }
    }
    if (is_modification) {
        if constexpr (requires { &Image::BindLayers; }) {
            if (True(image.flags & ImageFlagBits::SparseLayers)) {
                std::vector<u32> layers(static_cast<size_t>(image.info.resources.layers));
                std::iota(layers.begin(), layers.end(), 0U);
                // Layers over the image's memory cap stay unbacked (logged once).
                BindSparseLayers(image, layers);
            }
        }
        if (True(image.flags & ImageFlagBits::UnwrittenSparse) &&
            False(image.flags & ImageFlagBits::Remapped)) {
            // The GPU renders into it: these writes are lost, later uses get a backed image.
            LOG_WARNING(HW_GPU, "GPU write into unwritten sparse image {:#x}; replacing it",
                        image.gpu_addr);
            Tegra::Nx2SparseMarkWritten(image.gpu_addr, image.guest_size_bytes);
            image.flags |= ImageFlagBits::CpuModified | ImageFlagBits::Remapped;
            VideoCommon::Nx2FastDraw::Break(VideoCommon::Nx2FastDraw::Reason::Invalidation);
        }
        MarkModification(image);
    }
    lru_cache.Touch(image.lru_index, frame_tick);
}

template <class P>
void TextureCache<P>::PrepareImageView(ImageViewId image_view_id, bool is_modification,
                                       bool invalidate) {
    if (!image_view_id || image_view_id == NULL_IMAGE_VIEW_ID) {
        return;
    }
    const ImageViewBase& image_view = slot_image_views[image_view_id];
    if (image_view.IsBuffer()) {
        return;
    }
    PrepareImage(image_view.image_id, is_modification, invalidate);
}

template <class P>
void TextureCache<P>::CopyImage(ImageId dst_id, ImageId src_id, std::vector<ImageCopy> copies) {
    Image& dst = slot_images[dst_id];
    Image& src = slot_images[src_id];
    if (dst.info.astc_opaque != src.info.astc_opaque) {
        // BC1 and BC3 blocks differ in size, so the host can't copy between them; both hold the
        // same guest data, which is uploaded from guest memory instead.
        return;
    }
    // As in BlitImage: land a parked deferred clear before the copy overwrites the image.
    runtime.FlushDeferredClear();
    const bool is_rescaled = True(src.flags & ImageFlagBits::Rescaled);
    if (is_rescaled) {
        ASSERT(True(dst.flags & ImageFlagBits::Rescaled));
        const bool both_2d{src.info.type == ImageType::e2D && dst.info.type == ImageType::e2D};
        const auto& resolution = Settings::values.resolution_info;
        for (auto& copy : copies) {
            copy.src_offset.x = resolution.ScaleUp(copy.src_offset.x);
            copy.dst_offset.x = resolution.ScaleUp(copy.dst_offset.x);
            copy.extent.width = resolution.ScaleUp(copy.extent.width);
            if (both_2d) {
                copy.src_offset.y = resolution.ScaleUp(copy.src_offset.y);
                copy.dst_offset.y = resolution.ScaleUp(copy.dst_offset.y);
                copy.extent.height = resolution.ScaleUp(copy.extent.height);
            }
        }
    }
    const auto dst_format_type = GetFormatType(dst.info.format);
    const auto src_format_type = GetFormatType(src.info.format);
    if (src_format_type == dst_format_type) {
        if constexpr (HAS_EMULATED_COPIES) {
            if (!runtime.CanImageBeCopied(dst, src)) {
                return runtime.EmulateCopyImage(dst, src, copies);
            }
        }
        return runtime.CopyImage(dst, src, copies);
    }
    UNIMPLEMENTED_IF(dst.info.type != ImageType::e2D);
    UNIMPLEMENTED_IF(src.info.type != ImageType::e2D);
    if (runtime.ShouldReinterpret(dst, src)) {
        return runtime.ReinterpretImage(dst, src, copies);
    }
    for (const ImageCopy& copy : copies) {
        UNIMPLEMENTED_IF(copy.dst_subresource.num_layers != 1);
        UNIMPLEMENTED_IF(copy.src_subresource.num_layers != 1);
        UNIMPLEMENTED_IF(copy.src_offset != Offset3D{});
        UNIMPLEMENTED_IF(copy.dst_offset != Offset3D{});

        const SubresourceBase dst_base{
            .level = copy.dst_subresource.base_level,
            .layer = copy.dst_subresource.base_layer,
        };
        const SubresourceBase src_base{
            .level = copy.src_subresource.base_level,
            .layer = copy.src_subresource.base_layer,
        };
        const SubresourceExtent dst_extent{.levels = 1, .layers = 1};
        const SubresourceExtent src_extent{.levels = 1, .layers = 1};
        const SubresourceRange dst_range{.base = dst_base, .extent = dst_extent};
        const SubresourceRange src_range{.base = src_base, .extent = src_extent};
        PixelFormat dst_format = dst.info.format;
        if (GetFormatType(src.info.format) == SurfaceType::DepthStencil &&
            GetFormatType(dst_format) == SurfaceType::ColorTexture &&
            BytesPerBlock(dst_format) == 4) {
            dst_format = PixelFormat::A8B8G8R8_UNORM;
        }
        const ImageViewInfo dst_view_info(ImageViewType::e2D, dst_format, dst_range);
        const ImageViewInfo src_view_info(ImageViewType::e2D, src.info.format, src_range);
        const auto [dst_framebuffer_id, dst_view_id] = RenderTargetFromImage(dst_id, dst_view_info);
        Framebuffer* const dst_framebuffer = &slot_framebuffers[dst_framebuffer_id];
        const ImageViewId src_view_id = FindOrEmplaceImageView(src_id, src_view_info);
        ImageView& dst_view = slot_image_views[dst_view_id];
        ImageView& src_view = slot_image_views[src_view_id];
        [[maybe_unused]] const Extent3D expected_size{
            .width = (std::min)(dst_view.size.width, src_view.size.width),
            .height = (std::min)(dst_view.size.height, src_view.size.height),
            .depth = (std::min)(dst_view.size.depth, src_view.size.depth),
        };
        const Extent3D scaled_extent = [is_rescaled, expected_size]() {
            if (!is_rescaled) {
                return expected_size;
            }
            const auto& resolution = Settings::values.resolution_info;
            return Extent3D{
                .width = resolution.ScaleUp(expected_size.width),
                .height = resolution.ScaleUp(expected_size.height),
                .depth = expected_size.depth,
            };
        }();
        UNIMPLEMENTED_IF(copy.extent != scaled_extent);

        runtime.ConvertImage(dst_framebuffer, dst_view, src_view);
    }
}

template <class P>
void TextureCache<P>::BindRenderTarget(ImageViewId* old_id, ImageViewId new_id) {
    if (*old_id == new_id) {
        return;
    }
    if (new_id) {
        const ImageViewBase& old_view = slot_image_views[new_id];
        if (True(old_view.flags & ImageViewFlagBits::PreemtiveDownload) &&
            False(slot_images[old_view.image_id].flags & (ImageFlagBits::Unbacked | ImageFlagBits::TrimmedLevels))) {
            const PendingDownload new_download{true, 0, old_view.image_id};
            uncommitted_downloads.emplace_back(new_download);
        }
    }
    *old_id = new_id;
}

template <class P>
std::pair<FramebufferId, ImageViewId> TextureCache<P>::RenderTargetFromImage(
    ImageId image_id, const ImageViewInfo& view_info) {
    const ImageViewId view_id = FindOrEmplaceImageView(image_id, view_info);
    const ImageBase& image = slot_images[image_id];
    const bool is_rescaled = True(image.flags & ImageFlagBits::Rescaled);
    const bool is_color = GetFormatType(image.info.format) == SurfaceType::ColorTexture;
    const ImageViewId color_view_id = is_color ? view_id : ImageViewId{};
    const ImageViewId depth_view_id = is_color ? ImageViewId{} : view_id;
    Extent3D extent = MipSize(image.info.size, view_info.range.base.level);
    if (is_rescaled) {
        const auto& resolution = Settings::values.resolution_info;
        extent.width = resolution.ScaleUp(extent.width);
        if (image.info.type == ImageType::e2D) {
            extent.height = resolution.ScaleUp(extent.height);
        }
    }
    const u32 num_samples = image.info.num_samples;
    const auto [samples_x, samples_y] = SamplesLog2(num_samples);
    const FramebufferId framebuffer_id = GetFramebufferId(RenderTargets{
        .color_buffer_ids = {color_view_id},
        .depth_buffer_id = depth_view_id,
        .size = {extent.width >> samples_x, extent.height >> samples_y},
        .is_rescaled = is_rescaled,
    });
    return {framebuffer_id, view_id};
}

template <class P>
bool TextureCache<P>::IsFullClear(ImageViewId id, std::optional<size_t> rt_index) {
    if (!id) {
        return true;
    }
    // EDEN_NX2_CLEAR_KEEPS_UPLOAD=1: never drop a target's pending upload for a clear.
    static const bool keep_upload = std::getenv("EDEN_NX2_CLEAR_KEEPS_UPLOAD") != nullptr;
    if (keep_upload) {
        return false;
    }
    const ImageViewBase& image_view = slot_image_views[id];
    const ImageBase& image = slot_images[image_view.image_id];
    const Extent3D size = image_view.size;
    const auto& regs = maxwell3d->regs;
    const auto& scissor = regs.scissor_test[0];
    if (image.info.resources.levels > 1 || image.info.resources.layers > 1) {
        // Images with multiple resources can't be cleared in a single call
        return false;
    }
    // A clear writes one colour target (clear_surface.RT), one layer, and the channels and
    // aspects it names. Every other bound target, channel or aspect keeps its contents, so its
    // pending CPU upload must not be dropped.
    const auto& surface = regs.clear_surface;
    if (surface.layer != 0) {
        return false;
    }
    if (rt_index) {
        if (*rt_index != surface.RT || !surface.R || !surface.G || !surface.B || !surface.A) {
            return false;
        }
    } else {
        const bool full_stencil = surface.S && regs.stencil_front_mask == 0xFF;
        switch (VideoCore::Surface::GetFormatType(image.info.format)) {
        case VideoCore::Surface::SurfaceType::Depth:
            if (!surface.Z) {
                return false;
            }
            break;
        case VideoCore::Surface::SurfaceType::Stencil:
            if (!full_stencil) {
                return false;
            }
            break;
        case VideoCore::Surface::SurfaceType::DepthStencil:
            if (!surface.Z || !full_stencil) {
                return false;
            }
            break;
        default:
            return false;
        }
    }
    // The clear covers at most the render area: the surface clip, cut to the smallest bound
    // target. Texels of a larger image outside it keep what the upload would have written.
    const auto smaller = [&size](u32 width, u32 height) {
        return width < size.width || height < size.height;
    };
    if (smaller(regs.surface_clip.width, regs.surface_clip.height)) {
        return false;
    }
    for (const ImageViewId other : render_targets.color_buffer_ids) {
        if (other &&
            smaller(slot_image_views[other].size.width, slot_image_views[other].size.height)) {
            return false;
        }
    }
    if (const ImageViewId depth = render_targets.depth_buffer_id;
        depth && smaller(slot_image_views[depth].size.width, slot_image_views[depth].size.height)) {
        return false;
    }
    if (regs.clear_control.use_scissor == 0) {
        // If scissor testing is disabled, the clear covers the whole render area
        return true;
    }
    // Make sure the clear covers all texels in the subresource
    return scissor.min_x == 0 && scissor.min_y == 0 && scissor.max_x >= size.width &&
           scissor.max_y >= size.height;
}

template <class P>
bool TextureCache<P>::ClearOverwritesDeferredUpload(ImageViewId id, bool is_depth,
                                                    size_t index) {
    if (!id || clear_deferred_uploads.empty()) {
        return true;
    }
    const ImageId image_id = slot_image_views[id].image_id;
    if (std::ranges::find(clear_deferred_uploads, image_id) == clear_deferred_uploads.end()) {
        return true;
    }
    // The image holds nothing yet, so the clear must write every texel of it.
    const ImageBase& image = slot_images[image_id];
    const u32 width = image.info.size.width;
    const u32 height = image.info.size.height;
    const auto& regs = maxwell3d->regs;
    const auto& surface = regs.clear_surface;
    if (surface.layer != 0 || regs.rasterize_enable == 0) {
        return false;
    }
    // The clear is bounded by the render area: the surface clip and the smallest target.
    if (regs.surface_clip.width < width || regs.surface_clip.height < height) {
        return false;
    }
    const auto too_small = [&](ImageViewId other) {
        if (!other) {
            return false;
        }
        const Extent3D other_size = slot_image_views[other].size;
        return other_size.width < width || other_size.height < height;
    };
    if (std::ranges::any_of(render_targets.color_buffer_ids, too_small) ||
        too_small(render_targets.depth_buffer_id)) {
        return false;
    }
    if (regs.clear_control.use_scissor != 0) {
        const auto& scissor = regs.scissor_test[0];
        // With a lower left origin the scissor is flipped within the surface clip height.
        const u32 min_height = (std::max)(height, regs.surface_clip.height.Value());
        if (scissor.enable != 0 && (scissor.min_x != 0 || scissor.min_y != 0 ||
                                    scissor.max_x < width || scissor.max_y < min_height)) {
            return false;
        }
    }
    const bool full_stencil = surface.S && regs.stencil_front_mask == 0xFF;
    switch (GetFormatType(image.info.format)) {
    case SurfaceType::ColorTexture:
        return !is_depth && index == surface.RT && surface.R && surface.G && surface.B &&
               surface.A;
    case SurfaceType::Depth:
        return is_depth && surface.Z;
    case SurfaceType::Stencil:
        return is_depth && full_stencil;
    case SurfaceType::DepthStencil:
        return is_depth && surface.Z && full_stencil;
    default:
        return false;
    }
}

template <class P>
void TextureCache<P>::CreateChannel(struct Tegra::Control::ChannelState& channel) {
    VideoCommon::ChannelSetupCaches<TextureCacheChannelInfo>::CreateChannel(channel);
    const auto it = channel_map.find(channel.bind_id);
    auto* this_state = &channel_storage[it->second];
    const auto& this_as_ref = address_spaces[channel.memory_manager->GetID()];
    this_state->gpu_page_table = &gpu_page_table_storage[this_as_ref.storage_id * 2];
    this_state->sparse_page_table = &gpu_page_table_storage[this_as_ref.storage_id * 2 + 1];
}

/// Bind a channel for execution.
template <class P>
void TextureCache<P>::OnGPUASRegister([[maybe_unused]] size_t map_id) {
    gpu_page_table_storage.emplace_back();
    gpu_page_table_storage.emplace_back();
}

} // namespace VideoCommon
