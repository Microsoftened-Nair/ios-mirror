#pragma once

#include <cstdint>
#include <string>

#if __has_include("types/common_types.hpp")
#include "types/common_types.hpp"
#elif __has_include("shared/include/types/common_types.hpp")
#include "shared/include/types/common_types.hpp"
#else
#include "../../shared/include/types/common_types.hpp"
#endif

#if __has_include("session/handshake_wire.hpp")
#include "session/handshake_wire.hpp"
#elif __has_include("ios-client/include/session/handshake_wire.hpp")
#include "ios-client/include/session/handshake_wire.hpp"
#else
#include "handshake_wire.hpp"
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

namespace ios::session {

/**
 * @brief Parameters for QuickTime CoreMedia video stream activation.
 */
struct StreamConfiguration {
    uint32_t target_fps{60};
    bool request_high_quality{true};
    uint32_t screen_width{0};
    uint32_t screen_height{0};
};

/**
 * @brief Categorized error results for the QuickTime handshake negotiation lifecycle.
 */
enum class HandshakeError {
    Success = 0,
    TransportError,
    Timeout,
    UnexpectedPacket,
    MalformedPlist,
    DeviceRejectedConfiguration,
    ClockSyncFailed,
};

/**
 * @brief Synchronous protocol handshake engine orchestrating CoreMedia I/O activation.
 */
class DeviceHandshake {
public:
    DeviceHandshake(transport::IUsbTransport& transport, protocol::ClockSynchronizer& clock_sync);
    ~DeviceHandshake() = default;

    DeviceHandshake(const DeviceHandshake&) = delete;
    DeviceHandshake& operator=(const DeviceHandshake&) = delete;
    DeviceHandshake(DeviceHandshake&&) = delete;
    DeviceHandshake& operator=(DeviceHandshake&&) = delete;

    /**
     * @brief Executes the four-step CoreMedia activation handshake sequentially.
     */
    shared::Result<void, HandshakeError> ExecuteHandshake(const StreamConfiguration& config);

    /**
     * @brief Transmits a 32-byte SYNC clok packet configuring clock domain parameters.
     */
    shared::Result<void, HandshakeError> SendClockParameters();

    /**
     * @brief Transmits a 28-byte SPRP need packet specifying framerate and quality preferences.
     */
    shared::Result<void, HandshakeError> SendStreamParameters(const StreamConfiguration& config);

    /**
     * @brief Constructs and transmits an SPRP vfmt XML property list enabling/disabling media streaming.
     */
    shared::Result<void, HandshakeError> SetStreamEnabled(bool enabled);

private:
    transport::IUsbTransport& transport_;
    protocol::ClockSynchronizer& clock_sync_;
    uint64_t correlation_counter_{1};
};

} // namespace ios::session
