#include "p4_presentation_test_boundary.h"

using esphome::p4_video_renderer::P4VideoRenderer;

static void publish(P4VideoRenderer &renderer, uint16_t *source, int width, int height) {
  assert(renderer.pending_surface_ == -1);
  renderer.surfaces_[0] = source;
  renderer.surface_content_width_[0] = width;
  renderer.surface_content_height_[0] = height;
  renderer.surface_stride_bytes_[0] = width * 2;
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  renderer.surface_data_size_[0] = width * height * 2;
#else
  renderer.surface_data_size_[0] = width * height * 3 / 2;
  renderer.surface_generation_[0] = renderer.rx_session_generation_.load();
  renderer.refresh_layout_();
  renderer.surface_layout_size_[0] = renderer.layout_size_.load();
#endif
  renderer.pending_surface_generation_ = renderer.rx_session_generation_.load();
  renderer.pending_surface_ = 0;
}

static void refresh(P4VideoRenderer &renderer) {
  for (const auto &area : invalidated)
    for (int y = area.y1; y <= area.y2; ++y)
      for (int x = area.x1; x <= area.x2; ++x) pixels.at(y * 64 + x) = 0x1234;
  invalidated.clear();
  renderer.on_display_refresh_ready_();
}

int main() {
  P4VideoRenderer renderer;
  Display display;
  renderer.display_ = &display;
  lv_obj_t container;
  container.screen = &screen;
  renderer.attach_video_container(&container);
  uint16_t source[32 * 24];
  std::fill_n(source, 32 * 24, 0xA1A1);
  publish(renderer, source, 32, 24);
  renderer.loop();
  assert(display.commits == 1 && renderer.pending_surface_ == -1);
  assert(invalidated.empty() && pixels[12 * 64] == 0xA1A1);

  // Identical geometry keeps the direct hot path and performs no repaint.
  publish(renderer, source, 32, 24);
  renderer.loop();
  assert(display.commits == 2 && invalidated.empty());

  // Portrait after landscape exposes two bars. The old source remains leased
  // until LVGL restores its real (nonblack) background, then a later loop draws.
  std::fill_n(source, 32 * 24, 0xC3C3);
  publish(renderer, source, 12, 24);
  renderer.loop();
  assert(display.commits == 2 && renderer.pending_surface_ == 0);
  assert(renderer.presentation_in_flight_ && invalidated.size() == 1);
  renderer.loop();
  assert(display.commits == 2 && invalidated.size() == 1);
  refresh(renderer);
  assert(display.commits == 2 && renderer.pending_surface_ == 0);
  renderer.loop();
  assert(display.commits == 3 && renderer.pending_surface_ == -1);
  assert(pixels[12 * 64] == 0x1234 && pixels[12 * 64 + 16] == 0xC3C3);
  assert(pixels[12 * 64 + 31] == 0x1234);

  // Moving the container restores old pixels even outside the new bounds.
  container.area = {24, 12, 55, 35};
  publish(renderer, source, 12, 24);
  renderer.loop();
  assert(renderer.pending_surface_ == 0 && display.commits == 3);
  refresh(renderer);
  renderer.loop();
  assert(display.commits == 4 && pixels[12 * 64 + 16] == 0x1234);
  assert(pixels[24 * 64 + 40] == 0xC3C3);

  // Stop while waiting cannot commit the stopped session after REFR_READY.
  container.area = {24, 12, 39, 23};
  publish(renderer, source, 12, 24);
  renderer.loop();
  assert(renderer.pending_surface_ == 0 && renderer.presentation_in_flight_);
  renderer.rx_active_ = false;
  ++renderer.rx_session_generation_;
  renderer.pending_surface_ = -1;
  renderer.remote_frame_visible_ = 0;
  renderer.presentation_in_flight_ = false;
  renderer.video_ended_pending_ = true;
  renderer.loop();
  refresh(renderer);
  renderer.loop();
  assert(display.commits == 4 && pixels[24 * 64 + 40] == 0x1234);

  // Redial before the ending repaint holds the new frame until it is safe.
  renderer.rx_active_ = true;
  ++renderer.rx_session_generation_;
  renderer.video_ended_pending_ = true;
  publish(renderer, source, 12, 24);
  renderer.loop();
  assert(display.commits == 4 && renderer.pending_surface_ == 0);
  refresh(renderer);
  renderer.loop();
  assert(display.commits == 5 && renderer.pending_surface_ == -1);
  assert(pixels[18 * 64 + 32] == 0xC3C3);

  // Existing generation admission still protects stop during driver wait.
  publish(renderer, source, 12, 24);
  display.before_dma = [&] { renderer.rx_active_ = false; ++renderer.rx_session_generation_; };
  renderer.loop();
  assert(display.commits == 5 && renderer.pending_surface_ == -1);
}
