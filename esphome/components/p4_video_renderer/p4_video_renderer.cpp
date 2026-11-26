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

} // namespace

#ifdef USE_P4_VIDEO_RENDERER_DEBUG
void P4VideoRenderer::update_max(std::atomic<uint32_t> &maximum, uint32_t value) {
  uint32_t current = maximum.load(std::memory_order_relaxed);
  while (value > current && !maximum.compare_exchange_weak(
                                current, value, std::memory_order_relaxed)) {
  }
}
#endif

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
  if (active) {
    this->cadence_.reset(std::max<uint8_t>(
        1, std::min(this->framerate_, this->rx_format_.max_fps == 0
                                          ? this->framerate_
                                          : this->rx_format_.max_fps)));
#ifdef USE_P4_VIDEO_RENDERER_H264
    this->waiting_for_key_frame_.store(true, std::memory_order_release);
#endif
    this->rx_active_.store(true, std::memory_order_release);
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
  // JPEG admission protects decoder work, independently of a producer's
  // presentation clock. Arrival pacing also covers absent/backwards timestamps;
  // the original presentation-time validity is never rewritten.
  if (!this->cadence_.accept(micros())) {
    this->rx_slot_state_.store(0, std::memory_order_release);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
    this->rx_rate_drops_.fetch_add(1, std::memory_order_relaxed);
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
    const bool rendered = this->decode_jpeg_access_unit_(session_generation);
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
