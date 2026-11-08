#include "presentation/sdl_renderer.hpp"
#include "video/decoded_frame_pool.hpp"

#include <cassert>
#include <cstdlib>
#include <iostream>

int main() {
    std::cout << "[RUN] Testing Presentation Lifecycle & SDL2 Renderer..." << std::endl;

    // Headless mode for headless environments / CI
    setenv("SDL_VIDEODRIVER", "dummy", 1);

    ios::presentation::SdlRenderer renderer;
    assert(!renderer.IsWindowOpen());

    // 1. Initialize renderer
    ios::presentation::RendererConfig config{
        .window_title = "Unified Mirror - Presentation Lifecycle Test",
        .initial_width = 800,
        .initial_height = 600,
        .vsync_enabled = false,
        .resizable = true,
    };

    auto init_res = renderer.Initialize(config);
    assert(init_res.is_ok());
    assert(renderer.IsWindowOpen());
    assert(renderer.GetWindow() != nullptr);
    assert(renderer.GetRenderer() != nullptr);

    // 2. Event Callback Registration
    bool close_requested = false;
    bool window_resized = false;
    renderer.SetEventCallback([&](const ios::presentation::WindowEvent& evt) {
        if (evt.code == ios::presentation::WindowEventCode::CloseRequested) {
            close_requested = true;
        } else if (evt.code == ios::presentation::WindowEventCode::WindowResized) {
            window_resized = true;
        }
    });

    // 3. Viewport Geometry & Letterboxing Computation
    // Case A: Pillarbox (tall mobile screen 1170x2532 inside 800x600 window)
    renderer.UpdateViewport(1170, 2532);
    auto geo = renderer.GetViewportGeometry();
    assert(geo.width > 0);
    assert(geo.height == 600);
    assert(geo.x > 0); // centered horizontally
    assert(geo.y == 0);
    assert(geo.scale_factor > 0.0);

    // Case B: Letterbox (wide screen 1920x1080 inside 800x600 window)
    renderer.UpdateViewport(1920, 1080);
    geo = renderer.GetViewportGeometry();
    assert(geo.width == 800);
    assert(geo.height > 0);
    assert(geo.x == 0);
    assert(geo.y > 0); // centered vertically

    // 4. DecodedFramePool Integration & Multi-Plane Texture Uploads
    ios::video::DecodedFramePool pool;
    auto pool_init = pool.Initialize(640, 480, AV_PIX_FMT_YUV420P);
    assert(pool_init.is_ok());

    // Acquire scratch frame and populate test buffer
    AVFrame* scratch = pool.GetScratchFrame();
    assert(scratch != nullptr);
    int alloc_ret = av_frame_get_buffer(scratch, 32);
    assert(alloc_ret >= 0);

    // Fill planes with test pattern
    for (int y = 0; y < scratch->height; ++y) {
        for (int x = 0; x < scratch->width; ++x) {
            scratch->data[0][y * scratch->linesize[0] + x] = static_cast<uint8_t>((x + y) & 0xFF);
        }
    }
    for (int y = 0; y < scratch->height / 2; ++y) {
        for (int x = 0; x < scratch->width / 2; ++x) {
            scratch->data[1][y * scratch->linesize[1] + x] = 128;
            scratch->data[2][y * scratch->linesize[2] + x] = 128;
        }
    }

    pool.PublishFrame(1'000'000'000ULL, true);

    // Render frame
    auto render_res = renderer.RenderFrame(pool);
    assert(render_res.is_ok());
    assert(render_res.value() == true);
    assert(renderer.GetTexture() != nullptr);

    // 5. Poll Events
    renderer.PollEvents();

    // 6. Sequential Teardown
    renderer.Release();
    assert(!renderer.IsWindowOpen());
    assert(renderer.GetWindow() == nullptr);
    assert(renderer.GetRenderer() == nullptr);
    assert(renderer.GetTexture() == nullptr);

    std::cout << "[PASS] Presentation lifecycle test completed successfully!" << std::endl;
    return 0;
}
