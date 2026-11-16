#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace esphome::video_renderer {

enum class Codec : uint8_t { JPEG, H264 };
enum class JpegSampling : uint8_t { UNKNOWN, YUV444, YUV422, YUV420, GRAY };

struct JpegFrameInfo {
  uint16_t width{0}, height{0};
  JpegSampling sampling{JpegSampling::UNKNOWN};
  bool is_set() const {
    return this->width != 0 || this->height != 0 || this->sampling != JpegSampling::UNKNOWN;
  }
};

// Microseconds in the producer's presentation timeline. Zero is valid.
// An unavailable timestamp is explicit and never substitutes a different clock.
struct FrameTime {
  uint64_t microseconds{0};
  bool valid{false};
};

struct StreamFormat {
  Codec codec{Codec::JPEG};
  uint16_t width{0}, height{0};
  uint8_t max_fps{0};
  bool valid() const { return (this->codec == Codec::JPEG || this->codec == Codec::H264) &&
      this->width != 0 && this->height != 0 && this->max_fps != 0; }
};

struct RendererCapabilities {
  StreamFormat preferred{};
  uint16_t max_decode_width{0}, max_decode_height{0};
  size_t max_encoded_frame_bytes{0};
  bool requires_lease{false};
};

// Presentation credit uses microseconds, not any network clock. One-us
// timestamp quantization may incur a bounded debt, repaid by later frames.
// This preserves fractional cadence without a timer or accumulated early credit.
class FrameCadence {
 public:
  void reset(uint8_t fps) {
    this->fps_.store(fps == 0 ? 1 : fps, std::memory_order_relaxed);
    this->seen_.store(false, std::memory_order_release);
    this->credit_.store(0, std::memory_order_relaxed);
  }
  bool accept(uint32_t timestamp_us) {
    const uint32_t fps = this->fps_.load(std::memory_order_relaxed);
    if (!this->seen_.exchange(true, std::memory_order_acq_rel)) {
      this->last_.store(timestamp_us, std::memory_order_relaxed);
      return true;
    }
    const uint32_t elapsed = timestamp_us - this->last_.exchange(timestamp_us, std::memory_order_relaxed);
    const uint32_t interval = (1000000U + fps - 1U) / fps;
    int32_t credit = elapsed >= interval * 2U ? 1000000 :
        this->credit_.load(std::memory_order_relaxed) + static_cast<int32_t>(elapsed * fps);
    if (credit + static_cast<int32_t>(fps) < 1000000) {
      this->credit_.store(credit, std::memory_order_relaxed);
      return false;
    }
    this->credit_.store(credit - 1000000, std::memory_order_relaxed);
    return true;
  }
 protected:
  std::atomic<uint32_t> fps_{10}, last_{0};
  std::atomic<int32_t> credit_{0};
  std::atomic<bool> seen_{false};
};

// One complete encoded frame/access unit. With a release callback, true from
// submit_frame transfers this lease: the renderer releases it exactly once,
// including accepted frames later discarded on stop/supersession. False never
// takes ownership and never releases. Without a lease, bytes are borrowed only
// through submit_frame and an asynchronous consumer must copy before returning.
struct EncodedFrame {
  using ReleaseCallback = void (*)(void *);
  const uint8_t *data{nullptr};
  size_t size{0};
  FrameTime presentation_time{};
  bool key_frame{false};
  ReleaseCallback release_callback{nullptr};
  void *release_ctx{nullptr};
  JpegFrameInfo jpeg_info{};
  void release() const { if (this->release_callback != nullptr) this->release_callback(this->release_ctx); }
};

// Decoder resources and presentation remain component-owned. No transport,
// stream negotiation protocol or network session lives in this interface.
class EncodedFrameRenderer {
 public:
  virtual ~EncodedFrameRenderer() = default;
  virtual RendererCapabilities get_capabilities() const = 0;
  virtual bool prepare(const StreamFormat &format) = 0;
  virtual bool start() = 0;
  virtual bool set_active(bool active) = 0;
  virtual void stop() = 0;
  virtual bool submit_frame(const EncodedFrame &frame) = 0;
};

}  // namespace esphome::video_renderer
