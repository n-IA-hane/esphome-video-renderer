#include "p4_video_renderer.h"

#if defined(USE_ESP_IDF) && defined(USE_P4_VIDEO_RENDERER_JPEG)

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::p4_video_renderer {

static const char *const TAG = "p4_video_renderer";

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

bool P4VideoRenderer::decode_jpeg_access_unit_(uint32_t session_generation) {
  if (this->pending_surface_.load(std::memory_order_acquire) >= 0 ||
      this->jpeg_decoder_ == nullptr)
    return false;

  jpeg_decode_picture_info_t picture{};
  if (!this->read_jpeg_picture_info_(this->rx_au_, this->rx_au_size_,
                                     this->rx_au_jpeg_info_, &picture))
    return false;

  const uint16_t storage_width = jpeg_storage_width_(picture);
  const uint16_t storage_height = jpeg_storage_height_(picture);
  const size_t expected_size =
      static_cast<size_t>(storage_width) * storage_height * 2U;
  if (expected_size > UINT32_MAX)
    return false;
  uint32_t decoded_size = 0;
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  const uint32_t started = micros();
#endif
  // ESP-IDF synchronizes outbuf_size bytes around DMA. Limit that range to
  // this frame's padded size, already checked against the single surface.
  const esp_err_t error = jpeg_decoder_process(
      this->jpeg_decoder_, &this->jpeg_decode_config_, this->rx_au_,
      static_cast<uint32_t>(this->rx_au_size_), this->surfaces_[0],
      static_cast<uint32_t>(expected_size), &decoded_size);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  update_max(this->rx_jpeg_decode_max_us_, micros() - started);
#endif
  if (error != ESP_OK || decoded_size != expected_size ||
      !this->rx_active_.load(std::memory_order_acquire) ||
      !this->rx_session_prepared_.load(std::memory_order_acquire) ||
      session_generation !=
          this->rx_session_generation_.load(std::memory_order_acquire))
    return false;

  this->surface_data_size_[0] = decoded_size;
  this->surface_stride_bytes_[0] = storage_width * sizeof(uint16_t);
  this->surface_content_width_[0] = static_cast<uint16_t>(picture.width);
  this->surface_content_height_[0] = static_cast<uint16_t>(picture.height);
#ifdef USE_P4_VIDEO_RENDERER_DEBUG
  this->pending_published_us_.store(micros(), std::memory_order_relaxed);
#endif
  this->pending_surface_generation_.store(session_generation,
                                          std::memory_order_relaxed);
  this->pending_surface_.store(0, std::memory_order_release);
  this->enable_loop_soon_any_context();
  return true;
}

} // namespace esphome::p4_video_renderer

#endif
