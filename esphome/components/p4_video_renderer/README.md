# P4 encoded video renderer

This component accepts complete JPEG or H.264 frames through the protocol-neutral
`video_renderer` API. `display_id` selects a compatible MIPI DSI display whose
public pixel-transform capability owns the framebuffer and composes rotation.
The final configuration validator rejects unsupported display providers. No
VoIP mode or additional renderer-enable option is required.

Keep the existing renderer ID and `attach_video_container` hook. LVGL owns page
visibility and the logical video rectangle; the renderer creates no LVGL image.
JPEG uses one persistent decoded RGB565 buffer. H.264 retains its decoder and
I420 conversion, handing one packed YUV buffer to the same display owner. The
main loop submits one blocking color/scale/rotation transaction after admission;
the decoder worker never writes the panel. Pending pixels remain leased until
that transaction returns. Stop and generation changes reject late frames.

Aspect-fit preserves the former 1/16 scale quantization and decoded RGB storage
budget: with 800x800 decode bounds, a 320x240 source in a 1280x720 rectangle
produces 920x690 logical pixels. The driver applies its rotation once.

`dump_state()` reports admitted, decoded, presented and completed-transaction
counts and existing optional timing counters. These establish software/driver
activity, not optical panel refresh or acoustic quality. Physical qualification
is required separately. The source and S3 APIs remain unchanged.
