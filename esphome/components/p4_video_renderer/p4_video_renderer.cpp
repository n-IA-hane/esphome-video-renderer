#include "p4_video_renderer.h"

#ifdef USE_ESP_IDF

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "esp_heap_caps.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace esphome::p4_video_renderer {

static const char *const TAG = "p4_video_renderer";

namespace {

#ifdef USE_P4_VIDEO_RENDERER_JPEG
void *alloc_surface(size_t bytes, size_t reserve_after) {
  const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  if (largest < bytes || free < bytes + reserve_after)
    return nullptr;
  return heap_caps_aligned_alloc(
      64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
}
#endif

#ifdef USE_P4_VIDEO_RENDERER_H264
void *alloc_psram_dma(size_t bytes) {
  return heap_caps_aligned_alloc(
      64, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_DMA);
}
#endif

#ifdef USE_P4_VIDEO_RENDERER_DEBUG
void update_max(std::atomic<uint32_t> &maximum, uint32_t value) {
  uint32_t current = maximum.load(std::memory_order_relaxed);
  while (value > current && !maximum.compare_exchange_weak(
                                current, value, std::memory_order_relaxed)) {
  }
}
#endif

} // namespace

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
  if (container_width <= 0 || container_height <= 0)
    return false;
  // Preserve ca1's integer aspect-fit and output budget even though the driver
  // now owns the final framebuffer; no scaled intermediate is allocated here.
  constexpr int SCALE_UNITS = 16;
  int units = std::min(container_width * SCALE_UNITS / content_width,
                       container_height * SCALE_UNITS / content_height);
  const auto output_fits = [&](int value) {
    const size_t width = size_t(content_width) * value / SCALE_UNITS;
    const size_t height = size_t(content_height) * value / SCALE_UNITS;
    return width != 0 && height != 0 &&
           width <= this->surface_capacity_bytes_ / sizeof(uint16_t) / height;
  };
  if (units < 1)
    return false;
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
  if (!output_fits(units))
    return false;
  const int width = content_width * units / SCALE_UNITS;
  const int height = content_height * units / SCALE_UNITS;
  const int x = area.x1 + (container_width - width) / 2;
  const int y = area.y1 + (container_height - height) / 2;
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
  operation.scale_x = operation.scale_y = float(units) / SCALE_UNITS;
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
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  update_max(this->rx_present_ppa_max_us_, micros() - started);
#endif
  return success && still_current(&token);
}

void P4VideoRenderer::setup() {
  this->rx_done_ = xSemaphoreCreateBinaryStatic(&this->rx_done_storage_);
  this->presentation_mutex_ =
      xSemaphoreCreateMutexStatic(&this->presentation_mutex_storage_);
  if (this->rx_done_ == nullptr || this->presentation_mutex_ == nullptr) {
    ESP_LOGE(TAG, "P4 video receive synchronization setup failed");
    this->mark_failed();
    return;
  }
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  // Claim the small INTERNAL/DMA descriptors before reserving multi-megabyte
  // PSRAM surfaces. The camera encoder may own another handle; esp-idf
  // serializes both handles on the one physical JPEG engine.
  if (!this->init_jpeg_decoder_()) {
    ESP_LOGE(TAG, "P4 hardware JPEG decoder setup failed");
    this->mark_failed();
    return;
  }
#endif
#ifdef USE_P4_VIDEO_RENDERER_H264
  this->h264_au_queue_ = xQueueCreateStatic(
      kH264AccessUnitQueueDepth, sizeof(uint8_t), this->h264_au_queue_storage_,
      &this->h264_au_queue_control_);
  if (this->h264_au_queue_ == nullptr) {
    ESP_LOGE(TAG, "P4 H.264 access-unit queue setup failed");
    this->mark_failed();
    return;
  }
#endif
  // Reserve the large codec and presentation surfaces before calls can be
  // offered. This keeps PSRAM heap walks and multi-megabyte allocations out
  // of stream preparation and fails deterministically at boot if no
  // suitable contiguous extent remains.
  if (!this->allocate_session_resources_()) {
    ESP_LOGE(TAG, "Unable to reserve P4 video receive resources at setup");
    this->mark_failed();
    return;
  }
#ifdef USE_P4_VIDEO_RENDERER_H264
  // Prime the normal negotiated resolution at boot. A peer that legally sends
  // another admitted resolution reconfigures this one worker-owned handle
  // once, rather than allocating or rebuilding conversion state per frame.
  if (!this->configure_i420_converter_(this->width_, this->height_)) {
    ESP_LOGE(TAG, "Unable to initialize P4 I420 conversion at setup");
    this->mark_failed();
    return;
  }
  // Allocate tinyH264's working set at boot. Normal call setup only changes
  // generation/admission state; it performs no decoder heap churn.
  if (!this->reset_h264_decoder_()) {
    ESP_LOGE(TAG, "Unable to initialize P4 H.264 decoder at setup");
    this->mark_failed();
    return;
  }
#endif
  // The decoder worker is component-owned, not call-owned. It sleeps on a
  // direct notification while idle and lets the producer revoke a generation
  // without waiting for an in-flight hardware or software decode.
  this->rx_running_.store(true, std::memory_order_release);
  if (!this->start_rx_task_()) {
    this->rx_running_.store(false, std::memory_order_release);
    ESP_LOGE(TAG, "Unable to start persistent P4 video decoder worker");
    this->mark_failed();
    return;
  }
  this->disable_loop();
}

void P4VideoRenderer::on_shutdown() {
  this->rx_session_prepared_.store(false, std::memory_order_release);
  this->rx_active_.store(false, std::memory_order_release);
  this->rx_session_generation_.fetch_add(1, std::memory_order_acq_rel);
  this->stop_rx_task_();
}

void P4VideoRenderer::loop() {
  this->disable_loop();
  this->refresh_layout_();
  if (this->video_ended_pending_.exchange(false, std::memory_order_acq_rel)) {
    this->video_ended_trigger_.trigger();
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
    int expected = pending;
    this->pending_surface_.compare_exchange_strong(expected, -1,
                                                   std::memory_order_acq_rel);
  }
  xSemaphoreGive(this->presentation_mutex_);
  if (first_frame &&
      generation ==
          this->rx_session_generation_.load(std::memory_order_acquire) &&
      this->has_remote_frame())
    this->first_frame_trigger_.trigger();
}

