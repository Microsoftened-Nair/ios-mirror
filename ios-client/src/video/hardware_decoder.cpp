#include "video/hardware_decoder.hpp"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
}

#if __has_include("protocol/stream_demuxer.hpp")
#include "protocol/stream_demuxer.hpp"
#elif __has_include("ios-client/include/protocol/stream_demuxer.hpp")
#include "ios-client/include/protocol/stream_demuxer.hpp"
#endif

namespace ios::video {

HardwareDecoder::HardwareDecoder(DecodedFramePool& pool)
    : pool_(pool) {
    packet_ = av_packet_alloc();
}

HardwareDecoder::~HardwareDecoder() noexcept {
    Close();
}

void HardwareDecoder::Close() noexcept {
    is_initialized_.store(false, std::memory_order_release);

    if (codec_ctx_) {
        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }
    if (packet_) {
        av_packet_free(&packet_);
        packet_ = nullptr;
    }

    active_hw_type_ = AV_HWDEVICE_TYPE_NONE;
    active_hw_pix_fmt_ = AV_PIX_FMT_NONE;
    is_software_fallback_ = false;
}

void HardwareDecoder::Flush() noexcept {
    if (codec_ctx_ && is_initialized_.load(std::memory_order_acquire)) {
        avcodec_flush_buffers(codec_ctx_);
    }
    if (packet_) {
        av_packet_unref(packet_);
        packet_->data = nullptr;
        packet_->size = 0;
    }
}

HardwareAccelerationType HardwareDecoder::GetActiveAccelerationType() const noexcept {
    if (is_software_fallback_) {
        return HardwareAccelerationType::Software;
    }
    if (active_hw_type_ == AV_HWDEVICE_TYPE_VAAPI) {
        return HardwareAccelerationType::Vaapi;
    }
    if (active_hw_type_ == AV_HWDEVICE_TYPE_CUDA) {
        return HardwareAccelerationType::Cuda;
    }
    return HardwareAccelerationType::Software;
}

shared::Result<void, DecoderError> HardwareDecoder::InitHardwareContext(
    HardwareAccelerationType hw_accel,
    const std::string& device_node) {

    if (hw_device_ctx_) {
        av_buffer_unref(&hw_device_ctx_);
        hw_device_ctx_ = nullptr;
    }

    is_software_fallback_ = false;
    active_hw_type_ = AV_HWDEVICE_TYPE_NONE;
    active_hw_pix_fmt_ = AV_PIX_FMT_NONE;

    if (hw_accel == HardwareAccelerationType::Software) {
        is_software_fallback_ = true;
        return {};
    }

    auto try_vaapi = [this, &device_node]() -> bool {
        const char* dev = device_node.empty() ? nullptr : device_node.c_str();
        int ret = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_VAAPI, dev, nullptr, 0);
        if (ret >= 0 && hw_device_ctx_) {
            active_hw_type_ = AV_HWDEVICE_TYPE_VAAPI;
            active_hw_pix_fmt_ = AV_PIX_FMT_VAAPI;
            return true;
        }
        return false;
    };

    auto try_cuda = [this]() -> bool {
        int ret = av_hwdevice_ctx_create(&hw_device_ctx_, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
        if (ret >= 0 && hw_device_ctx_) {
            active_hw_type_ = AV_HWDEVICE_TYPE_CUDA;
            active_hw_pix_fmt_ = AV_PIX_FMT_CUDA;
            return true;
        }
        return false;
    };

    if (hw_accel == HardwareAccelerationType::Auto) {
        if (try_vaapi() || try_cuda()) {
            return {};
        }
        // Seamless fallback to software if hardware device creation fails
        is_software_fallback_ = true;
        return {};
    }

    if (hw_accel == HardwareAccelerationType::Vaapi) {
        if (try_vaapi()) {
            return {};
        }
        // Fallback to software seamlessly
        is_software_fallback_ = true;
        return {};
    }

    if (hw_accel == HardwareAccelerationType::Cuda) {
        if (try_cuda()) {
            return {};
        }
        // Fallback to software seamlessly
        is_software_fallback_ = true;
        return {};
    }

    is_software_fallback_ = true;
    return {};
}

enum AVPixelFormat HardwareDecoder::GetHwFormat(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    const auto* self = static_cast<const HardwareDecoder*>(ctx->opaque);
    if (!self) {
        return AV_PIX_FMT_NONE;
    }

    const enum AVPixelFormat target_fmt = self->active_hw_pix_fmt_;
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == target_fmt) {
            return *p;
        }
    }

    // Fall back to software pixel formats if the hardware surface format is not offered
    for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p) {
        if (*p == AV_PIX_FMT_YUV420P || *p == AV_PIX_FMT_NV12) {
            return *p;
        }
    }

    return pix_fmts[0];
}

