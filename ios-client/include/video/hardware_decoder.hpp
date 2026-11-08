#pragma once

#include <atomic>
#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/buffer.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#if __has_include("types/common_types.hpp")
#include "types/common_types.hpp"
#elif __has_include("shared/include/types/common_types.hpp")
#include "shared/include/types/common_types.hpp"
#else
#include "../../shared/include/types/common_types.hpp"
#endif

#if __has_include("video/decoded_frame_pool.hpp")
#include "video/decoded_frame_pool.hpp"
#elif __has_include("ios-client/include/video/decoded_frame_pool.hpp")
#include "ios-client/include/video/decoded_frame_pool.hpp"
#else
#include "decoded_frame_pool.hpp"
#endif

namespace ios::protocol {
struct VideoSample;
}

namespace ios::video {

/**
 * @brief Hardware acceleration backend modes for H.264 video decoding.
 */
enum class HardwareAccelerationType {
    Auto = 0,
    Vaapi,
    Cuda,
    Software,
};

/**
 * @brief Error categories for decoder initialization and runtime frame processing.
 */
enum class DecoderError {
    Success = 0,
    CodecNotFound,
    ContextAllocationFailed,
    HardwareDeviceInitFailed,
    ParametersInvalid,
    CodecOpenFailed,
    PacketSubmissionFailed,
    FrameRetrievalFailed,
    DecoderFlushed,
};

/**
 * @brief Configuration parameters for hardware and software video decoding.
 */
struct DecoderConfig {
    HardwareAccelerationType hw_accel{HardwareAccelerationType::Auto};
    std::string vaapi_device_path{"/dev/dri/renderD128"};
    int low_delay_flags{1};
    int thread_count{2};
};

/**
 * @brief Hardware-accelerated and software fallback H.264 video decoder.
 *
 * Configures ultra-low-latency FFmpeg decoding contexts, probes VA-API/NVDEC hardware,
 * formats Annex-B extradata, and directs decoded surfaces into DecodedFramePool.
 */
class HardwareDecoder {
public:
    explicit HardwareDecoder(DecodedFramePool& pool);
    ~HardwareDecoder() noexcept;

    HardwareDecoder(const HardwareDecoder&) = delete;
    HardwareDecoder& operator=(const HardwareDecoder&) = delete;
    HardwareDecoder(HardwareDecoder&&) = delete;
    HardwareDecoder& operator=(HardwareDecoder&&) = delete;

    /**
     * @brief Probes hardware context, formats SPS/PPS extradata, and opens the H.264 codec.
     */
    shared::Result<void, DecoderError> Initialize(shared::ReadOnlyBuffer sps,
                                                 shared::ReadOnlyBuffer pps,
                                                 const DecoderConfig& config = {});

    /**
     * @brief Ingests an H.264 slice or NAL unit packet without copying data.
     */
    shared::Result<void, DecoderError> IngestSample(const protocol::VideoSample& sample);
    shared::Result<void, DecoderError> IngestSample(shared::ReadOnlyBuffer buffer,
                                                   uint64_t pts_nanos,
                                                   bool is_keyframe);

    /**
     * @brief Drains all available decoded frames from FFmpeg into DecodedFramePool.
     */
    shared::Result<void, DecoderError> RetrieveDecodedFrames(uint64_t pts_nanos, bool is_keyframe);

    /**
     * @brief Flushes all pending codec buffers (e.g. on orientation change or stream discontinuity).
     */
    void Flush() noexcept;

    /**
     * @brief Closes the codec context and releases all allocated FFmpeg structures.
     */
    void Close() noexcept;

    bool IsInitialized() const noexcept { return is_initialized_.load(std::memory_order_acquire); }
    HardwareAccelerationType GetActiveAccelerationType() const noexcept;
    enum AVPixelFormat GetTargetHwPixelFormat() const noexcept { return active_hw_pix_fmt_; }
    DecodedFramePool& GetFramePool() noexcept { return pool_; }

private:
    shared::Result<void, DecoderError> InitHardwareContext(HardwareAccelerationType hw_accel,
                                                          const std::string& device_node);
    shared::Result<void, DecoderError> OpenCodec(shared::ReadOnlyBuffer sps,
                                                shared::ReadOnlyBuffer pps,
                                                const DecoderConfig& config);

    static enum AVPixelFormat GetHwFormat(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts);

    DecodedFramePool& pool_;
    AVCodecContext* codec_ctx_{nullptr};
    AVBufferRef* hw_device_ctx_{nullptr};
    AVPacket* packet_{nullptr};

    enum AVHWDeviceType active_hw_type_{AV_HWDEVICE_TYPE_NONE};
    enum AVPixelFormat active_hw_pix_fmt_{AV_PIX_FMT_NONE};

    std::atomic<bool> is_initialized_{false};
    bool is_software_fallback_{false};
};

} // namespace ios::video
