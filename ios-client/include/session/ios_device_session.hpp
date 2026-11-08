#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#if __has_include("session/i_device_session.hpp")
#include "session/i_device_session.hpp"
#elif __has_include("shared/include/session/i_device_session.hpp")
#include "shared/include/session/i_device_session.hpp"
#else
#include "../../../shared/include/session/i_device_session.hpp"
#endif

#if __has_include("session/feed_session.hpp")
#include "session/feed_session.hpp"
#elif __has_include("ios-client/include/session/feed_session.hpp")
#include "ios-client/include/session/feed_session.hpp"
#else
#include "feed_session.hpp"
#endif

#if __has_include("video/decoded_frame_pool.hpp")
#include "video/decoded_frame_pool.hpp"
#elif __has_include("ios-client/include/video/decoded_frame_pool.hpp")
#include "ios-client/include/video/decoded_frame_pool.hpp"
#else
#include "../video/decoded_frame_pool.hpp"
#endif

#if __has_include("video/hardware_decoder.hpp")
#include "video/hardware_decoder.hpp"
#elif __has_include("ios-client/include/video/hardware_decoder.hpp")
#include "ios-client/include/video/hardware_decoder.hpp"
#else
#include "../video/hardware_decoder.hpp"
#endif

#if __has_include("presentation/sdl_renderer.hpp")
#include "presentation/sdl_renderer.hpp"
#elif __has_include("ios-client/include/presentation/sdl_renderer.hpp")
#include "ios-client/include/presentation/sdl_renderer.hpp"
#else
#include "../presentation/sdl_renderer.hpp"
#endif

namespace ios::session {

/**
 * @brief Top-level iOS device session orchestrator and presentation coordinator.
 *
 * Implements the cross-platform IDeviceSession pure virtual interface, coordinating
 * FeedSession, DecodedFramePool, HardwareDecoder, and SdlRenderer into an end-to-end
 * crash-proof video pipeline with dedicated background decode polling.
 */
class IosDeviceSession final : public shared::IDeviceSession {
public:
    explicit IosDeviceSession(
        std::string udid = "",
        std::unique_ptr<FeedSession> feed_session = nullptr,
        std::unique_ptr<video::DecodedFramePool> frame_pool = nullptr,
        std::unique_ptr<video::HardwareDecoder> decoder = nullptr,
        std::unique_ptr<presentation::SdlRenderer> renderer = nullptr);

    ~IosDeviceSession() noexcept override;

    IosDeviceSession(const IosDeviceSession&) = delete;
    IosDeviceSession& operator=(const IosDeviceSession&) = delete;
    IosDeviceSession(IosDeviceSession&&) = delete;
    IosDeviceSession& operator=(IosDeviceSession&&) = delete;

    /**
     * @brief Initializes presentation window, frame pool, hardware decoder, and connects FeedSession.
     */
    shared::Result<void, shared::DeviceSessionError> Initialize() override;

    /**
     * @brief Spawns background decode worker and starts CoreMedia streaming.
     */
    shared::Result<void, shared::DeviceSessionError> StartStreaming() override;

    /**
     * @brief Idempotently halts worker, joins decode thread, stops capture feed, and closes decoder.
     */
    void Stop() noexcept override;

    /**
     * @brief Polls window events and renders latest ready frame to hardware display.
     */
    void ProcessPresentation() override;

    [[nodiscard]] bool IsRunning() const noexcept override {
        return is_running_.load(std::memory_order_acquire);
    }

    [[nodiscard]] shared::DevicePlatform GetPlatform() const noexcept override {
        return shared::DevicePlatform::iOS;
    }

    [[nodiscard]] std::string_view GetDeviceId() const noexcept override {
        return udid_;
    }

    FeedSession* GetFeedSession() noexcept { return feed_session_.get(); }
    video::DecodedFramePool* GetFramePool() noexcept { return frame_pool_.get(); }
    video::HardwareDecoder* GetDecoder() noexcept { return decoder_.get(); }
    presentation::SdlRenderer* GetRenderer() noexcept { return renderer_.get(); }

private:
    void DecodeWorkerLoop(std::stop_token stop_token);
    void ExtractAndInitDecoder(const protocol::VideoSample& sample);

    std::string udid_;
    std::atomic<bool> is_running_{false};

    std::unique_ptr<FeedSession> feed_session_;
    std::unique_ptr<video::DecodedFramePool> frame_pool_;
    std::unique_ptr<video::HardwareDecoder> decoder_;
    std::unique_ptr<presentation::SdlRenderer> renderer_;

    std::jthread decode_worker_;
};

} // namespace ios::session
