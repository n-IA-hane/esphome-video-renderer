#pragma once

// Host substitutes for LVGL, FreeRTOS and the display transaction. The test
// compiles the complete production presentation translation unit unchanged
// apart from replacing its hardware-facing class header with this boundary.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

struct lv_area_t { int32_t x1, y1, x2, y2; };
struct lv_display_t {};
struct lv_obj_t { lv_area_t area{0, 0, 31, 23}; lv_obj_t *screen{this}; };
struct lv_event_t { void *user_data; };
inline lv_obj_t screen;
inline lv_display_t lv_display;
inline std::vector<lv_area_t> invalidated;
inline std::vector<uint16_t> pixels(64 * 48, 0x1234);
inline bool semaphore_taken = false;
inline lv_obj_t *lv_screen_active() { return &screen; }
inline lv_obj_t *lv_obj_get_screen(lv_obj_t *object) { return object->screen; }
inline void lv_obj_get_coords(lv_obj_t *object, lv_area_t *area) { *area = object->area; }
inline int lv_area_get_width(const lv_area_t *area) { return area->x2 - area->x1 + 1; }
inline int lv_area_get_height(const lv_area_t *area) { return area->y2 - area->y1 + 1; }
inline int lv_obj_get_content_width(lv_obj_t *object) { return lv_area_get_width(&object->area); }
inline int lv_obj_get_content_height(lv_obj_t *object) { return lv_area_get_height(&object->area); }
inline lv_display_t *lv_obj_get_display(lv_obj_t *) { return &lv_display; }
inline void lv_obj_update_layout(lv_obj_t *) {}
inline void lv_obj_invalidate_area(lv_obj_t *, const lv_area_t *area) { invalidated.push_back(*area); }
inline void lv_obj_invalidate(lv_obj_t *) { invalidated.push_back({0, 0, 63, 47}); }
inline void *lv_event_get_user_data(lv_event_t *event) { return event->user_data; }
inline constexpr int LV_EVENT_REFR_READY = 1;
inline void lv_display_add_event_cb(lv_display_t *, void (*)(lv_event_t *), int, void *) {}
inline constexpr int pdTRUE = 1;
inline int xSemaphoreTake(void *, int) { if (semaphore_taken) return 0; semaphore_taken = true; return pdTRUE; }
inline void xSemaphoreGive(void *) { assert(semaphore_taken); semaphore_taken = false; }

struct ppa_srm_oper_config_t {
  struct { void *buffer{}; int pic_w{}, pic_h{}, block_w{}, block_h{}, srm_cm{}, yuv_range{}, yuv_std{}; } in;
  int rotation_angle{}, mode{};
  float scale_x{}, scale_y{};
};
inline constexpr int PPA_SRM_COLOR_MODE_RGB565 = 0, PPA_SRM_COLOR_MODE_YUV420 = 1;
inline constexpr int PPA_COLOR_RANGE_LIMIT = 0, PPA_COLOR_CONV_STD_RGB_YUV_BT601 = 0;
inline constexpr int PPA_SRM_ROTATION_ANGLE_0 = 0, PPA_TRANS_MODE_BLOCKING = 0;
struct Display {
  unsigned commits{};
  std::function<void()> before_dma;
  bool supports_rgb565_transform() const { return true; }
  bool draw_rgb565_transformed(const ppa_srm_oper_config_t &operation, size_t, int x, int y,
                              int width, int height, bool (*admit)(void *), void *context) {
    assert(semaphore_taken);
    if (before_dma) before_dma();
    if (!admit(context)) return false;
    const uint16_t color = *static_cast<const uint16_t *>(operation.in.buffer);
    for (int yy = y; yy < y + height; ++yy)
      for (int xx = x; xx < x + width; ++xx) pixels.at(yy * 64 + xx) = color;
    ++commits;
    return true;
  }
};
struct Trigger {
  unsigned count{};
  std::function<void()> callback;
  void trigger() { ++count; if (callback) callback(); }
};

namespace esphome::p4_video_renderer {
class P4VideoRenderer {
 public:
  void loop();
  bool present_surface_direct_(int index);
  bool refresh_layout_();
  void attach_video_container(lv_obj_t *container);
  static void display_refresh_ready_callback_(lv_event_t *event);
  void on_display_refresh_ready_();
  bool has_remote_frame() const {
#ifdef USE_P4_VIDEO_RENDERER_JPEG
    return rx_active_ && remote_frame_visible_ && remote_frame_visible_ == rx_session_generation_;
#else
    return rx_active_ && remote_frame_visible_;
#endif
  }
  void disable_loop() { wake = false; }
  void enable_loop_soon_any_context() { wake = true; }
  void mark_failed() { failed = true; }
  bool wake{}, failed{};
  Display *display_{};
  lv_obj_t *video_container_{};
  void *presentation_mutex_{};
  std::atomic<uint32_t> layout_size_{0}, rx_session_generation_{1}, pending_surface_generation_{1};
  std::atomic<uint32_t> remote_frame_visible_{0};
  std::atomic<bool> rx_running_{true}, rx_active_{true}, rx_session_prepared_{true};
  std::atomic<bool> presentation_in_flight_{false}, surface_ever_presented_{false}, video_ended_pending_{false};
  std::atomic<int> pending_surface_{-1};
  std::atomic<unsigned> rx_presented_frames_{0}, rx_refresh_completed_{0};
  lv_area_t presented_area_{};
  bool presented_area_valid_{};
  uint16_t *surfaces_[1]{};
  size_t surface_capacity_bytes_{64 * 48 * 2}, surface_data_size_[1]{};
  int surface_stride_bytes_[1]{}, surface_content_width_[1]{}, surface_content_height_[1]{};
  uint32_t surface_generation_[1]{}, surface_layout_size_[1]{};
  Trigger first_frame_trigger_, video_ended_trigger_;
};
}  // namespace esphome::p4_video_renderer
