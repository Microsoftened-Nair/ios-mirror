#include "presentation/sdl_renderer.hpp"

#include <cmath>

extern "C" {
#include <libavutil/pixfmt.h>
}

namespace ios::presentation {

SdlRenderer::~SdlRenderer() noexcept {
    Release();
}

uint32_t SdlRenderer::AvPixelFormatToSdl(int av_pix_fmt) noexcept {
    switch (av_pix_fmt) {
        case AV_PIX_FMT_YUV420P:
            return SDL_PIXELFORMAT_IYUV;
        case AV_PIX_FMT_NV12:
            return SDL_PIXELFORMAT_NV12;
        default:
            return SDL_PIXELFORMAT_UNKNOWN;
    }
}

shared::Result<void, RendererError> SdlRenderer::Initialize(const RendererConfig& config) {
    Release();

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) {
        return RendererError::WindowCreationFailed;
    }

    uint32_t window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;
    if (config.resizable) {
        window_flags |= SDL_WINDOW_RESIZABLE;
    }

    window_ = SDL_CreateWindow(
        config.window_title.c_str(),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        config.initial_width,
        config.initial_height,
        window_flags
    );

    if (!window_) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return RendererError::WindowCreationFailed;
    }

    uint32_t renderer_flags = SDL_RENDERER_ACCELERATED;
    if (config.vsync_enabled) {
        renderer_flags |= SDL_RENDERER_PRESENTVSYNC;
    }

    renderer_ = SDL_CreateRenderer(window_, -1, renderer_flags);
    if (!renderer_) {
        // Fallback without VSync if hardware/driver doesn't support VSync in current environment
        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
        if (!renderer_) {
            renderer_ = SDL_CreateRenderer(window_, -1, 0);
        }
    }

    if (!renderer_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return RendererError::RendererCreationFailed;
    }

    is_window_open_.store(true, std::memory_order_release);
    last_frame_width_ = config.initial_width;
    last_frame_height_ = config.initial_height;
    UpdateViewport(config.initial_width, config.initial_height);

    return {};
}

void SdlRenderer::Release() noexcept {
    is_window_open_.store(false, std::memory_order_release);

    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    SDL_QuitSubSystem(SDL_INIT_VIDEO);

    texture_width_ = 0;
    texture_height_ = 0;
    texture_format_ = 0;
    last_frame_width_ = 0;
    last_frame_height_ = 0;
    destination_rect_ = SDL_Rect{0, 0, 0, 0};
    viewport_geometry_ = ViewportGeometry{};
}

shared::Result<void, RendererError> SdlRenderer::EnsureTextureAllocated(
    int32_t width,
    int32_t height,
    uint32_t sdl_format) {

    if (!renderer_) {
        return RendererError::RendererCreationFailed;
    }

    if (texture_ && texture_width_ == width && texture_height_ == height && texture_format_ == sdl_format) {
        return {};
    }

    if (texture_) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
    }

    texture_ = SDL_CreateTexture(
        renderer_,
        sdl_format,
        SDL_TEXTUREACCESS_STREAMING,
        width,
        height
    );

    if (!texture_) {
        return RendererError::TextureCreationFailed;
    }

    texture_width_ = width;
    texture_height_ = height;
    texture_format_ = sdl_format;
    return {};
}

void SdlRenderer::UpdateViewport(int32_t frame_width, int32_t frame_height) noexcept {
    if (frame_width > 0 && frame_height > 0) {
        last_frame_width_ = frame_width;
        last_frame_height_ = frame_height;
    }

    if (!window_ || last_frame_width_ <= 0 || last_frame_height_ <= 0) {
        return;
    }

    int window_w = 0;
    int window_h = 0;
    SDL_GetWindowSize(window_, &window_w, &window_h);
    if (window_w <= 0 || window_h <= 0) {
        return;
    }

    const double frame_aspect = static_cast<double>(last_frame_width_) / static_cast<double>(last_frame_height_);
    const double window_aspect = static_cast<double>(window_w) / static_cast<double>(window_h);

    int32_t dest_w = 0;
    int32_t dest_h = 0;
    int32_t dest_x = 0;
    int32_t dest_y = 0;

    if (window_aspect > frame_aspect) {
        // Pillarboxing (black bars on left and right)
        dest_h = window_h;
        dest_w = static_cast<int32_t>(std::round(dest_h * frame_aspect));
        dest_x = (window_w - dest_w) / 2;
        dest_y = 0;
    } else {
        // Letterboxing (black bars on top and bottom)
        dest_w = window_w;
        dest_h = static_cast<int32_t>(std::round(dest_w / frame_aspect));
        dest_x = 0;
        dest_y = (window_h - dest_h) / 2;
    }

    destination_rect_ = SDL_Rect{dest_x, dest_y, dest_w, dest_h};

    viewport_geometry_.x = dest_x;
    viewport_geometry_.y = dest_y;
    viewport_geometry_.width = dest_w;
    viewport_geometry_.height = dest_h;
    viewport_geometry_.scale_factor = static_cast<double>(dest_w) / static_cast<double>(last_frame_width_);
}

