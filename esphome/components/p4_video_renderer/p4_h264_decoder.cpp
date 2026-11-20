#include "p4_video_renderer.h"

#if defined(USE_ESP_IDF) && defined(USE_P4_VIDEO_RENDERER_H264)

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include "esp_heap_caps.h"

namespace esphome::p4_video_renderer {

static const char *const TAG = "p4_video_renderer";

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

bool P4VideoRenderer::consume_h264_nal_(esp_h264_dec_in_frame_t &input,
                                        esp_h264_dec_out_frame_t &output) {
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
               static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM |
                                                             MALLOC_CAP_8BIT)),
               static_cast<unsigned>(heap_caps_get_largest_free_block(
                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
               static_cast<unsigned>(heap_caps_get_free_size(
                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
               static_cast<unsigned>(heap_caps_get_largest_free_block(
                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    }
#endif
    return false;
  }
  input.raw_data.buffer += input.consume;
  input.raw_data.len -= input.consume;
  return true;
}

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
    if (!this->consume_h264_nal_(input, output)) {
      decode_failed = true;
      break;
    }
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

} // namespace esphome::p4_video_renderer

#endif
