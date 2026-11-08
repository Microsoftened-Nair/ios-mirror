#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>

#if __has_include("types/common_types.hpp")
#include "types/common_types.hpp"
#elif __has_include("shared/include/types/common_types.hpp")
#include "shared/include/types/common_types.hpp"
#else
#include "../../shared/include/types/common_types.hpp"
#endif

#define SyncTimePayload SyncTimePayload_Base
#define SyncCwpaPayload SyncCwpaPayload_Base
#if __has_include("protocol/coremedia_protocol.hpp")
#include "protocol/coremedia_protocol.hpp"
#elif __has_include("ios-client/include/protocol/coremedia_protocol.hpp")
#include "ios-client/include/protocol/coremedia_protocol.hpp"
#else
#include "coremedia_protocol.hpp"
#endif
#undef SyncTimePayload
#undef SyncCwpaPayload

namespace ios::protocol::wire {

#pragma pack(push, 1)

/**
 * @brief 16-byte payload for SYNC CWPA clock synchronization requests.
 */
struct SyncCwpaPayload : public SyncCwpaPayload_Base {
    using SyncCwpaPayload_Base::SyncCwpaPayload_Base;

    constexpr SyncCwpaPayload() = default;
    constexpr SyncCwpaPayload(uint32_t st, uint32_t clk, uint64_t corr) noexcept {
        this->sub_type = st;
        this->clock_id = clk;
        this->correlation_id = corr;
    }
};

/**
 * @brief 16-byte payload for SYNC TIME clock synchronization requests.
 */
struct SyncTimePayload : public SyncTimePayload_Base {
    using SyncTimePayload_Base::SyncTimePayload_Base;

    constexpr SyncTimePayload() = default;
    constexpr SyncTimePayload(uint32_t st, uint32_t clk, uint64_t dt) noexcept {
        this->sub_type = st;
        this->clock_id = clk;
        this->device_time_raw = dt;
    }
};

#pragma pack(pop)

static_assert(sizeof(SyncCwpaPayload) == 16, "SyncCwpaPayload must be 16 bytes");
static_assert(sizeof(SyncTimePayload) == 16, "SyncTimePayload must be 16 bytes");

} // namespace ios::protocol::wire

namespace ios::protocol {

/**
 * @brief Error categories for clock synchronization and time mapping operations.
 */
enum class ClockSyncError {
    Success = 0,
    InvalidPacketSize,
    ClockIdMismatch,
    TransportWriteFailed,
    ArithmeticOverflow,
};

/**
 * @brief Hardware clock synchronizer and time mapping engine for CoreMedia I/O.
 *
 * Synchronizes iOS device clock domain with host CLOCK_MONOTONIC_RAW, responding
 * to SYNC CWPA and TIME requests with zero heap allocations and thread-safe calibration.
 */
class ClockSynchronizer {
public:
    ClockSynchronizer() noexcept;
    ~ClockSynchronizer() = default;

    ClockSynchronizer(const ClockSynchronizer&) = delete;
    ClockSynchronizer& operator=(const ClockSynchronizer&) = delete;
    ClockSynchronizer(ClockSynchronizer&&) noexcept = default;
    ClockSynchronizer& operator=(ClockSynchronizer&&) noexcept = default;

    /**
     * @brief Resets all calibration anchors, active clock ID, and resets correlation counter.
     */
    void Reset() noexcept;

    /**
     * @brief Handles a SYNC CWPA request, recording clock ID and returning a CWPA reply packet.
     */
    shared::Result<wire::SyncCwpaReplyPacket, ClockSyncError> HandleCwpaRequest(
        uint64_t correlation_id, shared::ReadOnlyBuffer payload);

    /**
     * @brief Handles a SYNC TIME request, calibrating the anchor if not calibrated,
     * and returning a 48-byte TIME reply packet with correlated host timestamp.
     */
    shared::Result<wire::SyncTimeReplyPacket, ClockSyncError> HandleTimeRequest(
        uint64_t correlation_id, shared::ReadOnlyBuffer payload);

    /**
     * @brief Maps iOS device timestamp in nanoseconds to host CLOCK_MONOTONIC_RAW nanoseconds.
     * Falls back to current host timestamp if not calibrated.
     */
    uint64_t DeviceTimeToHostTime(uint64_t device_nanos) const noexcept;

    /**
     * @brief Creates a 16-byte PING packet with an incremented correlation ID.
     */
    wire::PacketHeader CreatePingPacket() noexcept;

    /**
     * @brief Returns whether the clock synchronizer has a calibrated anchor.
     */
    bool IsCalibrated() const noexcept;

    /**
     * @brief Returns current host monotonic timestamp in nanoseconds using CLOCK_MONOTONIC_RAW.
     */
    static uint64_t GetHostMonotonicNanos() noexcept;

    uint32_t GetActiveClockId() const noexcept;
    uint64_t GetDeviceTimeBase() const noexcept;
    uint64_t GetHostTimeBase() const noexcept;

private:
    std::atomic<bool> is_calibrated_{false};
    uint32_t active_clock_id_{0};
    uint64_t device_time_base_{0};
    uint64_t host_time_base_{0};
    uint64_t correlation_counter_{1};
};

} // namespace ios::protocol
