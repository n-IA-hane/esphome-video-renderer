# ESPHome Video Renderer

A bounded video renderer for ESPHome. An image producer supplies complete
encoded frames. The software `esp_video_renderer` decodes JPEG to an LVGL image;
`p4_video_renderer` uses hardware JPEG or software H.264 and a display-owned
pixel transaction. The renderer owns decoder resources, scheduling and input
lifetime. Your application owns the image source, display configuration,
container and page navigation.

**ESP-IDF, ESP32-S3 or ESP32-P4, and LVGL are required.** The coordinated
development baseline uses the official ESPHome 2026.11.0 release.
The default configuration also requires working PSRAM. This repository has its
own development version in [VERSION](VERSION) and history in
[CHANGELOG.md](CHANGELOG.md).

The component works without VoIP, SIP, RTP, Home Assistant, a camera or an HTTP
server. Adapters in
[ESPHome VoIP Stack](https://github.com/n-IA-hane/esphome-voip-stack) connect the
same renderer to that component's video sink.
There is one rendering implementation in both configurations.

## Install and display JPEG images

Use the independent repository through ESPHome's native
[external components](https://esphome.io/components/external_components/):

```yaml
external_components:
  - source: github://n-IA-hane/esphome-video-renderer@dev
    components: [video_renderer, esp_video_renderer]
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

The following settings and software JPEG lifetime describe `esp_video_renderer`.
The [P4 ownership contract](docs/MIGRATION_RENDERER_OWNERSHIP.md#display-presentation)
describes the hardware renderer separately.

| Option | Default | Contract |
| --- | --- | --- |
| `width` | `320` | Maximum decoded width, 8-640, multiple of 8. |
| `height` | `240` | Maximum decoded height, 8-640, multiple of 8. |
| `max_decode_width` | omitted | Optional JPEG input width, multiple of 8, at most 1280 and four times `width`. Sessions default to `width`. |
| `max_decode_height` | omitted | Optional JPEG input height, multiple of 8, at most 1280 and four times `height`. Sessions default to `height`. |
| `framerate` | `2` | Configured admission ceiling, 1-60 FPS. Actual decode and display rates depend on the board and workload. |
| `max_jpeg_bytes` | `131072` | Bytes in each compressed slot, 4096-524288. |
| `task_core` | `any` | Worker affinity: `any`, `0` or `1`; the core must exist. |
| `task_priority` | `5` | FreeRTOS worker priority, 1-24. |
| `task_stack_size` | `8192` | Bytes, 4096-65536, multiple of 16. |
| `task_stack_in_psram` | `true` | Reserve the worker stack in PSRAM. |
| `buffer_in_psram` | `true` | Reserve JPEG slots, RGB surfaces and decoder workspace in PSRAM. |
| `on_first_frame` | absent | Automation when the session's first image is submitted to LVGL. |
| `on_video_ended` | absent | Automation delivered in the main loop after deactivation. |

The decoded area must not exceed 640x480 pixels. Dimensions bound storage, rather
than requiring every source image to have that exact size. A portrait surface is
also supported within the same area budget.

The image buffers reserve `2 * max_jpeg_bytes + 4 * width * height` bytes.
Defaults reserve 569,344 bytes, plus a 3,100-byte ROM/basic decoder workspace and
an 8 KiB worker stack. A portable decoder build with `CONFIG_JD_FASTDECODE=1`
uses 3,488 bytes to accommodate 4:2:0 MCU storage with the default 512-byte
stream buffer. `CONFIG_JD_FASTDECODE=2` uses 65,472 bytes instead. Task control
structures stay in internal memory.
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
This component uses software JPEG, including the vendor ROM implementation
when selected by its default configuration. P4's hardware JPEG/software H.264
renderer is the separate `p4_video_renderer` component in this repository, not
in VoIP Stack. The software component supports complete single-scan baseline
JPEGs, not progressive JPEG or H.264.

Before decoding, the renderer validates bounded headers, geometry, tables, scan
structure and the end marker. Native input is bounded to four times each output
dimension, at most 1280 pixels per axis and 1,228,800 pixels total, as well as
`max_jpeg_bytes`. The decoder chooses native, half or quarter size, using the
finest reduction that fits both dimensions. For a 320x240 surface, 640x360 and
1280x720 input both produce 320x180 output. Smaller images retain native size.
Odd reduced dimensions round down, dropping the last partial row or column.
Images are centered without upscaling or aspect distortion.

An explicit `max_decode_width` or `max_decode_height` limits both declared
streams and actual encoded frames. Omitted input limits preserve the raw
`submit_jpeg()` reduction envelope above; declared sessions still default to
`width` and `height`. The input rectangle may not exceed 1,228,800 pixels.
For example, a 320x240 renderer with both input limits set to 800 can receive
800x800 JPEG and decode it directly to a centered 200x200 image. The compressed
buffers, RGB565 surfaces and worker remain the same size. A larger input raises
decode work even though it does not reserve a larger decoded surface.

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
    components: [video_renderer, esp_video_renderer]
  - source: github://n-IA-hane/esphome-voip-stack@dev
    components: [voip_stack]

esp_video_renderer:
  id: incoming_video
  width: 320
  height: 240
  framerate: 2

voip_stack:
  video:
    codec: jpeg
    sink: incoming_video
    width: 320
    height: 240
    framerate: 2
```

The rest of the native VoIP microphone/speaker, network and display configuration
is still required. [examples/voip.yaml](examples/voip.yaml) supplies a complete
microphone-only outgoing-audio/display example once Wi-Fi secrets and board pins
are provided. Add a supported speaker and camera through VoIP Stack's own
configuration when needed.

`video.sink` references the renderer ID directly. VoIP Stack creates its own
`S3RendererVideoSink` adapter and retains SIP/SDP negotiation, RTP reassembly and
call lifecycle. The renderer receives a protocol-neutral complete frame. An
accepted leased frame is copied and released by the renderer; a rejected frame
remains the caller's responsibility. Standalone builds have no VoIP dependency.

ESP32-P4 receive rendering lives in `p4_video_renderer` in this same repository.
The S3 backend publishes an LVGL image. P4 uses a compatible MIPI display-owned
pixel transaction without a video image. See the
[ownership migration guide](docs/MIGRATION_RENDERER_OWNERSHIP.md) for custom
configurations and the P4 component's resource and presentation contract.

On P4, `display_id` selects a display advertising the public owned-transform
capability. LVGL supplies page visibility and the logical container rectangle;
the display owns RGB/YUV conversion, scale, rotation, framebuffer writes and
serialization with UI drawing. JPEG retains its hardware-decoded RGB565 buffer;
H.264 retains its existing decoder, packed-YUV conversion and IDR recovery.
There is no second P4 `lv_image` presentation path. Renderer IDs and
`attach_video_container` are unchanged; the redundant `display_rotation` field
is accepted with a deprecation warning because rotation belongs to the display.

The current coordinated P4 provider is a minimal adaptation of official ESPHome
MIPI commit `c6f330458a2da2f64788487486bd6d0f8749fafd`, whose base matches
the recorded ESPHome baseline. Profiles follow ESPHome's recommended SDK;
exact supported SDK tags receive checksummed official build-local fixes
rather than a framework pin.
This source contract does not by itself qualify a physical panel or codec rate.

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
