#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

#if __has_include("session/device_handshake.hpp")
#include "session/device_handshake.hpp"
#elif __has_include("ios-client/include/session/device_handshake.hpp")
#include "ios-client/include/session/device_handshake.hpp"
#else
#include "device_handshake.hpp"
#endif

#if __has_include("protocol/stream_demuxer.hpp")
#include "protocol/stream_demuxer.hpp"
#elif __has_include("ios-client/include/protocol/stream_demuxer.hpp")
#include "ios-client/include/protocol/stream_demuxer.hpp"
#else
#include "stream_demuxer.hpp"
#endif

#if __has_include("transport/usb_transport.hpp")
#include "transport/usb_transport.hpp"
#elif __has_include("ios-client/include/transport/usb_transport.hpp")
#include "ios-client/include/transport/usb_transport.hpp"
#else
#include "usb_transport.hpp"
#endif

#if __has_include("transport/usbmuxd_client.hpp")
#include "transport/usbmuxd_client.hpp"
#elif __has_include("ios-client/include/transport/usbmuxd_client.hpp")
#include "ios-client/include/transport/usbmuxd_client.hpp"
#else
#include "usbmuxd_client.hpp"
#endif

namespace ios::session {

/**
 * @brief Discrete lifecycle states for the CoreMedia screen capture session.
 */
enum class SessionState : uint32_t {
    Disconnected = 0,
    VerifyingPairing,
    SwitchingCoreMediaMode,
    ClaimingInterface,
    NegotiatingHandshake,
    Streaming,
    Error,
    TearingDown,
};

/**
 * @brief Categorized error results for session initialization and runtime failures.
 */
enum class SessionError {
    Success = 0,
    DeviceNotConnected,
    PairingNotTrusted,
    ModeSwitchFailed,
    InterfaceClaimFailed,
    HandshakeFailed,
    TransportStreamingFailed,
    UnexpectedDisconnection,
};

using FrameAvailableCallback = std::function<void()>;
using FormatListener = std::function<void(const protocol::FormatDescriptor&)>;

/**
 * @brief Top-level coordinator managing device enumeration, lockdown pairing verification,
 * USB mode switching, packet demuxing, and teardown unwind.
 */
class FeedSession {
public:
    explicit FeedSession(std::string target_udid = "");
    FeedSession(std::unique_ptr<transport::UsbmuxdClient> usbmuxd,
                std::unique_ptr<transport::IUsbTransport> transport,
                std::unique_ptr<protocol::StreamDemuxer> demuxer = nullptr,
                std::unique_ptr<DeviceHandshake> handshake = nullptr,
                std::string target_udid = "");
    ~FeedSession() noexcept;

    FeedSession(const FeedSession&) = delete;
    FeedSession& operator=(const FeedSession&) = delete;
    FeedSession(FeedSession&&) = delete;
    FeedSession& operator=(FeedSession&&) = delete;

    /**
     * @brief Connects to usbmuxd, verifies pairing, opens USB transport, and claims AV interface.
     */
    shared::Result<void, SessionError> Connect();

    /**
     * @brief Starts background streaming and executes the device activation handshake.
     */
    shared::Result<void, SessionError> Start(const StreamConfiguration& config, FrameAvailableCallback on_frame);

    /**
     * @brief Strictly non-blocking sample consumption forwarder from demuxer ring buffer.
     */
    bool PollSample(std::function<void(const protocol::VideoSample&)> consumer);

    /**
     * @brief Idempotently halts streaming, disables capture on device, and cleans up resources.
     */
    void Stop() noexcept;

    /**
     * @brief Queries current lifecycle state using atomic acquire semantics.
     */
    SessionState GetState() const noexcept;

    void SetFormatListener(FormatListener listener);
    const protocol::FormatDescriptor& GetActiveFormat() const noexcept;
    const std::string& GetResolvedUdid() const noexcept;

private:
    void OnRawUsbDataReceived(shared::ReadOnlyBuffer chunk);
    void OnFormatChanged(const protocol::FormatDescriptor& desc);

    std::string target_udid_;
    std::string resolved_udid_;
    std::atomic<SessionState> state_{SessionState::Disconnected};

    std::unique_ptr<transport::UsbmuxdClient> usbmuxd_;
    std::unique_ptr<transport::IUsbTransport> transport_;
    std::unique_ptr<protocol::StreamDemuxer> demuxer_;
    std::unique_ptr<DeviceHandshake> handshake_;

    FrameAvailableCallback on_frame_callback_;
    FormatListener format_listener_;
    protocol::FormatDescriptor active_format_;
};

} // namespace ios::session