shared::Result<void, DecoderError> HardwareDecoder::OpenCodec(
    shared::ReadOnlyBuffer sps,
    shared::ReadOnlyBuffer pps,
    const DecoderConfig& config) {

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        return DecoderError::CodecNotFound;
    }

    codec_ctx_ = avcodec_alloc_context3(codec);
    if (!codec_ctx_) {
        return DecoderError::ContextAllocationFailed;
    }

    codec_ctx_->opaque = this;

    if (config.low_delay_flags) {
        codec_ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
        codec_ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    }

    if (!is_software_fallback_ && hw_device_ctx_) {
        codec_ctx_->hw_device_ctx = av_buffer_ref(hw_device_ctx_);
        if (!codec_ctx_->hw_device_ctx) {
            avcodec_free_context(&codec_ctx_);
            return DecoderError::HardwareDeviceInitFailed;
        }
        codec_ctx_->get_format = &HardwareDecoder::GetHwFormat;
        codec_ctx_->thread_count = 1;
    } else {
        codec_ctx_->thread_count = is_software_fallback_ ? 4 : (config.thread_count > 0 ? config.thread_count : 4);
        codec_ctx_->thread_type = FF_THREAD_SLICE;
    }

    auto strip_start_code = [](shared::ReadOnlyBuffer buf) -> shared::ReadOnlyBuffer {
        if (buf.size() >= 4 && buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x00 && buf[3] == 0x01) {
            return buf.subspan(4);
        }
        if (buf.size() >= 3 && buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x01) {
            return buf.subspan(3);
        }
        return buf;
    };

    auto clean_sps = strip_start_code(sps);
    auto clean_pps = strip_start_code(pps);

    if (clean_sps.empty() || clean_pps.empty()) {
        avcodec_free_context(&codec_ctx_);
        return DecoderError::ParametersInvalid;
    }

    const size_t total_size = 4 + clean_sps.size() + 4 + clean_pps.size();
    uint8_t* extradata = static_cast<uint8_t*>(av_mallocz(total_size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!extradata) {
        avcodec_free_context(&codec_ctx_);
        return DecoderError::ContextAllocationFailed;
    }

    static constexpr uint8_t kStartCode[4] = {0x00, 0x00, 0x00, 0x01};
    size_t offset = 0;
    std::memcpy(extradata + offset, kStartCode, 4);
    offset += 4;
    std::memcpy(extradata + offset, clean_sps.data(), clean_sps.size());
    offset += clean_sps.size();
    std::memcpy(extradata + offset, kStartCode, 4);
    offset += 4;
    std::memcpy(extradata + offset, clean_pps.data(), clean_pps.size());
    offset += clean_pps.size();

    codec_ctx_->extradata = extradata;
    codec_ctx_->extradata_size = static_cast<int>(total_size);

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "tune", "zerolatency", 0);

    int ret = avcodec_open2(codec_ctx_, codec, &opts);
    av_dict_free(&opts);

    if (ret < 0) {
        // If hardware acceleration failed at codec open in Auto mode, seamlessly fallback to software
        if (!is_software_fallback_ && config.hw_accel == HardwareAccelerationType::Auto) {
            avcodec_free_context(&codec_ctx_);
            if (hw_device_ctx_) {
                av_buffer_unref(&hw_device_ctx_);
                hw_device_ctx_ = nullptr;
            }
            is_software_fallback_ = true;
            active_hw_type_ = AV_HWDEVICE_TYPE_NONE;
            active_hw_pix_fmt_ = AV_PIX_FMT_NONE;
            return OpenCodec(sps, pps, config);
        }

        avcodec_free_context(&codec_ctx_);
        return DecoderError::CodecOpenFailed;
    }

    if (codec_ctx_->priv_data) {
        av_opt_set(codec_ctx_->priv_data, "tune", "zerolatency", 0);
    }

    return {};
}

