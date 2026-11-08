#include "protocol/clock_synchronizer.hpp"

#include <ctime>

namespace ios::protocol {

ClockSynchronizer::ClockSynchronizer() noexcept
    : is_calibrated_(false),
      active_clock_id_(0),
      device_time_base_(0),
      host_time_base_(0),
      correlation_counter_(1) {}

void ClockSynchronizer::Reset() noexcept {
    is_calibrated_.store(false, std::memory_order_release);
    active_clock_id_ = 0;
    device_time_base_ = 0;
    host_time_base_ = 0;
    correlation_counter_ = 1;
}

uint64_t ClockSynchronizer::GetHostMonotonicNanos() noexcept {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL) + static_cast<uint64_t>(ts.tv_nsec);
}

shared::Result<wire::SyncCwpaReplyPacket, ClockSyncError> ClockSynchronizer::HandleCwpaRequest(
    uint64_t correlation_id, shared::ReadOnlyBuffer payload) {
    if (payload.size() < sizeof(wire::SyncCwpaPayload)) {
        return ClockSyncError::InvalidPacketSize;
    }

    const auto* req = reinterpret_cast<const wire::SyncCwpaPayload*>(payload.data());
    active_clock_id_ = req->clock_id;

    uint64_t host_now = GetHostMonotonicNanos();
    wire::SyncCwpaReplyPacket reply(correlation_id, host_now);
    return reply;
}

shared::Result<wire::SyncTimeReplyPacket, ClockSyncError> ClockSynchronizer::HandleTimeRequest(
    uint64_t correlation_id, shared::ReadOnlyBuffer payload) {
    if (payload.size() < sizeof(wire::SyncTimePayload)) {
        return ClockSyncError::InvalidPacketSize;
    }

    const auto* req = reinterpret_cast<const wire::SyncTimePayload*>(payload.data());

    if (active_clock_id_ != 0 && req->clock_id != active_clock_id_) {
        return ClockSyncError::ClockIdMismatch;
    }
    active_clock_id_ = req->clock_id;

    uint64_t host_now = GetHostMonotonicNanos();

    // Calibrate baseline anchor on first observation
    if (!is_calibrated_.load(std::memory_order_acquire)) {
        device_time_base_ = req->device_time_raw;
        host_time_base_ = host_now;
        is_calibrated_.store(true, std::memory_order_release);
    }

    wire::SyncTimeReplyPacket reply(correlation_id, req->clock_id, req->device_time_raw, host_now);
    return reply;
}

uint64_t ClockSynchronizer::DeviceTimeToHostTime(uint64_t device_nanos) const noexcept {
    if (!is_calibrated_.load(std::memory_order_acquire)) {
        return GetHostMonotonicNanos();
    }

    if (device_nanos >= device_time_base_) {
        uint64_t delta = device_nanos - device_time_base_;
        return host_time_base_ + delta;
    } else {
        uint64_t delta = device_time_base_ - device_nanos;
        return host_time_base_ >= delta ? host_time_base_ - delta : 0;
    }
}

wire::PacketHeader ClockSynchronizer::CreatePingPacket() noexcept {
    uint64_t corr = correlation_counter_++;
    return wire::PacketHeader(
        static_cast<uint32_t>(sizeof(wire::PacketHeader)),
        wire::PacketMagic::Ping,
        corr
    );
}

bool ClockSynchronizer::IsCalibrated() const noexcept {
    return is_calibrated_.load(std::memory_order_acquire);
}

uint32_t ClockSynchronizer::GetActiveClockId() const noexcept {
    return active_clock_id_;
}

uint64_t ClockSynchronizer::GetDeviceTimeBase() const noexcept {
    return device_time_base_;
}

uint64_t ClockSynchronizer::GetHostTimeBase() const noexcept {
    return host_time_base_;
}

} // namespace ios::protocol
