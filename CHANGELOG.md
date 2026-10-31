# Changelog

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
