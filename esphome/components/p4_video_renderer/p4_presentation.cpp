#include "p4_video_renderer.h"

#ifdef USE_ESP_IDF

#include "p4_presentation_geometry.h"

#include "esphome/core/helpers.h"

namespace esphome::p4_video_renderer {

bool P4VideoRenderer::present_surface_direct_(int index) {
  if (index != 0 || this->display_ == nullptr ||
      !this->display_->supports_rgb565_transform() ||
      this->video_container_ == nullptr || this->surfaces_[index] == nullptr)
    return false;
  const uint32_t generation =
      this->rx_session_generation_.load(std::memory_order_acquire);
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  if (this->pending_surface_generation_.load(std::memory_order_acquire) !=
      generation)
    return false;
#else
  if (this->surface_generation_[index] != generation ||
      this->surface_layout_size_[index] !=
          this->layout_size_.load(std::memory_order_acquire))
    return false;
#endif
  const int content_width = this->surface_content_width_[index];
  const int content_height = this->surface_content_height_[index];
  if (content_width <= 0 || content_height <= 0 ||
      this->surface_data_size_[index] == 0)
    return false;
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  const int stride = this->surface_stride_bytes_[index];
  if (stride < content_width * int(sizeof(uint16_t)) ||
      stride % sizeof(uint16_t) != 0 ||
      this->surface_data_size_[index] > this->surface_capacity_bytes_ ||
      this->surface_data_size_[index] % stride != 0)
    return false;
  const int input_width = stride / sizeof(uint16_t);
  const int input_height = this->surface_data_size_[index] / stride;
  if (content_height > input_height)
    return false;
#else
  const int input_width = content_width, input_height = content_height;
  if (this->surface_data_size_[index] !=
      size_t(input_width) * input_height * 3 / 2)
    return false;
#endif
  lv_area_t area{};
  lv_obj_get_coords(this->video_container_, &area);
  const int container_width = lv_area_get_width(&area);
  const int container_height = lv_area_get_height(&area);
  const int units =
      fit_scale_units(content_width, content_height, container_width,
                      container_height, this->surface_capacity_bytes_);
  if (units == 0)
    return false;
  const int width = content_width * units / PPA_SCALE_UNITS;
  const int height = content_height * units / PPA_SCALE_UNITS;
  const int x = area.x1 + (container_width - width) / 2;
  const int y = area.y1 + (container_height - height) / 2;
  const lv_area_t destination{x, y, x + width - 1, y + height - 1};
  if (this->presented_area_valid_ &&
      (destination.x1 > this->presented_area_.x1 ||
       destination.y1 > this->presented_area_.y1 ||
       destination.x2 < this->presented_area_.x2 ||
       destination.y2 < this->presented_area_.y2)) {
    // Direct display writes are outside LVGL's object tree. Ask its normal
    // painter to restore the old region before drawing a smaller/moved frame;
    // painting black here would destroy the application's actual background.
    // Keep the decoded lease until REFR_READY wakes this main-loop owner.
    this->presented_area_valid_ = false;
    this->presentation_in_flight_.store(true, std::memory_order_release);
    lv_obj_invalidate_area(lv_screen_active(), &this->presented_area_);
    return false;
  }
  ppa_srm_oper_config_t operation{};
  operation.in.buffer = this->surfaces_[index];
  operation.in.pic_w = input_width;
  operation.in.pic_h = input_height;
  operation.in.block_w = content_width;
  operation.in.block_h = content_height;
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  operation.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
#else
  operation.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
  operation.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
  operation.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
#endif
  operation.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  operation.scale_x = operation.scale_y = float(units) / PPA_SCALE_UNITS;
  operation.mode = PPA_TRANS_MODE_BLOCKING;
  struct Admission {
    P4VideoRenderer *renderer;
    uint32_t generation;
    int index;
  } token{this, generation, index};
  const auto still_current = [](void *context) -> bool {
    const auto &token = *static_cast<const Admission *>(context);
    return token.renderer->rx_active_.load(std::memory_order_acquire) &&
           token.renderer->rx_session_prepared_.load(
               std::memory_order_acquire) &&
           token.renderer->rx_session_generation_.load(
               std::memory_order_acquire) == token.generation &&
           token.renderer->pending_surface_.load(std::memory_order_acquire) ==
               token.index;
  };
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  const uint32_t started = micros();
#endif
  const bool success = this->display_->draw_rgb565_transformed(
      operation, this->surface_data_size_[index], x, y, width, height,
      still_current, &token);
  if (success) {
    this->presented_area_ = destination;
    this->presented_area_valid_ = true;
  }
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  update_max(this->rx_present_ppa_max_us_, micros() - started);
#endif
  return success && still_current(&token);
}

void P4VideoRenderer::loop() {
  this->disable_loop();
  this->refresh_layout_();
  if (this->video_ended_pending_.exchange(false, std::memory_order_acq_rel)) {
    this->video_ended_trigger_.trigger();
    this->presented_area_valid_ = false;
    // A quick redial may already have a decoded frame. It must not be
    // committed before this repaint, which would immediately erase it.
    this->presentation_in_flight_.store(true, std::memory_order_release);
    lv_obj_invalidate(lv_screen_active());
  }
  if (xSemaphoreTake(this->presentation_mutex_, 0) != pdTRUE) {
    this->enable_loop_soon_any_context();
    return;
  }
  const int pending = this->pending_surface_.load(std::memory_order_acquire);
  const uint32_t generation =
      this->rx_session_generation_.load(std::memory_order_acquire);
  bool first_frame = false;
  if (this->rx_running_.load(std::memory_order_acquire) &&
      this->rx_active_.load(std::memory_order_acquire) &&
      !this->presentation_in_flight_.load(std::memory_order_acquire) &&
      pending == 0 && this->surfaces_[pending] != nullptr &&
      this->video_container_ != nullptr) {
#ifdef USE_P4_VIDEO_RENDERER_JPEG
    const bool current = this->pending_surface_generation_.load(
                             std::memory_order_acquire) == generation;
#else
    const bool current = this->surface_generation_[pending] == generation &&
                         this->surface_layout_size_[pending] ==
                             this->layout_size_.load(std::memory_order_acquire);
#endif
    if (current) {
      first_frame = !this->has_remote_frame();
#ifdef USE_P4_VIDEO_RENDERER_JPEG
      this->remote_frame_visible_.store(generation, std::memory_order_release);
#else
      this->remote_frame_visible_.store(true, std::memory_order_release);
#endif
      const bool page_active =
          lv_obj_get_screen(this->video_container_) == lv_screen_active();
      if (first_frame && !page_active) {
        // Application callbacks may stop/redial. Keep the decoded buffer
        // pending and let the first page repaint finish before its direct pixel
        // commit.
        this->presentation_in_flight_.store(true, std::memory_order_release);
        xSemaphoreGive(this->presentation_mutex_);
        if (generation ==
                this->rx_session_generation_.load(std::memory_order_acquire) &&
            this->has_remote_frame())
          this->first_frame_trigger_.trigger();
        return;
      }
#if defined(USE_P4_VIDEO_RENDERER_JPEG) && defined(USE_P4_VIDEO_RENDERER_DEBUG)
      if (page_active) {
        const uint32_t waited = micros() - this->pending_published_us_.load(
                                               std::memory_order_relaxed);
        update_max(this->pending_wait_max_us_, waited);
        this->pending_wait_total_us_.fetch_add(waited,
                                               std::memory_order_relaxed);
        this->pending_wait_count_.fetch_add(1, std::memory_order_relaxed);
      }
#endif
      if (page_active && this->present_surface_direct_(pending)) {
        this->rx_presented_frames_.fetch_add(1, std::memory_order_relaxed);
        this->rx_refresh_completed_.fetch_add(1, std::memory_order_relaxed);
        this->surface_ever_presented_.store(true, std::memory_order_release);
      }
    }
    // The driver has completed all accepted DMA before returning. Only now may
    // the worker reuse the single decoded pixel buffer, including on Stop.
    if (!this->presentation_in_flight_.load(std::memory_order_acquire)) {
      int expected = pending;
      this->pending_surface_.compare_exchange_strong(expected, -1,
                                                     std::memory_order_acq_rel);
    }
  }
  xSemaphoreGive(this->presentation_mutex_);
  if (first_frame &&
      generation ==
          this->rx_session_generation_.load(std::memory_order_acquire) &&
      this->has_remote_frame())
    this->first_frame_trigger_.trigger();
}

bool P4VideoRenderer::refresh_layout_() {
  if (this->video_container_ == nullptr)
    return false;
  const int width = lv_obj_get_content_width(this->video_container_);
  const int height = lv_obj_get_content_height(this->video_container_);
  if (width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX)
    return false;
  this->layout_size_.store((uint32_t(width) << 16U) | uint16_t(height),
                           std::memory_order_release);
  return true;
}

void P4VideoRenderer::attach_video_container(lv_obj_t *container) {
  if (container == nullptr || this->video_container_ != nullptr)
    return;
  this->video_container_ = container;
  lv_display_t *display = lv_obj_get_display(container);
  if (display == nullptr) {
    this->mark_failed();
    return;
  }
  lv_display_add_event_cb(display,
                          P4VideoRenderer::display_refresh_ready_callback_,
                          LV_EVENT_REFR_READY, this);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  lv_display_add_event_cb(display, P4VideoRenderer::display_debug_callback_,
                          LV_EVENT_RENDER_START, this);
  lv_display_add_event_cb(display, P4VideoRenderer::display_debug_callback_,
                          LV_EVENT_RENDER_READY, this);
  lv_display_add_event_cb(display, P4VideoRenderer::display_debug_callback_,
                          LV_EVENT_FLUSH_START, this);
  lv_display_add_event_cb(display, P4VideoRenderer::display_debug_callback_,
                          LV_EVENT_FLUSH_FINISH, this);
#endif
  lv_obj_update_layout(container);
  this->refresh_layout_();
  if (this->pending_surface_.load(std::memory_order_acquire) >= 0)
    this->enable_loop_soon_any_context();
}

#ifdef USE_P4_VIDEO_RENDERER_DEBUG

void P4VideoRenderer::display_debug_callback_(lv_event_t *event) {
  auto *renderer =
      static_cast<P4VideoRenderer *>(lv_event_get_user_data(event));
  if (renderer != nullptr) {
    renderer->on_display_debug_event_(
        lv_event_get_code(event),
        static_cast<const lv_area_t *>(lv_event_get_param(event)));
  }
}

void P4VideoRenderer::on_display_debug_event_(lv_event_code_t code,
                                              const lv_area_t *area) {
  if (!this->rx_active_.load(std::memory_order_acquire) ||
      !this->rx_session_prepared_.load(std::memory_order_acquire))
    return;
  const uint32_t now = micros();
  if (code == LV_EVENT_RENDER_START) {
    this->debug_render_started_us_.store(now, std::memory_order_relaxed);
  } else if (code == LV_EVENT_RENDER_READY) {
    const uint32_t started =
        this->debug_render_started_us_.exchange(0, std::memory_order_relaxed);
    if (started == 0)
      return;
    const uint32_t elapsed = now - started;
    this->debug_render_count_.fetch_add(1, std::memory_order_relaxed);
    this->debug_render_total_us_.fetch_add(elapsed, std::memory_order_relaxed);
    update_max(this->debug_render_max_us_, elapsed);
  } else if (code == LV_EVENT_FLUSH_START) {
    this->debug_flush_started_us_.store(now, std::memory_order_relaxed);
    if (area != nullptr) {
      this->debug_flush_width_.store(area->x2 - area->x1 + 1,
                                     std::memory_order_relaxed);
      this->debug_flush_height_.store(area->y2 - area->y1 + 1,
                                      std::memory_order_relaxed);
    }
  } else if (code == LV_EVENT_FLUSH_FINISH) {
    const uint32_t started =
        this->debug_flush_started_us_.exchange(0, std::memory_order_relaxed);
    if (started == 0)
      return;
    const uint32_t elapsed = now - started;
    this->debug_flush_count_.fetch_add(1, std::memory_order_relaxed);
    this->debug_flush_total_us_.fetch_add(elapsed, std::memory_order_relaxed);
    update_max(this->debug_flush_max_us_, elapsed);
  }
}
#endif

void P4VideoRenderer::display_refresh_ready_callback_(lv_event_t *event) {
  auto *renderer =
      static_cast<P4VideoRenderer *>(lv_event_get_user_data(event));
  if (renderer != nullptr)
    renderer->on_display_refresh_ready_();
}

void P4VideoRenderer::on_display_refresh_ready_() {
  // LVGL's page/background flush is complete. Pixels are committed by our
  // main-loop owner, not by this callback or by the decoder worker.
  this->presentation_in_flight_.store(false, std::memory_order_release);
  if (this->rx_running_.load(std::memory_order_acquire) &&
      this->rx_active_.load(std::memory_order_acquire) &&
      this->pending_surface_.load(std::memory_order_acquire) >= 0)
    this->enable_loop_soon_any_context();
}

} // namespace esphome::p4_video_renderer

#endif