shared::Result<bool, RendererError> SdlRenderer::RenderFrame(video::DecodedFramePool& frame_pool) {
    if (!renderer_ || !is_window_open_.load(std::memory_order_acquire)) {
        return false;
    }

    bool rendered = false;
    RendererError error = RendererError::Success;

    bool acquired = frame_pool.AcquireLatestFrame([&](const AVFrame* frame, const video::FrameMetadata& /*meta*/) {
        if (!frame || frame->width <= 0 || frame->height <= 0) {
            return;
        }

        uint32_t sdl_fmt = AvPixelFormatToSdl(frame->format);
        if (sdl_fmt == SDL_PIXELFORMAT_UNKNOWN) {
            error = RendererError::TextureUpdateFailed;
            return;
        }

        auto tex_res = EnsureTextureAllocated(frame->width, frame->height, sdl_fmt);
        if (tex_res.is_err()) {
            error = tex_res.error();
            return;
        }

        if (frame->width != last_frame_width_ || frame->height != last_frame_height_) {
            UpdateViewport(frame->width, frame->height);
        }

        int update_ret = 0;
        if (frame->format == AV_PIX_FMT_YUV420P) {
            update_ret = SDL_UpdateYUVTexture(
                texture_,
                nullptr,
                frame->data[0],
                frame->linesize[0],
                frame->data[1],
                frame->linesize[1],
                frame->data[2],
                frame->linesize[2]
            );
        } else if (frame->format == AV_PIX_FMT_NV12) {
            update_ret = SDL_UpdateNVTexture(
                texture_,
                nullptr,
                frame->data[0],
                frame->linesize[0],
                frame->data[1],
                frame->linesize[1]
            );
        } else {
            error = RendererError::TextureUpdateFailed;
            return;
        }

        if (update_ret < 0) {
            error = RendererError::TextureUpdateFailed;
            return;
        }

        SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
        if (SDL_RenderClear(renderer_) < 0) {
            error = RendererError::RenderPresentFailed;
            return;
        }

        if (SDL_RenderCopy(renderer_, texture_, nullptr, &destination_rect_) < 0) {
            error = RendererError::RenderPresentFailed;
            return;
        }

        SDL_RenderPresent(renderer_);
        rendered = true;
    });

    if (error != RendererError::Success) {
        return error;
    }

    return rendered && acquired;
}

void SdlRenderer::PollEvents() noexcept {
    if (!window_) {
        return;
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            is_window_open_.store(false, std::memory_order_release);
        } else if (event.type == SDL_WINDOWEVENT) {
            if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                UpdateViewport(last_frame_width_, last_frame_height_);
            } else if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                is_window_open_.store(false, std::memory_order_release);
            }
        }

        if (event_callback_) {
            WindowEvent wevt{};
            if (event.type == SDL_QUIT) {
                wevt.code = WindowEventCode::CloseRequested;
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    wevt.code = WindowEventCode::WindowResized;
                    wevt.data1 = event.window.data1;
                    wevt.data2 = event.window.data2;
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                    wevt.code = WindowEventCode::FocusGained;
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                    wevt.code = WindowEventCode::FocusLost;
                } else if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                    wevt.code = WindowEventCode::CloseRequested;
                }
            }
            if (wevt.code != WindowEventCode::None) {
                event_callback_(wevt);
            }
        }
    }
}

} // namespace ios::presentation
