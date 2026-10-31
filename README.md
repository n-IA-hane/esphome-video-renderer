# ESPHome Video Renderer

A bounded baseline JPEG renderer for ESPHome and LVGL. An image producer supplies
complete JPEGs, and `esp_video_renderer` decodes them to RGB565 and presents them
inside an existing LVGL container. It owns decoder resources, scheduling and
image lifetime. Your application owns the image source, display configuration,
container and page navigation.

**ESPHome 2026.10.0 or newer, ESP-IDF, ESP32-S3 or ESP32-P4, and LVGL are required.**
The default configuration also requires working PSRAM. This repository has its
own development version in [VERSION](VERSION) and history in
[CHANGELOG.md](CHANGELOG.md).

The component works without VoIP, SIP, RTP, Home Assistant, a camera or an HTTP
server. An optional adapter connects the same renderer to the existing
[ESPHome VoIP Stack](https://github.com/n-IA-hane/esphome-voip-stack) video sink.
There is one rendering implementation in both configurations.

## Install and display JPEG images

Use the independent repository through ESPHome's native
[external components](https://esphome.io/components/external_components/):

```yaml
external_components:
  - source: github://n-IA-hane/esphome-video-renderer@dev
    components: [esp_video_renderer]
```

The `dev` branch tracks development. Pin a reviewed commit when reproducible
builds are required. Keep your board's `spi`, `display`, `psram` and
[LVGL](https://esphome.io/components/lvgl/) declarations, then add:

```yaml
esp_video_renderer:
  id: incoming_video
  width: 320
  height: 240
  framerate: 2

lvgl:
  on_boot:
    - lambda: id(incoming_video).attach_video_container(id(video_surface));
  pages:
    - id: video_page
      widgets:
        - obj:
            id: video_surface
            width: 320
            height: 240
            pad_all: 0
            border_width: 0
```

Start from an existing automation:

```yaml
- esp_video_renderer.start: incoming_video
```

An existing source component or lambda can submit a complete image:

```cpp
bool accepted = id(incoming_video).submit_jpeg(jpeg_data, jpeg_length);
```

The data is borrowed only for this function call. On admission the renderer
copies it before returning, so the source can immediately reuse its buffer.
`false` means no image was accepted, for example because the receiver is stopped,
the rate budget is exhausted, the input is too large, or the mailbox is busy.
An accepted image still undergoes full JPEG validation and can subsequently
fail decoding. The call neither transfers ownership nor blocks until display.

Stop from an automation with `esp_video_renderer.stop: incoming_video`.
`start()` and `stop()` are also available from C++. The renderer reserves its
resources at boot; starting and stopping preserve those allocations.

[examples/standalone.yaml](examples/standalone.yaml) is a complete local-source
ESP32-S3/ST7789V compile fixture. It includes
[examples/display-base.yaml](examples/display-base.yaml), which contains the
board, PSRAM, display and LVGL configuration. Adjust pins and PSRAM mode to the
actual board. [examples/standalone-remote.yaml](examples/standalone-remote.yaml)
uses the independent GitHub source with the same display fixture. Neither
example supplies an image source; compilation checks the real renderer and
display integration without introducing an artificial transport.

## Configuration and budgets

| Option | Default | Contract |
| --- | --- | --- |
| `width` | `320` | Maximum decoded width, 8-640, multiple of 8. |
| `height` | `240` | Maximum decoded height, 8-640, multiple of 8. |
| `framerate` | `2` | Admission ceiling, 1-15 FPS. |
| `max_jpeg_bytes` | `131072` | Bytes in each compressed slot, 4096-524288. |
| `task_core` | `any` | Worker affinity: `any`, `0` or `1`; the core must exist. |
| `task_priority` | `5` | FreeRTOS worker priority, 1-24. |
| `task_stack_size` | `8192` | Bytes, 4096-65536, multiple of 16. |
| `task_stack_in_psram` | `true` | Reserve the worker stack in PSRAM. |
| `buffer_in_psram` | `true` | Reserve JPEG slots, RGB surfaces and decoder workspace in PSRAM. |
| `voip.id` | absent | Explicit optional VoIP `EncodedVideoSink` adapter. |
| `on_first_frame` | absent | Automation when the session's first image is submitted to LVGL. |
| `on_video_ended` | absent | Automation delivered in the main loop after deactivation. |

The decoded area must not exceed 640x480 pixels. Dimensions bound storage, rather
than requiring every source image to have that exact size. A portrait surface is
also supported within the same area budget.

The image buffers reserve `2 * max_jpeg_bytes + 4 * width * height` bytes.
Defaults reserve 569,344 bytes, plus a 3,100-byte decoder workspace and an 8 KiB
worker stack. A source-decoder build with `CONFIG_JD_FASTDECODE=2` uses a
65,472-byte workspace instead. Task control structures stay in internal memory.
LVGL buffers, your display driver, source acquisition and any transport or audio
stack need their own budgets.

Both placement options require a declared `psram` component when true. Failed
allocation fails component setup explicitly; there is no silent internal-memory
fallback. Choosing internal placement requires reducing capacities to fit the
available memory. Affinity and priority are application choices: account for
Wi-Fi, display transfers and other real-time tasks before changing them.

The generic admission ceiling is 2 FPS. A maintained Spotpear profile in the
[Intercom project](https://github.com/n-IA-hane/esphome-intercom) uses a 10 FPS
ceiling for its qualified configuration. That is a board/profile choice, not a
universal performance guarantee. Actual output depends on JPEG dimensions and
complexity, decoder settings, display bandwidth, memory placement, Wi-Fi and
concurrent features. Supplying images near the destination resolution avoids
spending decoder time on detail that would be discarded.

## JPEG format and lifetime

The decoder is Espressif's
[`esp_jpeg` 1.3.2](https://components.espressif.com/components/espressif/esp_jpeg/versions/1.3.2/readme).
The backend is software JPEG on both supported chips, using the vendor ROM
implementation when its default configuration selects it. ESP32-P4's hardware
JPEG/H.264 pipeline remains a separate component in VoIP Stack. This renderer
supports complete single-scan baseline JPEGs, not progressive JPEG or H.264.

Before decoding, the renderer validates bounded headers, geometry, tables, scan
structure and the end marker. Native input is bounded to four times each output
dimension, at most 1280 pixels per axis and 1,228,800 pixels total, as well as
`max_jpeg_bytes`. The decoder chooses native, half or quarter size, using the
finest reduction that fits both dimensions. For a 320x240 surface, 640x360 and
1280x720 input both produce 320x180 output. Smaller images retain native size.
Odd reduced dimensions round down, dropping the last partial row or column.
Images are centered without upscaling or aspect distortion.

One worker blocks on notifications when idle. It owns two JPEG slots, and keeps
only the latest queued image while an in-flight decode retains its own slot.
Two RGB565 surfaces separate decoder output from display input. A decoded back
surface remains reserved until the main loop takes it. With both surfaces
occupied, decoding waits for the handoff notification while newer compressed
images may replace the queued successor. Work and memory remain bounded under
sustained input. The wall-clock admission budget retains one frame of recovery
credit for small arrival jitter; long idle gaps cannot accumulate unbounded work.

Only the main loop calls LVGL. The front surface stays immutable while LVGL
references it; refresh completion permits the next surface swap. Session
generations discard stale work after stop, suspension or restart. Shutdown keeps
possibly referenced surfaces until reset rather than freeing display memory
prematurely. Attach the container once from `lvgl.on_boot` and keep it alive for
the renderer's lifetime.

`on_first_frame` indicates submission to LVGL, and `on_video_ended` follows
deactivation in the main loop. Your automations can change pages or apply other
application policy. `get_stats()` exposes admitted, replaced and rejected frames,
decodes, errors, LVGL submissions, maximum decode elapsed time and maximum frame
age at submission. `dump_state()` logs these counters at INFO on demand. Elapsed
decode time includes scheduling; it is not exclusive CPU time. Submission counts
are not physical panel qualification.

## Optional VoIP adapter

Load the independent renderer and VoIP sources explicitly:

```yaml
external_components:
  - source: github://n-IA-hane/esphome-video-renderer@dev
    components: [esp_video_renderer]
  - source: github://n-IA-hane/esphome-voip-stack@dev
    components: [voip_stack]

esp_video_renderer:
  id: incoming_video
  width: 320
  height: 240
  framerate: 2
  voip:
    id: incoming_video_sink

voip_stack:
  video:
    codec: jpeg
    sink: incoming_video_sink
    width: 320
    height: 240
    framerate: 2
```

The rest of the native VoIP microphone/speaker, network and display configuration
is still required. [examples/voip.yaml](examples/voip.yaml) supplies a complete
microphone-only outgoing-audio/display example once Wi-Fi secrets and board pins
are provided. Add a supported speaker and camera through VoIP Stack's own
configuration when needed.

The `voip:` block creates `EspVideoRendererSink` under
`USE_ESP_VIDEO_RENDERER_VOIP`, using VoIP Stack's existing `EncodedVideoSink` and
`EncodedVideoAccessUnit` interfaces. Standalone builds do not load or compile the
VoIP component. An accepted leased access unit is copied and released by the
adapter; a rejected unit remains the caller's responsibility. SIP/SDP negotiation,
RTP, JPEG reassembly, camera acquisition and call lifecycle remain with their
existing owners. This repository does not create a second media engine or call
state machine. VoIP-specific image-size negotiation and wake-word policy belong
to VoIP Stack and the application packages.

## Development and validation

Install the pinned test environment and run checks from this checkout:

```sh
python -m venv .venv
.venv/bin/pip install -r requirements-dev.txt
.venv/bin/python -m ruff check esphome tests tools
.venv/bin/python -m pytest -q
.venv/bin/python -m esphome compile examples/standalone.yaml
```

The five focused test modules exercise the production renderer's threaded
lifecycle, stale generations and borrowed-buffer lifetime, refresh handoff and
sustained input, admission jitter/clock wrap/overload, bounded malformed-JPEG
rejection and native/half/quarter decode planning. Configuration tests generate
real ESPHome sources and prove standalone builds omit SIP. Adapter/codegen tests
need a separate actual VoIP Stack checkout and skip explicitly when it is absent:

```sh
VOIP_STACK_SOURCE=../esphome-voip-stack .venv/bin/python -m pytest -q
```

`VOIP_STACK_SOURCE` points to the repository root, not its components directory.
CI fetches that optional dependency and executes the tests against its real
header; it also compiles and links the standalone ESP32-S3 display fixture with
ESPHome 2026.10.0. The renderer source is never replaced by a test implementation.
Host mocks model RTOS, decoder and display boundaries; physical scheduling,
ROM execution and panel output still require identified hardware.

For independent pixels and sanitizers, use the official component downloaded by
the SDK build:

```sh
.venv/bin/python tools/qualify_esp_video_renderer_jpeg.py \
  --component examples/.esphome/build/jpeg-display/managed_components/espressif__esp_jpeg \
  --output test-results/pixels
```

This tool executes the official wrapper and source decoder under ASan/UBSan,
checks RGB565 byte order and color error for 4:4:4, 4:2:2, 4:2:0 and grayscale at
native/half/quarter size, odd dimensions, output bounds, malformed headers and
truncated prefixes. Add `--input-jpeg captured.jpg` to replay a real source image.
It writes decoded PNGs and a JSON receipt. Set `TMPDIR` to persistent build
storage where required by the development environment.

## Source and license

Extracted from
[`n-IA-hane/esphome-voip-stack`](https://github.com/n-IA-hane/esphome-voip-stack),
with component origin and hashes recorded in [SOURCE.md](SOURCE.md). The initial
five component files are byte-for-byte identical to that source revision.
MIT license, preserving the original notice in [LICENSE](LICENSE). ESPHome and
Espressif dependencies retain their own licenses and are fetched by the official
builder, rather than copied into this repository.
