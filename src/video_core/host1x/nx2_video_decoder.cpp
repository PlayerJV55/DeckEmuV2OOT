// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/host1x/ffmpeg.h"
#include "video_core/host1x/h264_packet.h"
#include "video_core/host1x/nx2_video_decoder.h"

extern "C" {
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif

#include <libswscale/swscale.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

namespace Tegra::Host1x {

namespace {

/// Packets kept for a hardware decoder that hasn't output a frame yet; with that many and still
/// no frame, the CPU decoder takes over.
constexpr std::size_t MaxStartupPackets = 64;

// 2560x1440 VP9 movies decoded below 60 fps on one thread, and the client aborts when no frame is
// ready. Frame threads add a few frames of latency, covered by the client's queued inputs.
// EDEN_NX2_VDEC_FRAME_THREADS=0 keeps the single decode thread.
bool UseFrameThreads() {
    static const bool frame_threads = [] {
        const char* value = std::getenv("EDEN_NX2_VDEC_FRAME_THREADS");
        return value == nullptr || value[0] != '0';
    }();
    return frame_threads;
}

// libdav1d's threads, 0 for FFmpeg's default (it scales with the host's threads). The 4 frame
// threads the other decoders get also cap dav1d's frames in flight: upstream saw the Ocarina of
// Time intro drop 20-24 frames per 4200 with half the host threads against 5-7 at the default.
// EDEN_NX2_VDEC_DAV1D_THREADS=<n> sets the count (4 as before).
int Dav1dThreads() {
    static const int threads = [] {
        const char* value = std::getenv("EDEN_NX2_VDEC_DAV1D_THREADS");
        return value == nullptr ? 0 : std::clamp(std::atoi(value), 0, 64);
    }();
    return threads;
}

std::optional<NvdecCommon::VideoCodec> CodecFromMime(std::string_view mime) {
    if (mime.find("vp9") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP9;
    }
    if (mime.find("vp8") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::VP8;
    }
    // "video/av01" or "video/av1". FFmpeg decodes AV1 with libdav1d.
    if (mime.find("av01") != std::string_view::npos || mime.find("av1") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::AV1;
    }
    if (mime.find("avc") != std::string_view::npos || mime.find("264") != std::string_view::npos) {
        return NvdecCommon::VideoCodec::H264;
    }
    return std::nullopt;
}

} // namespace

struct Nx2VideoDecoder::Impl {
    std::string name;
    std::optional<FFmpeg::Decoder> decoder;
    std::optional<FFmpeg::HardwareContext> hardware;
    std::optional<FFmpeg::DecoderContext> context;
    SwsContext* scaler{};
    bool open{};
    bool using_hardware{};
    bool received_any{}; ///< A frame came out since the decoder was opened or reset
    H264PacketConverter h264_packets;
    /// Timestamps of submitted frames not yet output. Frames copied back from a host GPU decoder
    /// lose theirs and take the earliest one instead.
    std::multiset<u64> pending_pts;
    std::vector<u8> packet; ///< FFmpeg reads up to AV_INPUT_BUFFER_PADDING_SIZE past the data
    /// Padded packets sent to a hardware decoder before its first frame, so the CPU decoder can
    /// start over from the stream headers if it fails.
    std::vector<std::pair<u64, std::vector<u8>>> startup_packets;

    ~Impl() {
        sws_freeContext(scaler);
        context.reset();
        hardware.reset();
    }

    /// Opens a hardware decoder, or a CPU one; false if that fails.
    bool Open(NvdecCommon::VideoCodec codec, bool use_gpu) {
        open = false;
        using_hardware = false;
        context.reset();
        hardware.reset();
        decoder.emplace(codec);
        if (decoder->GetCodec() == nullptr) {
            return false;
        }
        context.emplace(*decoder);
        if (use_gpu) {
            hardware.emplace();
            if (!hardware->InitializeForDecoder(*context, *decoder)) {
                context.reset();
                hardware.reset();
                return false;
            }
        } else if (UseFrameThreads()) {
            AVCodecContext* const codec_context = context->GetCodecContext();
            // A low-delay context stops FFmpeg from choosing frame threads.
            codec_context->flags &= ~AV_CODEC_FLAG_LOW_DELAY;
            codec_context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            codec_context->thread_count =
                std::string_view{decoder->GetCodec()->name} == "libdav1d" ? Dav1dThreads() : 4;
        }
        open = context->OpenContext(*decoder);
        using_hardware = open && use_gpu;
        if (!open) {
            context.reset();
            hardware.reset();
        }
        return open;
    }

    /// Sends a packet and takes the frames out; false on a decode error.
    bool Submit(std::span<const u8> bytes, u64 pts, std::vector<Nx2Picture>& pictures) {
        {
            FFmpeg::Packet input{bytes};
            input.GetPacket()->pts = static_cast<s64>(pts);
            if (!context->SendPacket(input)) {
                return false;
            }
        }
        // Hidden VP9 frames never come out: keep the set from growing over a long movie.
        if (pending_pts.size() >= 64) {
            pending_pts.erase(pending_pts.begin());
        }
        pending_pts.insert(pts);
        Receive(pictures);
        return !context->HadDecodeError();
    }

    /// Opens the CPU decoder in place of a hardware one that gave no frame, and sends it the
    /// packets the hardware one got.
    bool FallBackToCpu(std::vector<Nx2Picture>& pictures) {
        auto packets = std::exchange(startup_packets, {});
        pending_pts.clear();
        const auto codec = CodecFromMime(name);
        if (!codec || !Open(*codec, false)) {
            LOG_ERROR(HW_GPU, "nx2 video: no CPU decoder for \"{}\" after the hardware one failed",
                      name);
            return false;
        }
        LOG_INFO(HW_GPU, "nx2 video: hardware decoding failed for \"{}\", falling back to CPU",
                 name);
        for (const auto& [pts, bytes] : packets) {
            Submit(std::span<const u8>{bytes.data(), bytes.size() - AV_INPUT_BUFFER_PADDING_SIZE},
                   pts, pictures);
        }
        return true;
    }

    void Receive(std::vector<Nx2Picture>& pictures) {
        while (auto frame = context->ReceiveFrame()) {
            const AVFrame* f = frame->GetFrame();
            const s64 stamp = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp
                                                                         : f->pts;
            u64 pts{};
            if (stamp != AV_NOPTS_VALUE) {
                pts = static_cast<u64>(stamp);
                if (const auto it = pending_pts.find(pts); it != pending_pts.end()) {
                    pending_pts.erase(it);
                }
            } else if (!pending_pts.empty()) {
                pts = *pending_pts.begin();
                pending_pts.erase(pending_pts.begin());
            }
            pictures.push_back({static_cast<u32>(f->width), static_cast<u32>(f->height), pts,
                                std::move(frame)});
            received_any = true;
            startup_packets.clear();
        }
    }
};

Nx2VideoDecoder::Nx2VideoDecoder(std::string_view mime) : impl{std::make_unique<Impl>()} {
    impl->name = std::string{mime};
    const auto codec = CodecFromMime(mime);
    if (!codec) {
        LOG_ERROR(HW_GPU, "nx2 video: no decoder for codec \"{}\"", mime);
        return;
    }
    impl->decoder.emplace(*codec);
    if (impl->decoder->GetCodec() == nullptr) {
        LOG_ERROR(HW_GPU, "nx2 video: FFmpeg has no decoder for \"{}\"", mime);
        return;
    }
    // Without a usable hardware decoder, the CPU one (with its frame threads) instead of the same
    // context left without the hardware.
    if (Settings::values.nvdec_emulation.GetValue() == Settings::NvdecEmulation::Gpu &&
        !impl->Open(*codec, true)) {
        LOG_INFO(HW_GPU, "nx2 video: no hardware decoder for \"{}\", using the CPU", mime);
    }
    if (!impl->open) {
        impl->Open(*codec, false);
    }
    if (!impl->open) {
        LOG_INFO(HW_GPU, "nx2 video: decoder for \"{}\" failed to open", mime);
        return;
    }
    const int threads = impl->context->GetCodecContext()->thread_count;
    LOG_INFO(HW_GPU, "nx2 video: decoder for \"{}\" open: {} on the {}, {} threads", mime,
             impl->decoder->GetCodec()->name, impl->using_hardware ? "GPU" : "CPU",
             threads != 0 ? std::to_string(threads) : std::string{"default"});
}

Nx2VideoDecoder::~Nx2VideoDecoder() = default;

bool Nx2VideoDecoder::IsOpen() const {
    return impl->open;
}

const std::string& Nx2VideoDecoder::Name() const {
    return impl->name;
}

void Nx2VideoDecoder::Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures) {
    if (!impl->open || data.empty()) {
        return;
    }
    // A hardware decoder that took this many packets without a frame isn't going to give one.
    if (impl->using_hardware && !impl->received_any &&
        impl->startup_packets.size() >= MaxStartupPackets && !impl->FallBackToCpu(pictures)) {
        return;
    }
    impl->packet.assign(data.size() + AV_INPUT_BUFFER_PADDING_SIZE, 0);
    std::memcpy(impl->packet.data(), data.data(), data.size());
    if (impl->name.find("avc") != std::string::npos || impl->name.find("264") != std::string::npos) {
        impl->h264_packets.Convert(std::span<u8>{impl->packet.data(), data.size()});
    }
    std::span<const u8> bytes{impl->packet.data(), data.size()};
    if (impl->using_hardware && !impl->received_any) {
        impl->startup_packets.emplace_back(pts, std::move(impl->packet));
        bytes = std::span<const u8>{impl->startup_packets.back().second.data(), data.size()};
    }
    if (!impl->Submit(bytes, pts, pictures) && impl->using_hardware && !impl->received_any) {
        impl->FallBackToCpu(pictures);
    }
}

void Nx2VideoDecoder::Drain(std::vector<Nx2Picture>& pictures) {
    if (!impl->open) {
        return;
    }
    const auto drain = [&] {
        const int ret = avcodec_send_packet(impl->context->GetCodecContext(), nullptr);
        if (ret < 0 && ret != AVERROR_EOF && ret != AVERROR(EAGAIN)) {
            return false;
        }
        impl->Receive(pictures);
        return !impl->context->HadDecodeError();
    };
    if (!drain() && impl->using_hardware && !impl->received_any && impl->FallBackToCpu(pictures)) {
        drain();
    }
    Reset();
}

void Nx2VideoDecoder::Reset() {
    if (impl->open) {
        avcodec_flush_buffers(impl->context->GetCodecContext());
    }
    impl->pending_pts.clear();
    impl->h264_packets.Reset();
    impl->received_any = false;
    impl->startup_packets.clear();
}

void Nx2VideoDecoder::Render(const Nx2Picture& picture, Nx2PictureFormat format, u32 width,
                             u32 height, u64 pitch, u64 chroma_offset, std::span<u8> out) {
    const AVFrame* f = picture.frame->GetFrame();
    const u64 needed = format == Nx2PictureFormat::Nv12
                           ? chroma_offset + pitch * ((u64{height} + 1) / 2)
                           : pitch * height;
    // An NV12 row (and its interleaved chroma row, rounded up to whole pairs for odd widths) must
    // fit the pitch, or the scaler writes past the next row and, on the last one, past `out`.
    // EDEN_NX2_NV12_LEGACY=1 drops the check.
    static const bool nv12_legacy = [] {
        const char* value = std::getenv("EDEN_NX2_NV12_LEGACY");
        return value != nullptr && value[0] == '1';
    }();
    const u64 min_pitch =
        format == Nx2PictureFormat::Nv12 && !nv12_legacy ? ((u64{width} + 1) / 2) * 2 : 0;
    if (width == 0 || height == 0 || pitch < min_pitch || out.size() < needed) {
        return;
    }
    const AVPixelFormat target = format == Nx2PictureFormat::Nv12    ? AV_PIX_FMT_NV12
                                 : format == Nx2PictureFormat::Rgba8 ? AV_PIX_FMT_RGBA
                                                                     : AV_PIX_FMT_BGRA;
    // Frames copied back from a host GPU decoder are already NV12. Copy them as they are:
    // FFmpeg 6.1's swscale leaves the chroma plane of an NV12 to NV12 copy at zero, which shows
    // the movies green.
    if (target == AV_PIX_FMT_NV12 && f->format == AV_PIX_FMT_NV12 &&
        static_cast<u32>(f->width) == width && static_cast<u32>(f->height) == height) {
        for (u32 y = 0; y < height; ++y) {
            std::memcpy(out.data() + y * pitch, f->data[0] + y * f->linesize[0], width);
        }
        for (u32 y = 0; y < (height + 1) / 2; ++y) {
            std::memcpy(out.data() + chroma_offset + y * pitch, f->data[1] + y * f->linesize[1],
                        width);
        }
        return;
    }
    impl->scaler = sws_getCachedContext(impl->scaler, f->width, f->height,
                                        static_cast<AVPixelFormat>(f->format),
                                        static_cast<int>(width), static_cast<int>(height), target,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (impl->scaler == nullptr) {
        LOG_ERROR(HW_GPU, "nx2 video: cannot convert {} to {}",
                  av_get_pix_fmt_name(static_cast<AVPixelFormat>(f->format)),
                  av_get_pix_fmt_name(target));
        return;
    }
    // The YUVJ formats are full range even when the frame leaves its range unset.
    const bool full_range_source =
        f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P ||
        f->format == AV_PIX_FMT_YUVJ422P || f->format == AV_PIX_FMT_YUVJ444P ||
        f->format == AV_PIX_FMT_YUVJ440P || f->format == AV_PIX_FMT_YUVJ411P;
    // The stream's matrix and range; untagged HD video is BT.709. NV12 output is limited range.
    // Set for every frame: the cached scaler keeps the previous frame's details otherwise.
    int space = SWS_CS_ITU601;
    if (f->colorspace == AVCOL_SPC_BT709 ||
        (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720)) {
        space = SWS_CS_ITU709;
    } else if (f->colorspace == AVCOL_SPC_BT2020_NCL || f->colorspace == AVCOL_SPC_BT2020_CL) {
        space = SWS_CS_BT2020;
    }
    const int* coefficients = sws_getCoefficients(space);
    sws_setColorspaceDetails(impl->scaler, coefficients, full_range_source, coefficients,
                             format != Nx2PictureFormat::Nv12, 0, 1 << 16, 1 << 16);
    std::array<u8*, 4> planes{out.data(), nullptr, nullptr, nullptr};
    std::array<int, 4> strides{static_cast<int>(pitch), 0, 0, 0};
    if (format == Nx2PictureFormat::Nv12) {
        planes[1] = out.data() + chroma_offset;
        strides[1] = static_cast<int>(pitch);
    }
    sws_scale(impl->scaler, f->data, f->linesize, 0, f->height, planes.data(), strides.data());
}

} // namespace Tegra::Host1x