shared::Result<void, DecoderError> HardwareDecoder::Initialize(
    shared::ReadOnlyBuffer sps,
    shared::ReadOnlyBuffer pps,
    const DecoderConfig& config) {

    Close();

    if (sps.empty() || pps.empty() || sps.data() == nullptr || pps.data() == nullptr) {
        return DecoderError::ParametersInvalid;
    }

    packet_ = av_packet_alloc();
    if (!packet_) {
        return DecoderError::ContextAllocationFailed;
    }

    auto hw_res = InitHardwareContext(config.hw_accel, config.vaapi_device_path);
    if (hw_res.is_err()) {
        Close();
        return hw_res.error();
    }

    auto open_res = OpenCodec(sps, pps, config);
    if (open_res.is_err()) {
        Close();
        return open_res.error();
    }

    is_initialized_.store(true, std::memory_order_release);
    return {};
}

shared::Result<void, DecoderError> HardwareDecoder::IngestSample(
    shared::ReadOnlyBuffer buffer,
    uint64_t pts_nanos,
    bool is_keyframe) {

    if (!is_initialized_.load(std::memory_order_acquire) || !codec_ctx_ || !packet_) {
        return DecoderError::CodecOpenFailed;
    }
    if (buffer.empty() || buffer.data() == nullptr) {
        return DecoderError::ParametersInvalid;
    }

    packet_->data = const_cast<uint8_t*>(buffer.data());
    packet_->size = static_cast<int>(buffer.size());
    packet_->pts = static_cast<int64_t>(pts_nanos);
    packet_->dts = packet_->pts;

    if (is_keyframe) {
        packet_->flags |= AV_PKT_FLAG_KEY;
    } else {
        packet_->flags &= ~AV_PKT_FLAG_KEY;
    }

    int ret = avcodec_send_packet(codec_ctx_, packet_);
    if (ret == AVERROR(EAGAIN)) {
        auto drain_res = RetrieveDecodedFrames(pts_nanos, is_keyframe);
        if (drain_res.is_err()) {
            packet_->data = nullptr;
            packet_->size = 0;
            return drain_res;
        }
        ret = avcodec_send_packet(codec_ctx_, packet_);
    }

    packet_->data = nullptr;
    packet_->size = 0;

    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        return DecoderError::PacketSubmissionFailed;
    }

    return RetrieveDecodedFrames(pts_nanos, is_keyframe);
}

shared::Result<void, DecoderError> HardwareDecoder::IngestSample(const protocol::VideoSample& sample) {
    return IngestSample(sample.as_span(), sample.pts_nanos, sample.is_keyframe);
}

shared::Result<void, DecoderError> HardwareDecoder::RetrieveDecodedFrames(
    uint64_t pts_nanos,
    bool is_keyframe) {

    if (!codec_ctx_ || !is_initialized_.load(std::memory_order_acquire)) {
        return DecoderError::CodecOpenFailed;
    }

    while (true) {
        AVFrame* scratch = pool_.GetScratchFrame();
        if (!scratch) {
            return DecoderError::FrameRetrievalFailed;
        }

        int ret = avcodec_receive_frame(codec_ctx_, scratch);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            return DecoderError::FrameRetrievalFailed;
        }

        uint64_t frame_pts = pts_nanos;
        if (scratch->pts != AV_NOPTS_VALUE && scratch->pts >= 0) {
            frame_pts = static_cast<uint64_t>(scratch->pts);
        }

        bool frame_is_key = is_keyframe;
#if defined(AV_FRAME_FLAG_KEY)
        if ((scratch->flags & AV_FRAME_FLAG_KEY) != 0) {
            frame_is_key = true;
        }
#endif

        pool_.PublishFrame(frame_pts, frame_is_key);
    }

    return {};
}

} // namespace ios::video
