# Changelog

## 2026.12.0-dev

- Restore exposed P4 video bounds through LVGL when aspect ratio, size or
  position changes. The pending frame remains leased until the background
  repaint completes; equal-geometry frames retain the direct transform path.
- Move the hardware-accelerated P4 renderer into this repository alongside the
  S3 renderer. Both accept complete encoded frames through the same small,
  protocol-independent contract, with stream generations and explicit buffer
  release. Rendering no longer imports VoIP configuration or transport types.
- Keep SIP/RTP adapters in VoIP Stack. Referencing a renderer as the video sink
  creates its adapter automatically; no renderer mode or VoIP enable flag is
  required. Existing component names and configured IDs are preserved.
- Commit P4 video through the compatible MIPI display's public owned transform.
  The driver owns RGB/YUV conversion, scale, rotation, framebuffer and UI/video
  serialization. LVGL supplies the logical rectangle and page visibility;
  P4 does not create a video image widget or scaled intermediate RGB surface.
  Retain hardware JPEG, bounded preallocation and H.264 IDR recovery.
- Keep the S3 software renderer's LVGL image and refresh-complete lifetime.
  Existing renderer IDs and container hooks are unchanged; display rotation is
  composed once by the P4 display owner.
- Add a complete P4 encoded-frame example with no VoIP component, credentials,
  camera or network. The producer owns frame timestamps and passes complete
  JPEG bytes through the renderer's normal API.

The standalone P4 example requires ESPHome 2026.11.0 or newer and the
declared P4 display capability. Default-SDK receive-video
checks measured 9.696 and 9.941 FPS with a 10 FPS source. The repeat retained
24 browser startup audio-playout drops and failed that strict probe. These
bounded checks do not complete optical panel or acoustic qualification.
A matching forward check on the true SDK 6.0.3 tag measured 9.824 native FPS,
with zero browser playout drops/underruns, zero RTP loss and complete cleanup.
The recommended-SDK firmware was restored afterwards. A valid framebuffer
capture cannot establish absence of transient physical-panel flashes.

The standalone P4 example follows ESPHome's recommended SDK and acceleration
selection. Its separate CI job compiles the local frame producer and checks
that the selected MIPI source is used without VoIP, API, network or camera
components. The same minimal producer compiled and linked locally with official
Core 373 and its recommended SDK 5.5.6, using the actual prepared driver source.
No firmware was uploaded. Remote CI for the unpublished commit remains a
separate gate.

## 2026.11.1-dev

Initial standalone extraction of `esp_video_renderer` from ESPHome VoIP Stack.
The five component source files are unchanged by the extraction.

- Baseline JPEG to LVGL RGB565 presentation on ESP32-S3 and ESP32-P4 with ESP-IDF.
- Two bounded JPEG slots and two RGB565 surfaces, latest queued image replacement,
  notification-driven decoding and refresh-complete surface ownership.
- Explicit task affinity, priority, stack and PSRAM placement options.
- Optional adapter for the existing VoIP Stack `EncodedVideoSink` contract.
- Independent configuration, host lifecycle, admission and malformed-JPEG tests.

Requires ESPHome 2026.10.0 or newer. The extraction does not change firmware
already installed on a device or constitute new physical display qualification.
