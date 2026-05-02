#pragma once

#include <string>

namespace cta {

struct OverlayElement {
    bool enabled = true;
    double x_percent = 0.0;     // X position (0.0 = left, 1.0 = right)
    double y_percent = 0.0;     // Y position (0.0 = top, 1.0 = bottom)
    double scale = 1.0;         // Scale multiplier
};

struct VideoOverlayConfig {
    OverlayElement board = {true, 1.0, 0.0, 0.3};       // Default: Top-Right, 30% scale
    OverlayElement evalBar = {true, 0.0, 0.0, 1.0};     // Default: Left edge, 100% scale
    OverlayElement pvText = {true, 0.5, 0.95, 1.0};     // Default: Bottom center
    std::string arrowsTarget = "Analysis Board";
};

} // namespace cta
