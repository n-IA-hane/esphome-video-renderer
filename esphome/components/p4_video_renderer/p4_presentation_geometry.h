#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace esphome::p4_video_renderer {

constexpr int PPA_SCALE_UNITS = 16;

// The display takes a 1/16 scale step and writes an RGB565 output region.
inline int fit_scale_units(int content_width, int content_height,
                           int container_width, int container_height,
                           size_t capacity_bytes) {
  if (content_width <= 0 || content_height <= 0 || container_width <= 0 ||
      container_height <= 0)
    return 0;
  int units = std::min(container_width * PPA_SCALE_UNITS / content_width,
                       container_height * PPA_SCALE_UNITS / content_height);
  const auto output_fits = [&](int value) {
    const size_t width = size_t(content_width) * value / PPA_SCALE_UNITS;
    const size_t height = size_t(content_height) * value / PPA_SCALE_UNITS;
    return width != 0 && height != 0 &&
           width <= capacity_bytes / sizeof(uint16_t) / height;
  };
  if (units < 1)
    return 0;
  if (!output_fits(units)) {
    int low = 1, high = units;
    while (low < high) {
      const int candidate = low + (high - low + 1) / 2;
      if (output_fits(candidate))
        low = candidate;
      else
        high = candidate - 1;
    }
    units = low;
  }
  return output_fits(units) ? units : 0;
}

} // namespace esphome::p4_video_renderer
