#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#if __has_include("types/common_types.hpp")
#include "types/common_types.hpp"
#elif __has_include("shared/include/types/common_types.hpp")
#include "shared/include/types/common_types.hpp"
#else
#include "../../shared/include/types/common_types.hpp"
#endif

#if __has_include("protocol/clock_synchronizer.hpp")
#include "protocol/clock_synchronizer.hpp"
#elif __has_include("ios-client/include/protocol/clock_synchronizer.hpp")
#include "ios-client/include/protocol/clock_synchronizer.hpp"
#else
#include "clock_synchronizer.hpp"
#endif

#if __has_include("transport/usb_transport.hpp")
#include "transport/usb_transport.hpp"
#elif __has_include("ios-client/include/transport/usb_transport.hpp")
#include "ios-client/include/transport/usb_transport.hpp"
#else
#include "usb_transport.hpp"
#endif

namespace ios::protocol {

/**
 * @brief Zero-allocation video sample container sized to accommodate uncompressed/compressed 4K frames.
 */
struct alignas(64) VideoSample {
    static constexpr size_t kMaxPayloadCapacity = 1024 * 1024; // 1 MB per frame

    uint8_t data[kMaxPayloadCapacity]{0};
    size_t size{0};
    uint64_t pts_nanos{0};
    uint64_t duration_nanos{0};
    bool is_keyframe{false};

    constexpr VideoSample() = default;

    shared::ReadOnlyBuffer as_span() const noexcept {
        return shared::ReadOnlyBuffer(data, size);
    }

    shared::MutableBuffer as_mutable_span() noexcept {
        return shared::MutableBuffer(data, size);
    }

    void reset() noexcept {
        size = 0;
        pts_nanos = 0;
        duration_nanos = 0;
        is_keyframe = false;
    }
};

/**
 * @brief Lock-free Single-Producer Single-Consumer circular queue bounded to power-of-two capacity.
 *
 * Uses cacheline-aligned atomic head/tail indices with acquire-release ordering and lambda-based
 * zero-copy push/pop accessors.
 */
template <typename T, size_t Capacity = 64>
class SampleRingBuffer {
    static_assert((Capacity > 0) && ((Capacity & (Capacity - 1)) == 0),
                  "SampleRingBuffer capacity must be a power of two");

public:
    SampleRingBuffer() : slots_(std::make_unique<T[]>(Capacity)) {}
    ~SampleRingBuffer() = default;

    SampleRingBuffer(const SampleRingBuffer&) = delete;
    SampleRingBuffer& operator=(const SampleRingBuffer&) = delete;

    SampleRingBuffer(SampleRingBuffer&& other) noexcept
        : slots_(std::move(other.slots_)),
          head_(other.head_.load(std::memory_order_relaxed)),
          tail_(other.tail_.load(std::memory_order_relaxed)) {}

    SampleRingBuffer& operator=(SampleRingBuffer&& other) noexcept {
        if (this != &other) {
            slots_ = std::move(other.slots_);
            head_.store(other.head_.load(std::memory_order_relaxed), std::memory_order_relaxed);
            tail_.store(other.tail_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    /**
     * @brief Producer push via lambda writer into pre-allocated slot.
     */
    template <typename F>
    bool PushWith(F&& writer) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);
        const size_t current_head = head_.load(std::memory_order_acquire);

        if ((current_tail - current_head) >= Capacity) {
            return false; // Queue full
        }

        if constexpr (std::is_invocable_r_v<bool, F, T&>) {
            if (!writer(slots_[current_tail & kMask])) {
                return false;
            }
        } else {
            writer(slots_[current_tail & kMask]);
        }

        tail_.store(current_tail + 1, std::memory_order_release);
        return true;
    }

    /**
     * @brief Consumer pop via lambda reader from pre-allocated slot.
     */
    template <typename F>
    bool PopWith(F&& reader) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t current_tail = tail_.load(std::memory_order_acquire);

        if (current_head == current_tail) {
            return false; // Queue empty
        }

        if constexpr (std::is_invocable_r_v<bool, F, T&>) {
            if (!reader(slots_[current_head & kMask])) {
                return false;
            }
        } else {
            reader(slots_[current_head & kMask]);
        }

        head_.store(current_head + 1, std::memory_order_release);
        return true;
    }

