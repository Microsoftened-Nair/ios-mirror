#include "video/decoded_frame_pool.hpp"

#include <ctime>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

namespace ios::video {

DecodedFramePool::~DecodedFramePool() noexcept {
    Release();
}

shared::Result<void, FramePoolError> DecodedFramePool::Initialize(int width, int height, int format) {
    Release();

    if (width <= 0 || height <= 0) {
        return FramePoolError::InvalidDimensions;
    }

    width_ = static_cast<uint32_t>(width);
    height_ = static_cast<uint32_t>(height);

    if (format == AV_PIX_FMT_YUV420P) {
        pixel_format_ = PixelFormatType::YUV420P;
    } else if (format == AV_PIX_FMT_NV12) {
        pixel_format_ = PixelFormatType::NV12;
    } else {
        pixel_format_ = PixelFormatType::Unknown;
    }

    for (size_t i = 0; i < kSlotCount; ++i) {
        slots_[i].frame = av_frame_alloc();
        if (!slots_[i].frame) {
            Release();
            return FramePoolError::AllocationFailed;
        }

        slots_[i].frame->width = width;
        slots_[i].frame->height = height;
        slots_[i].frame->format = format;
        slots_[i].is_ready.store(false, std::memory_order_relaxed);
        slots_[i].metadata = FrameMetadata{};
    }

    latest_ready_idx_.store(kSlotCount, std::memory_order_release);
    scratch_idx_ = 0;
    sequence_counter_ = 0;
    is_initialized_ = true;

    return {};
}

void DecodedFramePool::Release() noexcept {
    for (size_t i = 0; i < kSlotCount; ++i) {
        if (slots_[i].frame) {
            av_frame_free(&slots_[i].frame);
            slots_[i].frame = nullptr;
        }
        slots_[i].is_ready.store(false, std::memory_order_relaxed);
        slots_[i].metadata = FrameMetadata{};
    }

    latest_ready_idx_.store(kSlotCount, std::memory_order_release);
    scratch_idx_ = 0;
    sequence_counter_ = 0;
    is_initialized_ = false;
}

AVFrame* DecodedFramePool::GetScratchFrame() noexcept {
    if (!is_initialized_) {
        return nullptr;
    }

    slots_[scratch_idx_].is_ready.store(false, std::memory_order_relaxed);
    return slots_[scratch_idx_].frame;
}

void DecodedFramePool::PublishFrame(uint64_t pts_nanos, bool is_keyframe) noexcept {
    if (!is_initialized_) {
        return;
    }

    auto& slot = slots_[scratch_idx_];
    slot.metadata.pts_nanos = pts_nanos;

    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    slot.metadata.decode_time_nanos = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
                                      static_cast<uint64_t>(ts.tv_nsec);

    slot.metadata.width = (slot.frame && slot.frame->width > 0)
                              ? static_cast<uint32_t>(slot.frame->width)
                              : width_;
    slot.metadata.height = (slot.frame && slot.frame->height > 0)
                               ? static_cast<uint32_t>(slot.frame->height)
                               : height_;
    slot.metadata.pixel_format = pixel_format_;
    slot.metadata.is_keyframe = is_keyframe;
    slot.metadata.sequence_number = ++sequence_counter_;

    slot.is_ready.store(true, std::memory_order_release);
    latest_ready_idx_.store(scratch_idx_, std::memory_order_release);

    scratch_idx_ = (scratch_idx_ + 1) % kSlotCount;
}

bool DecodedFramePool::AcquireLatestFrame(
    std::function<void(const AVFrame*, const FrameMetadata&)> consumer) noexcept {
    const size_t ready_idx = latest_ready_idx_.load(std::memory_order_acquire);
    if (ready_idx >= kSlotCount) {
        return false;
    }

    const auto& slot = slots_[ready_idx];
    if (!slot.is_ready.load(std::memory_order_acquire)) {
        return false;
    }

    if (consumer && slot.frame) {
        consumer(slot.frame, slot.metadata);
        return true;
    }

    return false;
}

} // namespace ios::video