void P4VideoRenderer::dump_config() {
#ifdef USE_P4_VIDEO_RENDERER_H264
  ESP_LOGCONFIG(TAG, "P4 software H.264 video renderer:");
#else
  ESP_LOGCONFIG(TAG, "P4 hardware JPEG video renderer:");
#endif
  ESP_LOGCONFIG(TAG, "  Preferred receive: %ux%u @ %u fps", this->width_,
                this->height_, this->framerate_);
#ifdef USE_P4_VIDEO_RENDERER_H264
  ESP_LOGCONFIG(TAG, "  Decode admission: <= %ux%u and <= %u macroblocks",
                this->max_decode_width_, this->max_decode_height_,
                static_cast<unsigned>(kH264Level30MaxMacroblocks));
  ESP_LOGCONFIG(
      TAG, "  Reserved YUV pixels: %u bytes, encoded-frame lease slots: %u",
      static_cast<unsigned>(this->optimized_yuv420_capacity_),
      static_cast<unsigned>(kH264AccessUnitQueueDepth));
  ESP_LOGCONFIG(
      TAG, "  PSRAM after setup: free=%u, largest=%u",
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
      static_cast<unsigned>(
          heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#else
  ESP_LOGCONFIG(TAG, "  Decode admission: <= %ux%u", this->max_decode_width_,
                this->max_decode_height_);
  size_t allocated_surfaces = 0;
  for (const auto *surface : this->surfaces_)
    allocated_surfaces += surface != nullptr;
  ESP_LOGCONFIG(TAG,
                "  Reserved RX buffers: surfaces=%u x %u bytes, AU=%u bytes",
                static_cast<unsigned>(allocated_surfaces),
                static_cast<unsigned>(this->surface_capacity_bytes_),
                static_cast<unsigned>(this->rx_au_capacity_));
#endif
  ESP_LOGCONFIG(
      TAG, "  Presentation: display-owned pixel transform, main-loop commit");
  ESP_LOGCONFIG(TAG, "  Task stack in PSRAM: %s", YESNO(kTaskStackInPsram));
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
  // LVGL's first page flush is complete. Pixels are committed by our main-loop
  // owner, not by this callback or by the decoder worker.
  this->presentation_in_flight_.store(false, std::memory_order_release);
  if (this->rx_running_.load(std::memory_order_acquire) &&
      this->rx_active_.load(std::memory_order_acquire) &&
      this->pending_surface_.load(std::memory_order_acquire) >= 0)
    this->enable_loop_soon_any_context();
}

video_renderer::RendererCapabilities P4VideoRenderer::get_capabilities() const {
  video_renderer::RendererCapabilities caps{};
#ifdef USE_P4_VIDEO_RENDERER_H264
  caps.preferred.codec = video_renderer::Codec::H264;
  caps.requires_lease = true;
#else
  caps.preferred.codec = video_renderer::Codec::JPEG;
#endif
  caps.preferred.width = this->width_;
  caps.preferred.height = this->height_;
  caps.preferred.max_fps = this->framerate_;
  caps.max_decode_width = this->max_decode_width_;
  caps.max_decode_height = this->max_decode_height_;
  caps.max_encoded_frame_bytes = kMaxAccessUnitBytes;
  return caps;
}

bool P4VideoRenderer::start() {
  return (this->rx_session_prepared_.load(std::memory_order_acquire) ||
          this->prepare(this->get_capabilities().preferred)) &&
         this->set_active(true);
}

#ifdef USE_P4_VIDEO_RENDERER_JPEG
bool P4VideoRenderer::init_jpeg_decoder_() {
  if (this->jpeg_decoder_ != nullptr)
    return true;
  jpeg_decode_engine_cfg_t engine_config{};
  // This timeout covers the hardware transaction itself. Waiting for a
  // concurrent camera encode is serialized separately by esp-idf's codec
  // mutex and therefore does not consume this budget.
  engine_config.timeout_ms = 100;
  const esp_err_t error =
      jpeg_new_decoder_engine(&engine_config, &this->jpeg_decoder_);
  if (error != ESP_OK || this->jpeg_decoder_ == nullptr) {
    ESP_LOGE(TAG, "JPEG decoder engine allocation failed: %s",
             esp_err_to_name(error));
    this->jpeg_decoder_ = nullptr;
    return false;
  }
  this->jpeg_decode_config_.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  // ESP-IDF names the native little-endian RGB565 layout "BGR" because it
  // describes byte/component order rather than the logical LVGL color.
  this->jpeg_decode_config_.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  this->jpeg_decode_config_.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
  return true;
}
#endif

#ifdef USE_P4_VIDEO_RENDERER_JPEG
uint16_t P4VideoRenderer::jpeg_storage_width_(
    const jpeg_decode_picture_info_t &picture) {
  const uint32_t alignment =
      picture.sample_method == JPEG_DOWN_SAMPLING_YUV444 ? 8U : 16U;
  return static_cast<uint16_t>((picture.width + alignment - 1U) &
                               ~(alignment - 1U));
}

uint16_t P4VideoRenderer::jpeg_storage_height_(
    const jpeg_decode_picture_info_t &picture) {
  const uint32_t alignment =
      picture.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16U : 8U;
  return static_cast<uint16_t>((picture.height + alignment - 1U) &
                               ~(alignment - 1U));
}

bool P4VideoRenderer::read_jpeg_picture_info_(
    const uint8_t *data, size_t size,
    const video_renderer::JpegFrameInfo &frame_info,
    jpeg_decode_picture_info_t *picture) const {
  if (data == nullptr || size == 0 || size > UINT32_MAX || picture == nullptr)
    return false;
  *picture = {};
  if (frame_info.is_set()) {
    // The depacketizer validated these fields against the interchange header
    // it built for this exact AU. No additional parse/allocation is needed.
    picture->width = frame_info.width;
    picture->height = frame_info.height;
    switch (frame_info.sampling) {
    case video_renderer::JpegSampling::YUV444:
      picture->sample_method = JPEG_DOWN_SAMPLING_YUV444;
      break;
    case video_renderer::JpegSampling::YUV422:
      picture->sample_method = JPEG_DOWN_SAMPLING_YUV422;
      break;
    case video_renderer::JpegSampling::YUV420:
      picture->sample_method = JPEG_DOWN_SAMPLING_YUV420;
      break;
    default:
      return false;
    }
  } else if (jpeg_decoder_get_info(data, static_cast<uint32_t>(size),
                                   picture) != ESP_OK) {
    // Generic producers may omit metadata; their actual header is
    // authoritative.
    return false;
  }
  if (picture->sample_method == JPEG_DOWN_SAMPLING_GRAY ||
      picture->width == 0 || picture->height == 0 ||
      picture->width > this->max_decode_width_ ||
      picture->height > this->max_decode_height_)
    return false;
  const uint16_t storage_width = jpeg_storage_width_(*picture);
  const uint16_t storage_height = jpeg_storage_height_(*picture);
  const size_t required =
      static_cast<size_t>(storage_width) * storage_height * 2U;
  if (storage_width == 0 || storage_height == 0 ||
      this->surfaces_[0] == nullptr ||
      required > this->surface_capacity_bytes_) {
    return false;
  }
  return true;
}
#endif

bool P4VideoRenderer::allocate_session_resources_() {
  const size_t encoded_buffer_bytes = kMaxAccessUnitBytes;
#ifdef USE_P4_VIDEO_RENDERER_H264
  // The producer owns bounded encoded-frame buffers and transfers leases to the
  // decoder worker. The renderer queues only slot metadata and never copies a
  // complete H.264 access unit.
  (void)encoded_buffer_bytes;
#else
  if (this->rx_au_ == nullptr) {
    this->rx_au_ = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        16, encoded_buffer_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    this->rx_au_capacity_ = this->rx_au_ == nullptr ? 0 : encoded_buffer_bytes;
  }
  if (this->rx_au_ == nullptr || this->rx_au_capacity_ < encoded_buffer_bytes) {
    this->free_codec_resources_();
    this->free_unpublished_surfaces_();
    return false;
  }
#endif

  // Keep one source-pixel buffer. PPA writes directly into the display-owned
  // framebuffer; there is no LVGL image or scaled RGB intermediate.
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  const size_t bytes = size_t((this->max_decode_width_ + 15U) & ~15U) *
                       ((this->max_decode_height_ + 15U) & ~15U) *
                       sizeof(uint16_t);
  this->surface_capacity_bytes_ = (bytes + 63U) & ~size_t(63U);
  if (this->surfaces_[0] == nullptr)
    this->surfaces_[0] =
        static_cast<uint8_t *>(alloc_surface(this->surface_capacity_bytes_, 0));
  if (this->surfaces_[0] == nullptr) {
    this->free_codec_resources_();
    this->free_unpublished_surfaces_();
    return false;
  }
#else
  const size_t bytes = this->h264_optimized_yuv_bytes_();
  if (this->optimized_yuv420_ == nullptr) {
    this->optimized_yuv420_ = static_cast<uint8_t *>(alloc_psram_dma(bytes));
    this->optimized_yuv420_capacity_ =
        this->optimized_yuv420_ == nullptr ? 0 : bytes;
  }
  if (this->optimized_yuv420_ == nullptr ||
      this->optimized_yuv420_capacity_ < bytes) {
    this->free_codec_resources_();
    this->free_unpublished_surfaces_();
    return false;
  }
  this->surfaces_[0] =
      this->optimized_yuv420_; // codec-owned storage, one pending lease
  this->surface_capacity_bytes_ = size_t(this->max_decode_width_) *
                                  this->max_decode_height_ * sizeof(uint16_t);
#endif
  this->surface_data_size_[0] = 0;
  this->surface_stride_bytes_[0] = 0;
  this->surface_content_width_[0] = this->surface_content_height_[0] = 0;

  return true;
}

#ifdef USE_P4_VIDEO_RENDERER_H264
bool P4VideoRenderer::reset_h264_decoder_() {
  if (this->h264_decoder_ != nullptr) {
    esp_h264_dec_close(this->h264_decoder_);
    esp_h264_dec_del(this->h264_decoder_);
    this->h264_decoder_ = nullptr;
  }
  esp_h264_dec_cfg_sw_t config{};
  config.pic_type = ESP_H264_RAW_FMT_I420;
  return esp_h264_dec_sw_new(&config, &this->h264_decoder_) ==
             ESP_H264_ERR_OK &&
         this->h264_decoder_ != nullptr &&
         esp_h264_dec_open(this->h264_decoder_) == ESP_H264_ERR_OK;
}

bool P4VideoRenderer::h264_resolution_fits_(uint16_t width,
                                            uint16_t height) const {
  const bool configured_orientation =
      width <= this->max_decode_width_ && height <= this->max_decode_height_;
  const bool rotated_orientation =
      width <= this->max_decode_height_ && height <= this->max_decode_width_;
  if (width == 0 || height == 0 || (width & 1U) != 0 || (height & 1U) != 0 ||
      (!configured_orientation && !rotated_orientation)) {
    return false;
  }
  const size_t macroblocks_w = (static_cast<size_t>(width) + 15) / 16;
  const size_t macroblocks_h = (static_cast<size_t>(height) + 15) / 16;
  return macroblocks_w * macroblocks_h <= kH264Level30MaxMacroblocks;
}
#endif

bool P4VideoRenderer::prepare(const video_renderer::StreamFormat &capability) {
  const bool component_failed = this->is_failed();
  const bool capability_valid = capability.valid();
#ifdef USE_P4_VIDEO_RENDERER_H264
  const bool codec_valid = capability.codec == video_renderer::Codec::H264;
  const bool level_valid =
      true; // Transport profile checks stay with the producer.
  const bool codec_ready = this->h264_decoder_ != nullptr;
#else
  const bool codec_valid = capability.codec == video_renderer::Codec::JPEG;
  const bool level_valid = true;
  const bool codec_ready = true;
#endif
  const bool worker_running = this->rx_running_.load(std::memory_order_acquire);
  const bool already_prepared =
      this->rx_session_prepared_.load(std::memory_order_acquire);
  if (component_failed || !capability_valid || !codec_valid || !level_valid ||
      !codec_ready || !worker_running || already_prepared) {
    ESP_LOGE(TAG,
             "Video prepare rejected: failed=%s capability=%s codec=%s "
             "level=%s ready=%s worker=%s prepared=%s",
             YESNO(component_failed), YESNO(capability_valid),
             YESNO(codec_valid), YESNO(level_valid), YESNO(codec_ready),
             YESNO(worker_running), YESNO(already_prepared));
    return false;
  }
  if (this->rx_session_prepared_.exchange(true, std::memory_order_acq_rel)) {
    ESP_LOGE(TAG, "Video prepare lost ownership race");
    return false;
  }
  this->rx_format_ = capability;
  // Session generations make an AU already owned by the persistent worker
  // harmless across a fast hangup/redial.
  this->rx_session_generation_.fetch_add(1, std::memory_order_acq_rel);
#ifdef USE_P4_VIDEO_RENDERER_H264
  // A fresh IDR carries the session-owned parameter sets and resets reference
  // picture state. Keep the decoder allocation itself component-owned.
  this->loss_generation_.fetch_add(1, std::memory_order_acq_rel);
  this->waiting_for_key_frame_.store(true, std::memory_order_release);
#endif
  if (this->rx_task_handle_ != nullptr)
    xTaskNotifyGive(this->rx_task_handle_);
  this->rx_admitted_frames_.store(0, std::memory_order_release);
  this->rx_rendered_frames_.store(0, std::memory_order_release);
  this->rx_presented_frames_.store(0, std::memory_order_release);
  this->rx_refresh_completed_.store(0, std::memory_order_release);
#ifdef USE_P4_VIDEO_RENDERER_H264
  this->h264_first_au_logged_ = false;
#endif
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  this->rx_refresh_max_ms_.store(0, std::memory_order_release);
  this->rx_refresh_total_ms_.store(0, std::memory_order_release);
  this->debug_render_started_us_.store(0, std::memory_order_relaxed);
  this->debug_flush_started_us_.store(0, std::memory_order_relaxed);
  this->debug_render_count_.store(0, std::memory_order_relaxed);
  this->debug_render_total_us_.store(0, std::memory_order_relaxed);
  this->debug_render_max_us_.store(0, std::memory_order_relaxed);
  this->debug_flush_count_.store(0, std::memory_order_relaxed);
  this->debug_flush_total_us_.store(0, std::memory_order_relaxed);
  this->debug_flush_max_us_.store(0, std::memory_order_relaxed);
  this->debug_flush_width_.store(0, std::memory_order_relaxed);
  this->debug_flush_height_.store(0, std::memory_order_relaxed);
  this->rx_present_ppa_max_us_.store(0, std::memory_order_release);
  this->presentation_started_ms_.store(0, std::memory_order_release);
  this->rx_rate_drops_.store(0, std::memory_order_release);
  this->rx_busy_drops_.store(0, std::memory_order_release);
  this->rx_decode_drops_.store(0, std::memory_order_release);
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  this->rx_jpeg_decode_max_us_.store(0, std::memory_order_release);
  this->rx_au_work_max_us_.store(0, std::memory_order_release);
  this->rx_busy_pending_drops_.store(0, std::memory_order_release);
  this->rx_busy_au_drops_.store(0, std::memory_order_release);
  this->pending_published_us_.store(0, std::memory_order_relaxed);
  this->pending_wait_count_.store(0, std::memory_order_release);
  this->pending_wait_max_us_.store(0, std::memory_order_release);
  this->pending_wait_total_us_.store(0, std::memory_order_release);
#endif
#ifdef USE_P4_VIDEO_RENDERER_H264
  this->h264_decode_failure_logged_ = false;
  this->rx_h264_decode_max_us_.store(0, std::memory_order_release);
  this->rx_i420_convert_max_us_.store(0, std::memory_order_release);
  this->rx_au_work_max_us_.store(0, std::memory_order_release);
  this->rx_queue_high_watermark_.store(0, std::memory_order_release);
  this->rx_queue_wait_max_us_.store(0, std::memory_order_release);
  this->rx_dependency_drops_.store(0, std::memory_order_release);
  this->rx_geometry_drops_.store(0, std::memory_order_release);
  this->rx_au_copy_max_us_.store(0, std::memory_order_release);
  this->rx_au_copy_total_us_.store(0, std::memory_order_release);
  this->rx_au_copy_total_bytes_.store(0, std::memory_order_release);
#endif
#endif
  this->rx_active_.store(false, std::memory_order_release);
  this->remote_frame_visible_.store(false, std::memory_order_release);
#ifdef USE_P4_VIDEO_RENDERER_H264
  ESP_LOGI(TAG, "P4 software H.264 receive path prepared");
#else
  ESP_LOGI(TAG, "P4 hardware JPEG receive path prepared");
#endif
  return true;
}

bool P4VideoRenderer::set_active(bool active) {
  if (active && (!this->rx_running_.load(std::memory_order_acquire) ||
                 !this->rx_session_prepared_.load(std::memory_order_acquire)))
    return false;
  this->rx_active_.store(active, std::memory_order_release);
  if (active) {
#ifdef USE_P4_VIDEO_RENDERER_H264
    this->cadence_.reset(std::max<uint8_t>(
        1, std::min(this->framerate_, this->rx_format_.max_fps == 0
                                          ? this->framerate_
                                          : this->rx_format_.max_fps)));
#endif
#ifdef USE_P4_VIDEO_RENDERER_H264
    this->waiting_for_key_frame_.store(true, std::memory_order_release);
#endif
    return true;
  }

  // Invalidate the generation before touching presentation state. A decoder
  // AU may already be owned, but every publish site rechecks rx_active_. Drop
  // a decoded surface still waiting for presentation. A surface handed to LVGL
  // remains immutable until the serialized presentation owner releases it.
  this->rx_active_.store(false, std::memory_order_release);
  this->rx_session_generation_.fetch_add(1, std::memory_order_acq_rel);
#ifdef USE_P4_VIDEO_RENDERER_H264
  this->loss_generation_.fetch_add(1, std::memory_order_acq_rel);
  this->waiting_for_key_frame_.store(true, std::memory_order_release);
#endif
  if (this->rx_task_handle_ != nullptr)
    xTaskNotifyGive(this->rx_task_handle_);
  // UI ownership ends immediately, even while the last surface is in flight.
  const bool was_visible =
      this->remote_frame_visible_.exchange(false, std::memory_order_acq_rel);
  if (xSemaphoreTake(this->presentation_mutex_, 0) != pdTRUE) {
    this->video_ended_pending_.store(true, std::memory_order_release);
    this->enable_loop_soon_any_context();
    return true;
  }
  this->pending_surface_.store(-1, std::memory_order_release);
  this->presentation_in_flight_.store(false, std::memory_order_release);
  xSemaphoreGive(this->presentation_mutex_);
  if (was_visible) {
    this->video_ended_pending_.store(true, std::memory_order_release);
    this->enable_loop_soon_any_context();
  }
  return true;
}

bool P4VideoRenderer::submit_frame(
    const video_renderer::EncodedFrame &access_unit) {
  if (!this->rx_running_.load(std::memory_order_acquire) ||
      !this->rx_active_.load(std::memory_order_acquire) ||
      access_unit.data == nullptr || access_unit.size == 0) {
    return false;
  }
#ifdef USE_P4_VIDEO_RENDERER_H264
  // H.264 transfers a preallocated producer slot to the persistent decoder
  // worker, so every accepted AU must carry the callback that returns that
  // lease. JPEG is copied synchronously into the renderer-owned slot below and
  // does not transfer ownership.
  if (access_unit.release_callback == nullptr)
    return false;
  if (!this->rx_session_prepared_.load(std::memory_order_acquire) ||
      this->h264_au_queue_ == nullptr ||
      access_unit.size > kMaxAccessUnitBytes) {
    this->loss_generation_.fetch_add(1, std::memory_order_acq_rel);
    this->waiting_for_key_frame_.store(true, std::memory_order_release);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
    this->rx_busy_drops_.fetch_add(1, std::memory_order_relaxed);
#endif
    return false;
  }
  if (this->waiting_for_key_frame_.load(std::memory_order_acquire) &&
      !access_unit.key_frame) {
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
    this->rx_dependency_drops_.fetch_add(1, std::memory_order_relaxed);
#endif
    return false;
  }

  uint8_t slot_index = UINT8_MAX;
  for (uint8_t index = 0; index < kH264AccessUnitQueueDepth; index++) {
    uint8_t expected = 0;
    if (this->h264_au_slots_[index].state.compare_exchange_strong(
            expected, 2, std::memory_order_acq_rel)) {
      slot_index = index;
      break;
    }
  }
  if (slot_index == UINT8_MAX) {
    // A full bounded queue means the decoder has fallen behind. Dependent
    // frames already queued under the previous generation are discarded and
    // The producer can request a fresh IDR; memory use remains bounded.
    this->loss_generation_.fetch_add(1, std::memory_order_acq_rel);
    this->waiting_for_key_frame_.store(true, std::memory_order_release);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
    this->rx_busy_drops_.fetch_add(1, std::memory_order_relaxed);
#endif
    return false;
  }

  auto &slot = this->h264_au_slots_[slot_index];
  slot.access_unit = access_unit;
  slot.key_frame = access_unit.key_frame;
  slot.session_generation =
      this->rx_session_generation_.load(std::memory_order_acquire);
  slot.loss_generation = this->loss_generation_.load(std::memory_order_acquire);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  slot.queued_at_us = micros();
#endif
  slot.state.store(1, std::memory_order_release);
  if (xQueueSend(this->h264_au_queue_, &slot_index, 0) != pdTRUE) {
    // Enqueue failed: ownership is still with the submitter.
    slot.access_unit = {};
    slot.state.store(0, std::memory_order_release);
    this->loss_generation_.fetch_add(1, std::memory_order_acq_rel);
    this->waiting_for_key_frame_.store(true, std::memory_order_release);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
    this->rx_busy_drops_.fetch_add(1, std::memory_order_relaxed);
#endif
    return false;
  }
  if (access_unit.key_frame)
    this->waiting_for_key_frame_.store(false, std::memory_order_release);
  this->rx_admitted_frames_.fetch_add(1, std::memory_order_relaxed);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  update_max(this->rx_queue_high_watermark_,
             uxQueueMessagesWaiting(this->h264_au_queue_));
#endif
  if (this->rx_task_handle_ != nullptr)
    xTaskNotifyGive(this->rx_task_handle_);
  return true;
#else
  if (access_unit.size > this->rx_au_capacity_)
    return false;
  // JPEG frames are independent and can be discarded while the previous
  // source pixels await their display-owned commit.
  if (this->pending_surface_.load(std::memory_order_acquire) >= 0) {
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
    this->rx_busy_drops_.fetch_add(1, std::memory_order_relaxed);
    this->rx_busy_pending_drops_.fetch_add(1, std::memory_order_relaxed);
#endif
    return false;
  }
  uint8_t expected = 0;
  if (!this->rx_slot_state_.compare_exchange_strong(
          expected, 2, std::memory_order_acq_rel)) {
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
    this->rx_busy_drops_.fetch_add(1, std::memory_order_relaxed);
    this->rx_busy_au_drops_.fetch_add(1, std::memory_order_relaxed);
#endif
    return false;
  }
  memcpy(this->rx_au_, access_unit.data, access_unit.size);
  this->rx_au_size_ = access_unit.size;
  this->rx_timestamp_us_ = access_unit.presentation_time.valid
                               ? access_unit.presentation_time.microseconds
                               : 0;
  this->rx_au_generation_ =
      this->rx_session_generation_.load(std::memory_order_acquire);
  this->rx_key_frame_ = access_unit.key_frame;
  this->rx_au_jpeg_info_ = access_unit.jpeg_info;
  this->rx_admitted_frames_.fetch_add(1, std::memory_order_relaxed);
  this->rx_slot_state_.store(1, std::memory_order_release);
  if (this->rx_task_handle_ != nullptr)
    xTaskNotifyGive(this->rx_task_handle_);
  access_unit.release();
  return true;
#endif
}

bool P4VideoRenderer::start_rx_task_() {
  if (this->rx_task_handle_ != nullptr)
    return false;
  xSemaphoreTake(this->rx_done_, 0);
  this->rx_done_observed_ = false;
  // Codec work stays below real-time audio priorities and blocks indefinitely
  // on a direct notification. H.264 drains only its fixed-depth AU queue.
  return video_renderer::start_managed_pinned_task(
      P4VideoRenderer::rx_task_trampoline_, "p4_video_rx", kTaskStackBytes,
      this, kTaskPriority, 1, kTaskStackInPsram, TAG, &this->rx_task_handle_,
#ifdef USE_ESP_IDF
      nullptr,
#else
      &this->rx_task_tcb_,
#endif
      &this->rx_task_stack_, &this->rx_task_with_caps_);
}

void P4VideoRenderer::rx_task_trampoline_(void *ctx) {
  static_cast<P4VideoRenderer *>(ctx)->rx_task_();
}

#ifdef USE_P4_VIDEO_RENDERER_H264
bool P4VideoRenderer::decode_h264_access_unit_(
    const uint8_t *data, size_t size, uint64_t timestamp_us,
    bool timestamp_valid, bool key_frame, uint32_t session_generation,
    uint32_t loss_generation, bool &decoded) {
  decoded = false;
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  const uint32_t au_started_us = micros();
#endif
  if (key_frame &&
      this->decoder_reset_pending_.exchange(false, std::memory_order_acq_rel) &&
      !this->reset_h264_decoder_()) {
    ESP_LOGE(TAG, "Failed to reset H.264 decoder for recovery IDR");
    this->rx_running_.store(false, std::memory_order_release);
    return false;
  }
  if (!this->h264_first_au_logged_) {
    this->h264_first_au_logged_ = true;
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
    ESP_LOGI(TAG,
             "H.264 first AU: bytes=%u key=%s; PSRAM free=%u largest=%u; "
             "internal free=%u largest=%u",
             static_cast<unsigned>(size), YESNO(key_frame),
             static_cast<unsigned>(
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(
                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                           MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(
                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
#else
    ESP_LOGI(TAG, "H.264 first AU: bytes=%u key=%s",
             static_cast<unsigned>(size), YESNO(key_frame));
#endif
  }

  bool rendered = false;
  bool decode_failed = false;
  // Espressif's decoder consumes an Annex-B access unit incrementally and
  // reports the byte count consumed for each NAL. Keep the start codes in
  // place and follow the component's official single-decoder example.
  esp_h264_dec_in_frame_t input{};
  input.raw_data.buffer = const_cast<uint8_t *>(data);
  input.raw_data.len = size;
  input.pts = static_cast<decltype(input.pts)>(timestamp_us);
  while (input.raw_data.len > 0 &&
         this->rx_running_.load(std::memory_order_acquire) &&
         this->rx_active_.load(std::memory_order_acquire) &&
         session_generation ==
             this->rx_session_generation_.load(std::memory_order_acquire)) {
    esp_h264_dec_out_frame_t output{};
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
    const uint32_t decode_started_us = micros();
#endif
    const esp_h264_err_t error =
        esp_h264_dec_process(this->h264_decoder_, &input, &output);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
    update_max(this->rx_h264_decode_max_us_, micros() - decode_started_us);
#endif
    if (error != ESP_H264_ERR_OK || input.consume == 0 ||
        input.consume > input.raw_data.len) {
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
      if (!this->h264_decode_failure_logged_) {
        this->h264_decode_failure_logged_ = true;
        ESP_LOGW(TAG,
                 "H.264 decode failure: error=%d consume=%u remaining=%u; "
                 "PSRAM free=%u largest=%u; internal free=%u largest=%u",
                 static_cast<int>(error), static_cast<unsigned>(input.consume),
                 static_cast<unsigned>(input.raw_data.len),
                 static_cast<unsigned>(heap_caps_get_free_size(
                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(
                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_free_size(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
      }
#endif
      decode_failed = true;
      break;
    }
    input.raw_data.buffer += input.consume;
    input.raw_data.len -= input.consume;
    if (output.outbuf == nullptr || output.out_size == 0)
      continue;
    decoded = true;
    if (session_generation !=
            this->rx_session_generation_.load(std::memory_order_acquire) ||
        !this->rx_active_.load(std::memory_order_acquire)) {
      break;
    }
    // Decode every H.264 picture so dependent P-frames retain a valid GOP,
    // but spend conversion, PPA and LVGL bandwidth only at the negotiated
    // presentation cadence. Peers sometimes exceed max-fps; dropping before
    // decode would corrupt subsequent references and force repeated IDRs.
    if (timestamp_valid && !this->cadence_.accept(output.pts)) {
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
      this->rx_rate_drops_.fetch_add(1, std::memory_order_relaxed);
#endif
      continue;
    }
    esp_h264_dec_param_sw_handle_t parameters = nullptr;
    esp_h264_resolution_t resolution{};
    if (esp_h264_dec_sw_get_param_hd(this->h264_decoder_, &parameters) ==
            ESP_H264_ERR_OK &&
        parameters != nullptr &&
        esp_h264_dec_get_resolution(parameters, &resolution) ==
            ESP_H264_ERR_OK) {
      rendered = this->render_i420_(output.outbuf, output.out_size,
                                    static_cast<uint16_t>(resolution.width),
                                    static_cast<uint16_t>(resolution.height),
                                    session_generation) ||
                 rendered;
    }
  }
  if (decode_failed) {
    this->loss_generation_.fetch_add(1, std::memory_order_acq_rel);
    this->waiting_for_key_frame_.store(true, std::memory_order_release);
    this->decoder_reset_pending_.store(true, std::memory_order_release);
  } else if (session_generation ==
                 this->rx_session_generation_.load(std::memory_order_acquire) &&
             loss_generation ==
                 this->loss_generation_.load(std::memory_order_acquire)) {
    // Keep admission open only if neither teardown nor a later slot loss
    // invalidated this GOP while the AU was being decoded.
    this->waiting_for_key_frame_.store(false, std::memory_order_release);
  }
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  update_max(this->rx_au_work_max_us_, micros() - au_started_us);
#endif
  return rendered;
}
#endif

void P4VideoRenderer::rx_task_() {
  while (this->rx_running_.load(std::memory_order_acquire)) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!this->rx_running_.load(std::memory_order_acquire))
      break;
#ifdef USE_P4_VIDEO_RENDERER_H264
    uint8_t slot_index = UINT8_MAX;
    while (xQueueReceive(this->h264_au_queue_, &slot_index, 0) == pdTRUE) {
      if (slot_index >= kH264AccessUnitQueueDepth)
        continue;
      auto &slot = this->h264_au_slots_[slot_index];
      uint8_t expected = 1;
      if (!slot.state.compare_exchange_strong(expected, 2,
                                              std::memory_order_acq_rel)) {
        continue;
      }
      const bool current =
          this->rx_active_.load(std::memory_order_acquire) &&
          this->rx_session_prepared_.load(std::memory_order_acquire) &&
          slot.session_generation ==
              this->rx_session_generation_.load(std::memory_order_acquire) &&
          slot.loss_generation ==
              this->loss_generation_.load(std::memory_order_acquire);
      bool rendered = false;
      bool decoded = false;
      if (current) {
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
        update_max(this->rx_queue_wait_max_us_, micros() - slot.queued_at_us);
#endif
        rendered = this->decode_h264_access_unit_(
            slot.access_unit.data, slot.access_unit.size,
            slot.access_unit.presentation_time.microseconds,
            slot.access_unit.presentation_time.valid, slot.key_frame,
            slot.session_generation, slot.loss_generation, decoded);
      }
      if (current) {
        if (rendered)
          this->rx_rendered_frames_.fetch_add(1, std::memory_order_relaxed);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
        else if (!decoded)
          this->rx_decode_drops_.fetch_add(1, std::memory_order_relaxed);
#endif
      }
      slot.access_unit.release();
      slot.access_unit = {};
      slot.state.store(0, std::memory_order_release);
    }
#else
    uint8_t expected = 1;
    if (!this->rx_slot_state_.compare_exchange_strong(
            expected, 2, std::memory_order_acq_rel)) {
      continue;
    }
    const uint32_t session_generation = this->rx_au_generation_;
    const bool current =
        this->rx_active_.load(std::memory_order_acquire) &&
        this->rx_session_prepared_.load(std::memory_order_acquire) &&
        session_generation ==
            this->rx_session_generation_.load(std::memory_order_acquire);
    if (!current) {
      this->rx_slot_state_.store(0, std::memory_order_release);
      continue;
    }
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
    const uint32_t au_started_us = micros();
#endif
    bool rendered = false;
    if (this->pending_surface_.load(std::memory_order_acquire) < 0 &&
        this->jpeg_decoder_ != nullptr) {
      jpeg_decode_picture_info_t picture{};
      if (this->read_jpeg_picture_info_(this->rx_au_, this->rx_au_size_,
                                        this->rx_au_jpeg_info_, &picture)) {
        const uint16_t storage_width = jpeg_storage_width_(picture);
        const uint16_t storage_height = jpeg_storage_height_(picture);
        const int output_index = 0;
        const size_t expected_size =
            static_cast<size_t>(storage_width) * storage_height * 2U;
        uint32_t decoded_size = 0;
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
        const uint32_t decode_started_us = micros();
#endif
        esp_err_t decode_error = ESP_ERR_INVALID_SIZE;
        if (output_index == 0 && this->surfaces_[output_index] != nullptr &&
            expected_size <= this->surface_capacity_bytes_ &&
            expected_size <= UINT32_MAX) {
          // ESP-IDF synchronizes exactly outbuf_size bytes before and after the
          // DMA transaction. Pass the padded size of this JPEG, not the maximum
          // 800x800 surface capacity, so a small remote frame does not evict an
          // unrelated megabyte of PSRAM cache while AFE is producing audio.
          decode_error = jpeg_decoder_process(
              this->jpeg_decoder_, &this->jpeg_decode_config_, this->rx_au_,
              static_cast<uint32_t>(this->rx_au_size_),
              this->surfaces_[output_index],
              static_cast<uint32_t>(expected_size), &decoded_size);
        }
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
        update_max(this->rx_jpeg_decode_max_us_, micros() - decode_started_us);
#endif
        if (decode_error == ESP_OK && decoded_size == expected_size &&
            this->rx_active_.load(std::memory_order_acquire) &&
            this->rx_session_prepared_.load(std::memory_order_acquire) &&
            session_generation ==
                this->rx_session_generation_.load(std::memory_order_acquire)) {
          this->surface_data_size_[output_index] = decoded_size;
          this->surface_stride_bytes_[output_index] =
              storage_width * sizeof(uint16_t);
          this->surface_content_width_[output_index] =
              static_cast<uint16_t>(picture.width);
          this->surface_content_height_[output_index] =
              static_cast<uint16_t>(picture.height);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
          this->pending_published_us_.store(micros(),
                                            std::memory_order_relaxed);
#endif
          this->pending_surface_generation_.store(session_generation,
                                                  std::memory_order_relaxed);
          this->pending_surface_.store(output_index, std::memory_order_release);
          this->enable_loop_soon_any_context();
          rendered = true;
        }
      }
    }
    if (rendered)
      this->rx_rendered_frames_.fetch_add(1, std::memory_order_relaxed);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
    else
      this->rx_decode_drops_.fetch_add(1, std::memory_order_relaxed);
    update_max(this->rx_au_work_max_us_, micros() - au_started_us);
#endif
    this->rx_slot_state_.store(0, std::memory_order_release);
#endif
  }
#ifdef USE_P4_VIDEO_RENDERER_H264
  for (auto &slot : this->h264_au_slots_) {
    slot.access_unit.release();
    slot.access_unit = {};
    slot.state.store(0, std::memory_order_release);
  }
#else
  this->rx_slot_state_.store(0, std::memory_order_release);
#endif
  video_renderer::finish_managed_pinned_task(this->rx_done_);
}

#ifdef USE_P4_VIDEO_RENDERER_H264
bool P4VideoRenderer::configure_i420_converter_(uint16_t width,
                                                uint16_t height) {
  if (this->i420_converter_ != nullptr &&
      this->i420_converter_width_ == width &&
      this->i420_converter_height_ == height) {
    return true;
  }
  if (this->i420_converter_ != nullptr) {
    esp_imgfx_color_convert_close(this->i420_converter_);
    this->i420_converter_ = nullptr;
  }
  this->i420_converter_width_ = 0;
  this->i420_converter_height_ = 0;

  esp_imgfx_color_convert_cfg_t config{};
  config.in_res.width = static_cast<int16_t>(width);
  config.in_res.height = static_cast<int16_t>(height);
  config.in_pixel_fmt = ESP_IMGFX_PIXEL_FMT_I420;
  config.out_pixel_fmt = ESP_IMGFX_PIXEL_FMT_O_UYY_E_VYY;
  config.color_space_std = ESP_IMGFX_COLOR_SPACE_STD_BT601;
  const esp_imgfx_err_t error =
      esp_imgfx_color_convert_open(&config, &this->i420_converter_);
  if (error != ESP_IMGFX_ERR_OK || this->i420_converter_ == nullptr) {
    ESP_LOGE(TAG, "Espressif I420 converter setup failed: %d (%ux%u)",
             static_cast<int>(error), width, height);
    this->i420_converter_ = nullptr;
    return false;
  }
  this->i420_converter_width_ = width;
  this->i420_converter_height_ = height;
  this->i420_converter_failure_logged_ = false;
  return true;
}

bool P4VideoRenderer::render_i420_(const uint8_t *i420, size_t size,
                                   uint16_t width, uint16_t height,
                                   uint32_t session_generation) {
  const size_t decoded_bytes = static_cast<size_t>(width) * height * 3 / 2;
  if (i420 == nullptr || !this->h264_resolution_fits_(width, height) ||
      size < decoded_bytes || this->optimized_yuv420_ == nullptr ||
      this->optimized_yuv420_capacity_ < decoded_bytes ||
      this->surfaces_[0] == nullptr ||
      this->pending_surface_.load(std::memory_order_acquire) >= 0) {
    return false;
  }
  if (!this->configure_i420_converter_(width, height))
    return false;
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  const uint32_t convert_started_us = micros();
#endif
  esp_imgfx_data_t input{
      .data = const_cast<uint8_t *>(i420),
      .data_len = static_cast<uint32_t>(decoded_bytes),
  };
  esp_imgfx_data_t output{
      .data = this->optimized_yuv420_,
      .data_len = static_cast<uint32_t>(decoded_bytes),
  };
  const esp_imgfx_err_t convert_error =
      esp_imgfx_color_convert_process(this->i420_converter_, &input, &output);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  update_max(this->rx_i420_convert_max_us_, micros() - convert_started_us);
#endif
  if (convert_error != ESP_IMGFX_ERR_OK) {
    if (!this->i420_converter_failure_logged_) {
      ESP_LOGE(TAG, "Espressif I420 conversion failed: %d (%ux%u)",
               static_cast<int>(convert_error), width, height);
      this->i420_converter_failure_logged_ = true;
    }
    return false;
  }
  if (xSemaphoreTake(this->presentation_mutex_, 0) != pdTRUE)
    return false;
  if (!this->rx_active_.load(std::memory_order_acquire) ||
      session_generation !=
          this->rx_session_generation_.load(std::memory_order_acquire)) {
    xSemaphoreGive(this->presentation_mutex_);
    return false;
  }
  this->surface_data_size_[0] = decoded_bytes;
  this->surface_stride_bytes_[0] = width;
  this->surface_content_width_[0] = width;
  this->surface_content_height_[0] = height;
  this->surface_layout_size_[0] =
      this->layout_size_.load(std::memory_order_acquire);
  this->surface_generation_[0] = session_generation;
  this->pending_surface_.store(0, std::memory_order_release);
  this->enable_loop_soon_any_context();
  xSemaphoreGive(this->presentation_mutex_);
  return true;
}
#endif

bool P4VideoRenderer::stop_rx_task_() {
  this->rx_running_.store(false, std::memory_order_release);
  if (this->rx_task_handle_ != nullptr)
    xTaskNotifyGive(this->rx_task_handle_);
  bool stopped = this->reap_rx_task_();
  if (!stopped && !this->rx_done_observed_ && this->rx_done_ != nullptr &&
      xSemaphoreTake(this->rx_done_, kTaskStopTimeoutTicks) == pdTRUE) {
    this->rx_done_observed_ = true;
    stopped = this->reap_rx_task_();
  }
  if (!stopped)
    ESP_LOGE(TAG, "Video RX task did not stop; retaining owned resources");
#ifndef USE_P4_VIDEO_RENDERER_H264
  if (stopped)
    this->rx_slot_state_.store(0, std::memory_order_release);
#endif
  return stopped;
}

bool P4VideoRenderer::reap_rx_task_() {
  if (this->rx_task_handle_ == nullptr) {
    this->rx_done_observed_ = false;
    return true;
  }
  if (!this->rx_done_observed_) {
    if (this->rx_done_ == nullptr ||
        xSemaphoreTake(this->rx_done_, 0) != pdTRUE) {
      return false;
    }
    this->rx_done_observed_ = true;
  }
  video_renderer::cleanup_managed_pinned_task(
      &this->rx_task_handle_, &this->rx_task_stack_, kTaskStackBytes,
      this->rx_task_with_caps_);
  if (this->rx_task_handle_ == nullptr) {
    this->rx_done_observed_ = false;
    return true;
  }
  return false;
}

void P4VideoRenderer::free_codec_resources_() {
#ifdef USE_P4_VIDEO_RENDERER_H264
  for (auto &slot : this->h264_au_slots_) {
    slot.access_unit.release();
    slot.access_unit = {};
    slot.state.store(0, std::memory_order_release);
  }
#else
  if (this->rx_au_ != nullptr)
    free(this->rx_au_);
#endif
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  if (this->jpeg_decoder_ != nullptr)
    jpeg_del_decoder_engine(this->jpeg_decoder_);
#endif
#ifdef USE_P4_VIDEO_RENDERER_H264
  if (this->h264_decoder_ != nullptr) {
    esp_h264_dec_close(this->h264_decoder_);
    esp_h264_dec_del(this->h264_decoder_);
    this->h264_decoder_ = nullptr;
  }
  if (this->i420_converter_ != nullptr)
    esp_imgfx_color_convert_close(this->i420_converter_);
  if (this->optimized_yuv420_ != nullptr)
    heap_caps_free(this->optimized_yuv420_);
#endif
#ifndef USE_P4_VIDEO_RENDERER_H264
  this->rx_au_ = nullptr;
  this->rx_au_capacity_ = 0;
#endif
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  this->jpeg_decoder_ = nullptr;
  this->rx_au_jpeg_info_ = {};
#endif
#ifdef USE_P4_VIDEO_RENDERER_H264
  this->i420_converter_ = nullptr;
  this->i420_converter_width_ = 0;
  this->i420_converter_height_ = 0;
  this->i420_converter_failure_logged_ = false;
  this->optimized_yuv420_ = nullptr;
  this->optimized_yuv420_capacity_ = 0;
#endif
}

void P4VideoRenderer::free_unpublished_surfaces_() {
  if (this->surface_ever_presented_.load(std::memory_order_acquire))
    return;
  for (size_t index = 0; index < this->surface_count_; index++) {
#ifdef USE_P4_VIDEO_RENDERER_JPEG
    if (this->surfaces_[index] != nullptr)
      heap_caps_free(this->surfaces_[index]);
#endif
    this->surfaces_[index] = nullptr;
    this->surface_data_size_[index] = 0;
    this->surface_stride_bytes_[index] = 0;
    this->surface_content_width_[index] = 0;
    this->surface_content_height_[index] = 0;
#ifdef USE_P4_VIDEO_RENDERER_H264
    this->surface_layout_size_[index] = 0;
    this->surface_generation_[index] = 0;
#endif
  }
  this->surface_capacity_bytes_ = 0;
  this->presentation_in_flight_.store(false, std::memory_order_release);
  this->pending_surface_.store(-1, std::memory_order_release);
}

void P4VideoRenderer::stop() {
  // Keep the large PSRAM buffers reserved across calls so esp_video can
  // recreate its MMAP queues in the same contiguous regions every time.
  this->set_active(false);
  // A codec or PPA transaction may be uninterruptible. The component-owned
  // worker discards stale generation-tagged AUs and returns to its blocking
  // notification; producer stop never joins it.
  this->rx_session_prepared_.store(false, std::memory_order_release);
  this->dump_state();
}

void P4VideoRenderer::dump_state() const {
  ESP_LOGI(
      TAG,
      "RX state: prepared=%s active=%s generation=%u pending=%d "
      "refresh_busy=%s surfaces=%u bytes_each=%u",
      YESNO(this->rx_session_prepared_.load(std::memory_order_acquire)),
      YESNO(this->rx_active_.load(std::memory_order_acquire)),
      (unsigned)this->rx_session_generation_.load(std::memory_order_acquire),
      this->pending_surface_.load(std::memory_order_acquire),
      YESNO(this->presentation_in_flight_.load(std::memory_order_acquire)),
      (unsigned)size_t(this->surfaces_[0] != nullptr),
      (unsigned)this->surface_capacity_bytes_);
  ESP_LOGI(TAG, "Heap: PSRAM free=%u largest=%u internal free=%u largest=%u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
#ifdef USE_P4_VIDEO_RENDERER_H264
  ESP_LOGI(
      TAG,
      "H.264 RX evidence: admitted=%u rendered=%u presented=%u "
      "refresh_done=%u",
      (unsigned)this->rx_admitted_frames_.load(std::memory_order_relaxed),
      (unsigned)this->rx_rendered_frames_.load(std::memory_order_relaxed),
      (unsigned)this->rx_presented_frames_.load(std::memory_order_relaxed),
      (unsigned)this->rx_refresh_completed_.load(std::memory_order_relaxed));
#endif
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  ESP_LOGI(
      TAG,
      "JPEG RX evidence: admitted=%u rendered=%u presented=%u "
      "refresh_done=%u",
      (unsigned)this->rx_admitted_frames_.load(std::memory_order_relaxed),
      (unsigned)this->rx_rendered_frames_.load(std::memory_order_relaxed),
      (unsigned)this->rx_presented_frames_.load(std::memory_order_relaxed),
      (unsigned)this->rx_refresh_completed_.load(std::memory_order_relaxed));
#endif
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  // RENDER includes synchronous FLUSH callbacks; subtract their deltas outside
  // the firmware for non-flush elapsed time, not exclusive CPU time.
  ESP_LOGI(
      TAG,
      "LVGL timing: render_count=%u render_total_us=%u render_max_us=%u "
      "flush_count=%u flush_total_us=%u flush_max_us=%u flush_w=%u flush_h=%u",
      (unsigned)this->debug_render_count_.load(std::memory_order_relaxed),
      (unsigned)this->debug_render_total_us_.load(std::memory_order_relaxed),
      (unsigned)this->debug_render_max_us_.load(std::memory_order_relaxed),
      (unsigned)this->debug_flush_count_.load(std::memory_order_relaxed),
      (unsigned)this->debug_flush_total_us_.load(std::memory_order_relaxed),
      (unsigned)this->debug_flush_max_us_.load(std::memory_order_relaxed),
      (unsigned)this->debug_flush_width_.load(std::memory_order_relaxed),
      (unsigned)this->debug_flush_height_.load(std::memory_order_relaxed));
#ifdef USE_P4_VIDEO_RENDERER_H264
  ESP_LOGI(
      TAG,
      "H.264 RX stats: admitted=%u rendered=%u rate_drop=%u "
      "busy_drop=%u dependency_drop=%u decode_drop=%u "
      "geometry_drop=%u presented=%u refresh_done=%u",
      (unsigned)this->rx_admitted_frames_.load(std::memory_order_relaxed),
      (unsigned)this->rx_rendered_frames_.load(std::memory_order_relaxed),
      (unsigned)this->rx_rate_drops_.load(std::memory_order_relaxed),
      (unsigned)this->rx_busy_drops_.load(std::memory_order_relaxed),
      (unsigned)this->rx_dependency_drops_.load(std::memory_order_relaxed),
      (unsigned)this->rx_decode_drops_.load(std::memory_order_relaxed),
      (unsigned)this->rx_geometry_drops_.load(std::memory_order_relaxed),
      (unsigned)this->rx_presented_frames_.load(std::memory_order_relaxed),
      (unsigned)this->rx_refresh_completed_.load(std::memory_order_relaxed));
  ESP_LOGI(
      TAG,
      "H.264 RX timing: refresh_max_ms=%u dec_max_us=%u "
      "convert_max_us=%u present_ppa_max_us=%u "
      "au_max_us=%u",
      (unsigned)this->rx_refresh_max_ms_.load(std::memory_order_relaxed),
      (unsigned)this->rx_h264_decode_max_us_.load(std::memory_order_relaxed),
      (unsigned)this->rx_i420_convert_max_us_.load(std::memory_order_relaxed),
      (unsigned)this->rx_present_ppa_max_us_.load(std::memory_order_relaxed),
      (unsigned)this->rx_au_work_max_us_.load(std::memory_order_relaxed));
  ESP_LOGI(
      TAG,
      "H.264 RX queue: peak=%u wait_max_us=%u copy_max_us=%u "
      "copy_total_us=%llu copy_total_bytes=%llu",
      (unsigned)this->rx_queue_high_watermark_.load(std::memory_order_relaxed),
      (unsigned)this->rx_queue_wait_max_us_.load(std::memory_order_relaxed),
      (unsigned)this->rx_au_copy_max_us_.load(std::memory_order_relaxed),
      (unsigned long long)this->rx_au_copy_total_us_.load(
          std::memory_order_relaxed),
      (unsigned long long)this->rx_au_copy_total_bytes_.load(
          std::memory_order_relaxed));
#else
  ESP_LOGI(
      TAG,
      "JPEG scheduling: busy_pending=%u busy_au=%u pending_wait_count=%u "
      "pending_wait_max_us=%u pending_wait_total_us=%u refresh_count=%u "
      "refresh_max_ms=%u refresh_total_ms=%u",
      (unsigned)this->rx_busy_pending_drops_.load(std::memory_order_relaxed),
      (unsigned)this->rx_busy_au_drops_.load(std::memory_order_relaxed),
      (unsigned)this->pending_wait_count_.load(std::memory_order_relaxed),
      (unsigned)this->pending_wait_max_us_.load(std::memory_order_relaxed),
      (unsigned)this->pending_wait_total_us_.load(std::memory_order_relaxed),
      (unsigned)this->rx_refresh_completed_.load(std::memory_order_relaxed),
      (unsigned)this->rx_refresh_max_ms_.load(std::memory_order_relaxed),
      (unsigned)this->rx_refresh_total_ms_.load(std::memory_order_relaxed));
  ESP_LOGI(
      TAG,
      "JPEG RX stats: admitted=%u rendered=%u rate_drop=%u "
      "busy_drop=%u decode_drop=%u presented=%u "
      "refresh_done=%u refresh_max_ms=%u "
      "dec_max_us=%u present_ppa_max_us=%u au_max_us=%u",
      (unsigned)this->rx_admitted_frames_.load(std::memory_order_relaxed),
      (unsigned)this->rx_rendered_frames_.load(std::memory_order_relaxed),
      (unsigned)this->rx_rate_drops_.load(std::memory_order_relaxed),
      (unsigned)this->rx_busy_drops_.load(std::memory_order_relaxed),
      (unsigned)this->rx_decode_drops_.load(std::memory_order_relaxed),
      (unsigned)this->rx_presented_frames_.load(std::memory_order_relaxed),
      (unsigned)this->rx_refresh_completed_.load(std::memory_order_relaxed),
      (unsigned)this->rx_refresh_max_ms_.load(std::memory_order_relaxed),
      (unsigned)this->rx_jpeg_decode_max_us_.load(std::memory_order_relaxed),
      (unsigned)this->rx_present_ppa_max_us_.load(std::memory_order_relaxed),
      (unsigned)this->rx_au_work_max_us_.load(std::memory_order_relaxed));
#endif
#endif
}

} // namespace esphome::p4_video_renderer

#endif
