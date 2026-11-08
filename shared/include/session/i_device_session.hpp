#pragma once

#include <cstdint>
#include <string_view>

#if __has_include("types/common_types.hpp")
#include "types/common_types.hpp"
#elif __has_include("shared/include/types/common_types.hpp")
#include "shared/include/types/common_types.hpp"
#else
#include "../types/common_types.hpp"
#endif

namespace shared {

/**
 * @brief Error categories for cross-platform device mirroring sessions.
 */
enum class DeviceSessionError {
    Success = 0,
    DeviceNotFound,
    DeviceNotPaired,
    InitializationFailed,
    CaptureStartFailed,
    DecoderInitializationFailed,
    PresentationFailed,
    SessionTerminated,
};

/**
 * @brief Target device operating system platforms.
 */
enum class DevicePlatform {
    Android = 0,
    iOS,
};

/**
 * @brief Abstract unified interface for cross-platform mirror sessions.
 *
 * Provides a platform-agnostic lifecycle contract supporting both Android and iOS runtimes.
 * Enforces zero dynamic memory allocations across virtual invocations.
 */
class IDeviceSession {
public:
    virtual ~IDeviceSession() = default;

    /**
     * @brief Performs device discovery, lockdown/transport pairing, and decoder configuration.
     */
    virtual Result<void, DeviceSessionError> Initialize() = 0;

    /**
     * @brief Initiates bulk video stream ingestion and starts background demuxer pump.
     */
    virtual Result<void, DeviceSessionError> StartStreaming() = 0;

    /**
     * @brief Halts background workers, tears down transport endpoints, and unlinks pipeline.
     */
    virtual void Stop() noexcept = 0;

    /**
     * @brief Harvests latest decoded frames and synchronizes presentation display.
     */
    virtual void ProcessPresentation() = 0;

    /**
     * @brief Queries whether the streaming session is actively running.
     */
    [[nodiscard]] virtual bool IsRunning() const noexcept = 0;

    /**
     * @brief Returns the device platform type (Android or iOS).
     */
    [[nodiscard]] virtual DevicePlatform GetPlatform() const noexcept = 0;

    /**
     * @brief Returns the unique identifier (UDID/serial) of the connected device.
     */
    [[nodiscard]] virtual std::string_view GetDeviceId() const noexcept = 0;
};

} // namespace shared
