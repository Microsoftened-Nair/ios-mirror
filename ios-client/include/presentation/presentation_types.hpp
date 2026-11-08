#pragma once

#include <cstdint>

namespace ios::presentation {

/**
 * @brief Cache-line aligned viewport geometry and scale factor for SDL presentation.
 *
 * Aligned to 32 bytes to prevent false sharing during render poll operations.
 */
struct alignas(32) ViewportGeometry {
    int32_t x{0};
    int32_t y{0};
    int32_t width{0};
    int32_t height{0};
    double scale_factor{1.0};
};

static_assert(alignof(ViewportGeometry) == 32, "ViewportGeometry must be 32-byte aligned");

/**
 * @brief Categorized window presentation and input lifecycle events.
 */
enum class WindowEventCode {
    None = 0,
    WindowResized,
    FocusGained,
    FocusLost,
    CloseRequested,
};

/**
 * @brief POD representation of SDL window events and dimension/coordinate payloads.
 */
struct WindowEvent {
    WindowEventCode code{WindowEventCode::None};
    int32_t data1{0};
    int32_t data2{0};
};

} // namespace ios::presentation
