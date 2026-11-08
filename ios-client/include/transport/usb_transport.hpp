#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

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
 * @brief Error codes encountered during USB transport lifecycle and data transfers.
 */
enum class UsbTransportError {
    Success = 0,
    InitFailed,
    DeviceNotFound,
    AccessDenied,
    OpenFailed,
    VendorControlFailed,
    InterfaceNotFound,
    KernelDriverDetachFailed,
    ClaimFailed,
    EndpointNotFound,
    StreamingAlreadyActive,
    TransferFailed,
    Timeout,
    PipeError,
    NotConnected,
    Other,
};

/**
 * @brief Cached pair of Bulk-IN and Bulk-OUT endpoints.
 */
struct UsbEndpointPair {
    uint8_t bulk_in{0};
    uint8_t bulk_out{0};

    constexpr bool is_valid() const noexcept {
        return bulk_in != 0 && bulk_out != 0;
    }
};

/**
 * @brief Fixed 256 KB memory buffer structure aligned to 64-byte cache lines.
 *
 * Enforces total size of exactly (256 * 1024) + 64 bytes with 64-byte alignment
 * to match high-throughput USB DMA transfer constraints.
 */
struct alignas(64) RawUsbBlock {
    static constexpr size_t kCapacity = 256 * 1024;

    size_t size{0};
    uint64_t sequence_id{0};
    uint8_t reserved[64 - sizeof(size_t) - sizeof(uint64_t)]{0};
    uint8_t data[kCapacity]{0};

    constexpr RawUsbBlock() = default;

    shared::ReadOnlyBuffer as_span() const noexcept {
        return shared::ReadOnlyBuffer(data, size);
    }

    shared::MutableBuffer as_mutable_span() noexcept {
        return shared::MutableBuffer(data, size);
    }

    operator shared::ReadOnlyBuffer() const noexcept {
        return as_span();
    }

    void reset() noexcept {
        size = 0;
        sequence_id = 0;
    }
};

static_assert(sizeof(RawUsbBlock) == (256 * 1024) + 64, "RawUsbBlock layout mismatch");
static_assert(alignof(RawUsbBlock) == 64, "RawUsbBlock must be 64-byte cacheline aligned");

/**
 * @brief Callback function type delivering streaming buffers to consumer components.
 */
using UsbDataCallback = std::function<void(shared::ReadOnlyBuffer)>;
using RawBlockCallback = std::function<void(const RawUsbBlock&)>;

/**
 * @brief Pure virtual USB transport abstraction separating business logic from raw libusb types.
 */
class IUsbTransport {
public:
    virtual ~IUsbTransport() = default;

    virtual shared::Result<void, UsbTransportError> Initialize() = 0;
    virtual shared::Result<void, UsbTransportError> OpenDevice(
        uint16_t vid, uint16_t pid, std::string_view serial = "") = 0;
    virtual shared::Result<void, UsbTransportError> TriggerCoreMediaMode() = 0;
    virtual shared::Result<void, UsbTransportError> ClaimCoreMediaInterface() = 0;
    virtual shared::Result<void, UsbTransportError> StartStreaming(UsbDataCallback callback) = 0;
    virtual void StopStreaming() noexcept = 0;
    virtual shared::Result<size_t, UsbTransportError> SendBulkData(
        shared::ReadOnlyBuffer data, uint32_t timeout_ms) = 0;
    virtual bool IsConnected() const noexcept = 0;
    virtual UsbEndpointPair GetActiveEndpoints() const noexcept = 0;
};

/**
 * @brief Production implementation of IUsbTransport using libusb-1.0.
 */
class LibUsbTransport : public IUsbTransport {
public:
    LibUsbTransport();
    ~LibUsbTransport() override;

    LibUsbTransport(const LibUsbTransport&) = delete;
    LibUsbTransport& operator=(const LibUsbTransport&) = delete;

    LibUsbTransport(LibUsbTransport&& other) noexcept;
    LibUsbTransport& operator=(LibUsbTransport&& other) noexcept;

    shared::Result<void, UsbTransportError> Initialize() override;
    shared::Result<void, UsbTransportError> OpenDevice(
        uint16_t vid, uint16_t pid, std::string_view serial = "") override;
    shared::Result<void, UsbTransportError> TriggerCoreMediaMode() override;
    shared::Result<void, UsbTransportError> ClaimCoreMediaInterface() override;
    shared::Result<void, UsbTransportError> StartStreaming(UsbDataCallback callback) override;
    shared::Result<void, UsbTransportError> StartStreaming(RawBlockCallback callback);
    void StopStreaming() noexcept override;
    shared::Result<size_t, UsbTransportError> SendBulkData(
        shared::ReadOnlyBuffer data, uint32_t timeout_ms) override;
    bool IsConnected() const noexcept override;
    UsbEndpointPair GetActiveEndpoints() const noexcept override;

    void CloseDevice() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ios::transport
