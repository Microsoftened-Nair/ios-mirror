#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#if __has_include("types/common_types.hpp")
#include "types/common_types.hpp"
#elif __has_include("shared/include/types/common_types.hpp")
#include "shared/include/types/common_types.hpp"
#else
#include "../../shared/include/types/common_types.hpp"
#endif

#if __has_include("transport/usbmuxd_wire.hpp")
#include "transport/usbmuxd_wire.hpp"
#elif __has_include("ios-client/include/transport/usbmuxd_wire.hpp")
#include "ios-client/include/transport/usbmuxd_wire.hpp"
#else
#include "usbmuxd_wire.hpp"
#endif

namespace ios::transport {

/**
 * @brief Error categories encountered during usbmuxd operations.
 */
enum class UsbmuxdError {
    SocketConnectionFailed,
    HandshakeFailed,
    ProtocolViolation,
    DeviceNotFound,
    DeviceNotPaired,
    PairRecordCorrupted,
    PermissionDenied,
    Timeout,
    BrokenPipe,
};

/**
 * @brief Representation of a connected Apple USB device entry.
 */
struct UsbDeviceEntry {
    uint32_t device_id{0};
    uint32_t product_id{0};
    std::string serial_number;
    uint32_t location_id{0};
    uint32_t connection_speed{0};
};

/**
 * @brief Native POSIX UNIX domain socket client for usbmuxd.
 *
 * Implements move-only RAII socket management, binary & plist packet transport,
 * non-blocking connect timeouts, device enumeration, pairing validation, and TCP port tunneling.
 */
class UsbmuxdClient {
public:
    static constexpr std::string_view kDefaultSocketPath = "/var/run/usbmuxd";

    explicit UsbmuxdClient(std::string socket_path = std::string(kDefaultSocketPath));

    // Move-only RAII semantics: copy is disabled
    UsbmuxdClient(const UsbmuxdClient&) = delete;
    UsbmuxdClient& operator=(const UsbmuxdClient&) = delete;

    // Move operations invalidate source socket handle to -1
    UsbmuxdClient(UsbmuxdClient&& other) noexcept;
    UsbmuxdClient& operator=(UsbmuxdClient&& other) noexcept;

    ~UsbmuxdClient();

    /**
     * @brief Connects to the usbmuxd daemon socket using non-blocking connect with timeout.
     */
    shared::Result<void, UsbmuxdError> Connect(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));

    /**
     * @brief Disconnects and closes the active usbmuxd socket handle.
     */
    void Disconnect() noexcept;

    /**
     * @brief Queries usbmuxd for currently attached USB devices.
     */
    shared::Result<std::vector<UsbDeviceEntry>, UsbmuxdError> EnumerateDevices();

    /**
     * @brief Validates device pairing status by reading and verifying the lockdown pair record.
     */
    shared::Result<bool, UsbmuxdError> VerifyDevicePairing(std::string_view udid);

    /**
     * @brief Reads raw lockdown pair record plist for the specified device UDID.
     * Searches pair record directories (/var/lib/lockdown, $HOME/.lockdown).
     */
    shared::Result<std::string, UsbmuxdError> ReadPairRecord(std::string_view udid);

    /**
     * @brief Requests usbmuxd to tunnel the connection to a target TCP port on the device.
     * On success, ownership of the connected socket descriptor is transferred to the caller.
     */
    shared::Result<int, UsbmuxdError> ConnectToDevicePort(uint32_t device_id, uint16_t port);

    // Configuration and state accessors
    void SetSocketPath(std::string path);
    const std::string& GetSocketPath() const noexcept;

    void SetPairRecordDirectory(std::string dir);
    const std::string& GetPairRecordDirectory() const noexcept;

    bool IsConnected() const noexcept;
    int GetSocket() const noexcept;

private:
    shared::Result<void, UsbmuxdError> SendPacket(uint32_t message_type,
                                                 const std::string& plist_payload);

    shared::Result<std::string, UsbmuxdError> ReceivePacket(uint32_t expected_tag,
                                                           std::chrono::milliseconds timeout);

    int socket_fd_{-1};
    std::string socket_path_;
    std::string pair_record_dir_;
    uint32_t tag_{0};
};

} // namespace ios::transport
