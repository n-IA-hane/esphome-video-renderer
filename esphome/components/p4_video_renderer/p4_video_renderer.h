#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP_IDF

#include "esphome/components/mipi_dsi/mipi_dsi.h"
#include "esphome/components/video_renderer/renderer_task.h"
#include "esphome/components/video_renderer/video_renderer.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"

#ifdef USE_P4_VIDEO_RENDERER_JPEG
#include "driver/jpeg_decode.h"
#endif
#if defined(USE_P4_VIDEO_RENDERER_H264) || defined(USE_P4_VIDEO_RENDERER_JPEG)
#include "driver/ppa.h"
#endif
#ifdef USE_P4_VIDEO_RENDERER_H264
#include "esp_h264_dec_sw.h"
#include "esp_imgfx_color_convert.h"
#endif
#include "lvgl.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace esphome::p4_video_renderer {

/// Encoded video display adapter for ESP32-P4.
///
/// The producer owns transport and stream lifecycle. This component receives
/// only complete access units and decodes the compile-time selected codec. LVGL
/// owns page/control state. The display owner transforms a decoded pixel buffer
/// once.
class P4VideoRenderer : public Component,
                        public video_renderer::EncodedFrameRenderer {
public:
  void setup() override;
  void on_shutdown() override;
  void loop() override;
  void dump_config() override;
  void dump_state() const;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void set_width(uint16_t value) { this->width_ = value; }
  void set_height(uint16_t value) { this->height_ = value; }
  void set_framerate(uint8_t value) { this->framerate_ = value; }
  void set_max_decode_width(uint16_t value) { this->max_decode_width_ = value; }
  void set_max_decode_height(uint16_t value) {
    this->max_decode_height_ = value;
  }

  void set_display(mipi_dsi::MipiDsi *value) { this->display_ = value; }

  /// Called from lvgl.on_boot after the YAML container exists.
  void attach_video_container(lv_obj_t *container);
  bool has_remote_frame() const {
#ifdef USE_P4_VIDEO_RENDERER_JPEG
    const uint32_t generation =
        this->remote_frame_visible_.load(std::memory_order_acquire);
    return this->rx_active_.load(std::memory_order_acquire) &&
           generation != 0 &&
           generation ==
               this->rx_session_generation_.load(std::memory_order_acquire);
#else
    return this->rx_active_.load(std::memory_order_acquire) &&
           this->remote_frame_visible_.load(std::memory_order_acquire);
#endif
  }
  Trigger<> *get_first_frame_trigger() { return &this->first_frame_trigger_; }
  Trigger<> *get_video_ended_trigger() { return &this->video_ended_trigger_; }

  video_renderer::RendererCapabilities get_capabilities() const override;
  bool start() override;
  bool prepare(const video_renderer::StreamFormat &capability) override;
  bool set_active(bool active) override;
  void stop() override;
  bool submit_frame(const video_renderer::EncodedFrame &access_unit) override;

protected:
#ifdef USE_P4_VIDEO_RENDERER_H264
  // Complete H.264 access units are bounded at 128 KiB.
  // Three metadata slots lease producer-owned frames; no encoded frame copy
  // or pixel allocation enters the hot path.
  static constexpr size_t kMaxAccessUnitBytes = 128 * 1024;
  static constexpr size_t kH264AccessUnitQueueDepth = 3;
#else
  // Independent JPEG frames can be substantially larger.
  static constexpr size_t kMaxAccessUnitBytes = 512 * 1024;
#endif
  static constexpr uint32_t kTaskStackBytes = 12288;
#ifdef USE_P4_VIDEO_RENDERER_H264
  static constexpr bool kTaskStackInPsram = true;
#else
  static constexpr bool kTaskStackInPsram = false;
#endif
  // Match Espressif's dual-task decoder priority. Audio/AFE stays above it at
  // 18-19, while the two decoder halves can run in parallel on separate cores.
  static constexpr uint8_t kTaskPriority = 17;
  static constexpr TickType_t kTaskStopTimeoutTicks =
      configTICK_RATE_HZ >= 2 ? configTICK_RATE_HZ / 2 : 1;

  bool allocate_session_resources_();
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  bool init_jpeg_decoder_();
  bool read_jpeg_picture_info_(const uint8_t *data, size_t size,
                               const video_renderer::JpegFrameInfo &frame_info,
                               jpeg_decode_picture_info_t *picture) const;
  static uint16_t
  jpeg_storage_width_(const jpeg_decode_picture_info_t &picture);
  static uint16_t
  jpeg_storage_height_(const jpeg_decode_picture_info_t &picture);
#endif
  bool present_surface_direct_(int index);
#ifdef USE_P4_VIDEO_RENDERER_H264
  // H.264 Baseline Level 3.0 limits a decoded picture to 1620 macroblocks.
  static constexpr size_t kH264Level30MaxMacroblocks = 1620;
  static constexpr uint16_t kPpaScaleUnits = 16;
  size_t h264_optimized_yuv_bytes_() const {
    return static_cast<size_t>(this->max_decode_width_) *
           this->max_decode_height_ * 3 / 2;
  }
  bool reset_h264_decoder_();
  bool h264_resolution_fits_(uint16_t width, uint16_t height) const;
  bool decode_h264_access_unit_(const uint8_t *data, size_t size,
                                uint64_t timestamp_us, bool timestamp_valid,
                                bool key_frame, uint32_t session_generation,
                                uint32_t loss_generation, bool &decoded);
  bool configure_i420_converter_(uint16_t width, uint16_t height);

  bool render_i420_(const uint8_t *i420, size_t size, uint16_t width,
                    uint16_t height, uint32_t session_generation);
#endif
  void free_codec_resources_();
  void free_unpublished_surfaces_();

  static void rx_task_trampoline_(void *ctx);
  void rx_task_();
  bool start_rx_task_();
  bool stop_rx_task_();
  bool reap_rx_task_();
  static void display_refresh_ready_callback_(lv_event_t *event);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  static void display_debug_callback_(lv_event_t *event);
  void on_display_debug_event_(lv_event_code_t code, const lv_area_t *area);
#endif
  void on_display_refresh_ready_();
  bool refresh_layout_();

  uint16_t width_{640};
  uint16_t height_{480};
  uint8_t framerate_{10};
  uint16_t max_decode_width_{1280};
  uint16_t max_decode_height_{800};

#ifdef USE_P4_VIDEO_RENDERER_H264
  struct H264AccessUnitSlot {
    video_renderer::EncodedFrame access_unit{};
    uint32_t session_generation{0};
    uint32_t loss_generation{0};
    bool key_frame{false};
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
    uint32_t queued_at_us{0};
#endif
    std::atomic<uint8_t> state{0}; // 0=free, 1=queued, 2=owned
  };
  H264AccessUnitSlot h264_au_slots_[kH264AccessUnitQueueDepth]{};
  QueueHandle_t h264_au_queue_{nullptr};
  StaticQueue_t h264_au_queue_control_{};
  uint8_t h264_au_queue_storage_[kH264AccessUnitQueueDepth]{};
#else
  uint8_t *rx_au_{nullptr};
  size_t rx_au_capacity_{0};
#endif
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  jpeg_decoder_handle_t jpeg_decoder_{nullptr};
  jpeg_decode_cfg_t jpeg_decode_config_{};
  video_renderer::JpegFrameInfo rx_au_jpeg_info_{};
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  std::atomic<uint32_t> rx_jpeg_decode_max_us_{0};
  std::atomic<uint32_t> rx_au_work_max_us_{0};
  std::atomic<uint32_t> rx_busy_pending_drops_{0};
  std::atomic<uint32_t> rx_busy_au_drops_{0};
  std::atomic<uint32_t> pending_published_us_{0};
  std::atomic<uint32_t> pending_wait_count_{0};
  std::atomic<uint32_t> pending_wait_max_us_{0};
  std::atomic<uint32_t> pending_wait_total_us_{0};
#endif
#endif
#ifdef USE_P4_VIDEO_RENDERER_H264
  esp_h264_dec_handle_t h264_decoder_{nullptr};
  esp_imgfx_color_convert_handle_t i420_converter_{nullptr};
  uint16_t i420_converter_width_{0};
  uint16_t i420_converter_height_{0};
  bool i420_converter_failure_logged_{false};
  uint8_t *optimized_yuv420_{nullptr};
  size_t optimized_yuv420_capacity_{0};
  bool h264_first_au_logged_{false};
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  bool h264_decode_failure_logged_{false};
  std::atomic<uint32_t> rx_h264_decode_max_us_{0};
  std::atomic<uint32_t> rx_i420_convert_max_us_{0};
  std::atomic<uint32_t> rx_au_work_max_us_{0};
#endif
#endif
  mipi_dsi::MipiDsi *display_{nullptr};
  std::atomic<uint32_t> layout_size_{0};
  static constexpr size_t surface_count_{1};
  uint8_t *surfaces_[surface_count_]{nullptr};
  size_t surface_capacity_bytes_{0};
  size_t surface_data_size_[surface_count_]{0};
  uint16_t surface_stride_bytes_[surface_count_]{0};
  uint16_t surface_content_width_[surface_count_]{0};
  uint16_t surface_content_height_[surface_count_]{0};
#ifdef USE_P4_VIDEO_RENDERER_H264
  uint32_t surface_layout_size_[surface_count_]{0};
  uint32_t surface_generation_[surface_count_]{0};
#endif

  video_renderer::StreamFormat rx_format_{};
#ifdef USE_P4_VIDEO_RENDERER_H264
  video_renderer::FrameCadence cadence_{};
#endif
  std::atomic<bool> rx_running_{false};
  std::atomic<bool> rx_active_{false};
  std::atomic<bool> rx_session_prepared_{false};
  std::atomic<uint32_t> rx_session_generation_{0};
#ifndef USE_P4_VIDEO_RENDERER_H264
  std::atomic<uint8_t> rx_slot_state_{0}; // 0=free, 1=ready, 2=owned
#endif
#ifndef USE_P4_VIDEO_RENDERER_H264
  size_t rx_au_size_{0};
  uint64_t rx_timestamp_us_{0};
  uint32_t rx_au_generation_{0};
  bool rx_key_frame_{false};
#endif
#ifdef USE_P4_VIDEO_RENDERER_H264
  std::atomic<bool> waiting_for_key_frame_{true};
  // Recreate tinyH264 only after an actual decoder error. Ordinary session
  // changes and locally dropped reference pictures recover on the next IDR.
  std::atomic<bool> decoder_reset_pending_{false};
  std::atomic<uint32_t> loss_generation_{0};
#endif
  TaskHandle_t rx_task_handle_{nullptr};
#ifndef USE_ESP_IDF
  StaticTask_t rx_task_tcb_{};
#endif
  StackType_t *rx_task_stack_{nullptr};
  bool rx_task_with_caps_{false};
  SemaphoreHandle_t rx_done_{nullptr};
  StaticSemaphore_t rx_done_storage_{};
  bool rx_done_observed_{false};
  // Serializes main-loop presentation state with stop(). Decoder publish
  // sites recheck the active session generation after every blocking codec or
  // PPA operation, so the persistent worker remains entirely frame-driven.
  SemaphoreHandle_t presentation_mutex_{nullptr};
  StaticSemaphore_t presentation_mutex_storage_{};

  lv_obj_t *video_container_{nullptr};
  std::atomic<int> pending_surface_{-1};
#ifdef USE_P4_VIDEO_RENDERER_JPEG
  // Published with pending_surface_, independent of reusable AU slot metadata.
  std::atomic<uint32_t> pending_surface_generation_{0};
  // Published frame generation, not another epoch. Zero means no visible
  // frame; unlike a bool, a late old publication cannot reopen a new call.
  std::atomic<uint32_t> remote_frame_visible_{0};
#endif
  // The first-page repaint must complete before committing its pending pixels.
  // Decoded storage remains pending through the blocking display transaction.
  std::atomic<bool> presentation_in_flight_{false};
  std::atomic<bool> surface_ever_presented_{false};
#ifndef USE_P4_VIDEO_RENDERER_JPEG
  std::atomic<bool> remote_frame_visible_{false};
#endif
  std::atomic<bool> video_ended_pending_{false};
  std::atomic<uint32_t> rx_admitted_frames_{0};
  std::atomic<uint32_t> rx_rendered_frames_{0};
  std::atomic<uint32_t> rx_presented_frames_{0};
  std::atomic<uint32_t> rx_refresh_completed_{0};
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  std::atomic<uint32_t> rx_refresh_max_ms_{0};
  std::atomic<uint32_t> rx_refresh_total_ms_{0};
  std::atomic<uint32_t> debug_render_started_us_{0};
  std::atomic<uint32_t> debug_flush_started_us_{0};
  std::atomic<uint32_t> debug_render_count_{0};
  std::atomic<uint32_t> debug_render_total_us_{0};
  std::atomic<uint32_t> debug_render_max_us_{0};
  std::atomic<uint32_t> debug_flush_count_{0};
  std::atomic<uint32_t> debug_flush_total_us_{0};
  std::atomic<uint32_t> debug_flush_max_us_{0};
  std::atomic<uint32_t> debug_flush_width_{0};
  std::atomic<uint32_t> debug_flush_height_{0};
  std::atomic<uint32_t> rx_present_ppa_max_us_{0};
  std::atomic<uint32_t> presentation_started_ms_{0};
  std::atomic<uint32_t> rx_rate_drops_{0};
  std::atomic<uint32_t> rx_busy_drops_{0};
  std::atomic<uint32_t> rx_decode_drops_{0};
#ifdef USE_P4_VIDEO_RENDERER_H264
  std::atomic<uint32_t> rx_queue_high_watermark_{0};
  std::atomic<uint32_t> rx_queue_wait_max_us_{0};
  std::atomic<uint32_t> rx_dependency_drops_{0};
  std::atomic<uint32_t> rx_geometry_drops_{0};
  std::atomic<uint32_t> rx_au_copy_max_us_{0};
  std::atomic<uint64_t> rx_au_copy_total_us_{0};
  std::atomic<uint64_t> rx_au_copy_total_bytes_{0};
#endif
#endif
  Trigger<> first_frame_trigger_;
  Trigger<> video_ended_trigger_;
};

} // namespace esphome::p4_video_renderer

#endif
