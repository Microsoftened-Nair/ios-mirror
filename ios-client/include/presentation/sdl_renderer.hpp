#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include <SDL2/SDL.h>

#if __has_include("types/common_types.hpp")
#include "types/common_types.hpp"
#elif __has_include("shared/include/types/common_types.hpp")
#include "shared/include/types/common_types.hpp"
#else
#include "../../shared/include/types/common_types.hpp"
#endif

#if __has_include("presentation/presentation_types.hpp")
#include "presentation/presentation_types.hpp"
#elif __has_include("ios-client/include/presentation/presentation_types.hpp")
#include "ios-client/include/presentation/presentation_types.hpp"
#else
#include "presentation_types.hpp"
#endif

#if __has_include("video/decoded_frame_pool.hpp")
#include "video/decoded_frame_pool.hpp"
#elif __has_include("ios-client/include/video/decoded_frame_pool.hpp")
#include "ios-client/include/video/decoded_frame_pool.hpp"
#else
#include "../video/decoded_frame_pool.hpp"
#endif

namespace ios::presentation {

/**
 * @brief Error categories for SDL2 window and presentation operations.
 */
enum class RendererError {
    Success = 0,
    WindowCreationFailed,
    RendererCreationFailed,
    TextureCreationFailed,
    TextureUpdateFailed,
    RenderPresentFailed,
};

/**
 * @brief Configuration parameters for SDL2 hardware presentation window.
 */
struct RendererConfig {
    std::string window_title{"iOS Screen Mirror"};
    int32_t initial_width{1170 / 2};
    int32_t initial_height{2532 / 2};
    bool vsync_enabled{true};
    bool resizable{true};
};

/**
 * @brief Hardware-accelerated presentation engine for video surfaces.
 *
 * Manages SDL2 window lifecycle, vsync presentation, aspect-ratio letterboxing,
 * and zero-copy multi-plane streaming texture uploads for YUV420P and NV12 decoded frames.
 */
class SdlRenderer {
public:
    SdlRenderer() noexcept = default;
    ~SdlRenderer() noexcept;

    SdlRenderer(const SdlRenderer&) = delete;
    SdlRenderer& operator=(const SdlRenderer&) = delete;
    SdlRenderer(SdlRenderer&&) = delete;
    SdlRenderer& operator=(SdlRenderer&&) = delete;

    /**
     * @brief Initializes SDL2 video subsystem, window, and hardware-accelerated renderer.
     */
    shared::Result<void, RendererError> Initialize(const RendererConfig& config);

    /**
     * @brief Destroys texture, renderer, window, and releases SDL2 video subsystem.
     */
    void Release() noexcept;

    /**
     * @brief Reallocates streaming SDL_Texture only when dimensions or pixel format change.
     */
    shared::Result<void, RendererError> EnsureTextureAllocated(int32_t width, int32_t height, uint32_t sdl_format);

    /**
     * @brief Computes centered aspect-ratio letterbox rectangle into destination_rect_.
     */
    void UpdateViewport(int32_t frame_width, int32_t frame_height) noexcept;

    /**
     * @brief Ingests latest decoded surface from DecodedFramePool and presents to hardware display.
     */
    shared::Result<bool, RendererError> RenderFrame(video::DecodedFramePool& frame_pool);

    /**
     * @brief Drains pending SDL window and quit events.
     */
    void PollEvents() noexcept;

    /**
     * @brief Sets callback to receive translated WindowEvent notifications.
     */
    void SetEventCallback(std::function<void(const WindowEvent&)> callback) noexcept {
        event_callback_ = std::move(callback);
    }

    /**
     * @brief Thread-safe query whether presentation window remains open.
     */
    [[nodiscard]] bool IsWindowOpen() const noexcept {
        return is_window_open_.load(std::memory_order_acquire);
    }

    /**
     * @brief Returns current letterbox viewport coordinates and scale factor.
     */
    [[nodiscard]] ViewportGeometry GetViewportGeometry() const noexcept {
        return viewport_geometry_;
    }

    /**
     * @brief Translates FFmpeg AVPixelFormat to SDL2 pixel format code.
     */
    static uint32_t AvPixelFormatToSdl(int av_pix_fmt) noexcept;

    SDL_Window* GetWindow() const noexcept { return window_; }
    SDL_Renderer* GetRenderer() const noexcept { return renderer_; }
    SDL_Texture* GetTexture() const noexcept { return texture_; }

private:
    SDL_Window* window_{nullptr};
    SDL_Renderer* renderer_{nullptr};
    SDL_Texture* texture_{nullptr};

    std::atomic<bool> is_window_open_{false};

    int32_t texture_width_{0};
    int32_t texture_height_{0};
    uint32_t texture_format_{0};

    int32_t last_frame_width_{0};
    int32_t last_frame_height_{0};

    SDL_Rect destination_rect_{0, 0, 0, 0};
    ViewportGeometry viewport_geometry_{};

    std::function<void(const WindowEvent&)> event_callback_{};
};

} // namespace ios::presentation
