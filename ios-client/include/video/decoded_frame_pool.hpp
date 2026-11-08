#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

extern "C" {
#include <libavutil/frame.h>
}

#if __has_include("types/common_types.hpp")
#include "types/common_types.hpp"
#elif __has_include("shared/include/types/common_types.hpp")
#include "shared/include/types/common_types.hpp"
#else
#include "../../shared/include/types/common_types.hpp"
#endif

namespace ios::video {

/**
 * @brief Hardware and software decoded video pixel format identifiers.
 */
enum class PixelFormatType : uint32_t {
    YUV420P = 0,
    NV12,
    VAAPI_SURFACE,
    CUDA_PTR,
    Unknown,
};

/**
 * @brief Cacheline-aligned frame descriptor conveying presentation timing and surface dimensions.
 */
struct alignas(64) FrameMetadata {
    uint64_t pts_nanos{0};
    uint64_t decode_time_nanos{0};
    uint32_t width{0};
    uint32_t height{0};
    PixelFormatType pixel_format{PixelFormatType::Unknown};
    bool is_keyframe{false};
    uint32_t sequence_number{0};
};

static_assert(alignof(FrameMetadata) == 64, "FrameMetadata must be 64-byte aligned");

/**
 * @brief Internal pool slot holding pre-allocated AVFrame, metadata, and readiness indicator.
 */
struct FrameSlot {
    AVFrame* frame{nullptr};
    FrameMetadata metadata{};
    alignas(64) std::atomic<bool> is_ready{false};
};

/**
 * @brief Categorized error conditions for decoded frame pool initialization.
 */
enum class FramePoolError {
    Success = 0,
    AllocationFailed,
    InvalidDimensions,
};

/**
 * @brief Lock-free triple-buffering pool utilizing pre-allocated AVFrame slots.
 *
 * Exchanges decoded surfaces between the video decoder and presentation renderer
 * with zero dynamic memory allocation on hot pathways.
 */
class DecodedFramePool {
public:
    static constexpr size_t kSlotCount = 4; // Triple-buffer display chain + 1 scratch decode target

    DecodedFramePool() noexcept = default;
    ~DecodedFramePool() noexcept;

    DecodedFramePool(const DecodedFramePool&) = delete;
    DecodedFramePool& operator=(const DecodedFramePool&) = delete;
    DecodedFramePool(DecodedFramePool&&) = delete;
    DecodedFramePool& operator=(DecodedFramePool&&) = delete;

    /**
     * @brief Allocates kSlotCount AVFrames via av_frame_alloc(), configures geometry, and clears readiness.
     */
    shared::Result<void, FramePoolError> Initialize(int width, int height, int format);

    /**
     * @brief Frees all allocated frames via av_frame_free() and resets atomic indices.
     */
    void Release() noexcept;

    /**
     * @brief Returns current scratch slot AVFrame for direct decoder target submission.
     */
    AVFrame* GetScratchFrame() noexcept;

    /**
     * @brief Commits the scratch slot, publishes metadata with release ordering, and advances scratch slot.
     */
    void PublishFrame(uint64_t pts_nanos, bool is_keyframe) noexcept;

    /**
     * @brief Reads latest ready slot with acquire ordering and executes consumer callback instantaneously.
     */
    bool AcquireLatestFrame(std::function<void(const AVFrame*, const FrameMetadata&)> consumer) noexcept;

    size_t GetSlotCount() const noexcept { return kSlotCount; }
    uint32_t GetWidth() const noexcept { return width_; }
    uint32_t GetHeight() const noexcept { return height_; }
    PixelFormatType GetPixelFormat() const noexcept { return pixel_format_; }
    bool IsInitialized() const noexcept { return is_initialized_; }

private:
    std::array<FrameSlot, kSlotCount> slots_{};
    alignas(64) std::atomic<size_t> latest_ready_idx_{kSlotCount};
    size_t scratch_idx_{0};
    uint32_t sequence_counter_{0};

    uint32_t width_{0};
    uint32_t height_{0};
    PixelFormatType pixel_format_{PixelFormatType::Unknown};
    bool is_initialized_{false};
};

} // namespace ios::video