    bool Empty() const noexcept {
        return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
    }

    bool Full() const noexcept {
        return (tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed)) >= Capacity;
    }

    size_t Size() const noexcept {
        return tail_.load(std::memory_order_relaxed) - head_.load(std::memory_order_relaxed);
    }

    static constexpr size_t capacity() noexcept {
        return Capacity;
    }

    void Reset() noexcept {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    static constexpr size_t kMask = Capacity - 1;

    std::unique_ptr<T[]> slots_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
};

/**
 * @brief Video stream format description extracted from CoreMedia VFMT descriptors.
 */
struct FormatDescriptor {
    uint32_t width{0};
    uint32_t height{0};
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;

    bool is_valid() const noexcept {
        return !sps.empty() && !pps.empty();
    }
};

using FormatChangeCallback = std::function<void(const FormatDescriptor&)>;

/**
 * @brief Continuous stream reassembly over raw USB input, inline handling of SYNC packets,
 * and AVCC-to-Annex-B transcoding into lock-free SPSC queue.
 */
class StreamDemuxer {
public:
    static constexpr size_t kReassemblyCapacity = 512 * 1024; // 512 KB

    explicit StreamDemuxer(transport::IUsbTransport& transport);
    ~StreamDemuxer() noexcept;

    StreamDemuxer(const StreamDemuxer&) = delete;
    StreamDemuxer& operator=(const StreamDemuxer&) = delete;
    StreamDemuxer(StreamDemuxer&&) = delete;
    StreamDemuxer& operator=(StreamDemuxer&&) = delete;

    /**
     * @brief Initializes callback, resets reassembly offset and clock synchronization state.
     */
    void Initialize(FormatChangeCallback format_callback);

    /**
     * @brief Starts background PING keep-alive heartbeat loop (1 Hz) on std::jthread.
     */
    void Start();

    /**
     * @brief Requests stop on heartbeat thread and safely joins.
     */
    void Stop() noexcept;

    /**
     * @brief Ingests raw USB data chunk, appends to reassembly buffer, and dispatches packets.
     */
    void IngestRawData(shared::ReadOnlyBuffer chunk);

    /**
     * @brief Access to lock-free video sample output ring buffer.
     */
    SampleRingBuffer<VideoSample, 64>& GetOutputQueue() noexcept {
        return output_queue_;
    }

    const SampleRingBuffer<VideoSample, 64>& GetOutputQueue() const noexcept {
        return output_queue_;
    }

    /**
     * @brief Access to the underlying clock synchronizer engine.
     */
    ClockSynchronizer& GetClockSynchronizer() noexcept {
        return clock_sync_;
    }

    const ClockSynchronizer& GetClockSynchronizer() const noexcept {
        return clock_sync_;
    }

private:
    void ParsePackets();
    void ProcessCompletePacket(shared::ReadOnlyBuffer packet);
    void ProcessSyncPacket(const wire::PacketHeader& header, shared::ReadOnlyBuffer payload);
    void ProcessAsynPacket(const wire::PacketHeader& header, shared::ReadOnlyBuffer payload);
    bool ExtractAvccSlices(shared::ReadOnlyBuffer avcc_data, uint64_t pts_nanos, uint64_t duration_nanos, bool is_keyframe);
    bool ParseAvccFormatDescriptor(shared::ReadOnlyBuffer vfmt_payload, FormatDescriptor& out_desc);

    transport::IUsbTransport& transport_;
    ClockSynchronizer clock_sync_;
    SampleRingBuffer<VideoSample, 64> output_queue_;
    FormatChangeCallback format_callback_;

    std::unique_ptr<uint8_t[]> reassembly_buffer_;
    size_t reassembly_offset_{0};

    std::jthread ping_thread_;
};

} // namespace ios::protocol
