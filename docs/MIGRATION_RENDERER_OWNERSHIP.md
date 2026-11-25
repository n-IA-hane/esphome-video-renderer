# Renderer ownership migration

The renderer repository now provides both `esp_video_renderer` (software JPEG)
and `p4_video_renderer` (P4 hardware JPEG or software H.264 with PPA). Their
component names, renderer IDs and `attach_video_container` hooks are unchanged.
The small `video_renderer` component contains only common frame types and codec
dependency setup; it creates no renderer instance or task.

Load `video_renderer` with the selected renderer from this repository. Load the
VoIP component separately when it is needed. The renderer has no `voip` or
`standalone` setting. A normal `voip_stack.video.sink` reference automatically
creates the protocol adapter in Native VoIP Stack.

Before, a software renderer could declare a transport-owned sink ID:

```yaml
esp_video_renderer:
  id: screen_video
  voip:
    id: old_video_sink
voip_stack:
  video:
    codec: jpeg
    sink: old_video_sink
```

After:

```yaml
esp_video_renderer:
  id: screen_video
voip_stack:
  video:
    codec: jpeg
    sink: screen_video
```

Remove the old nested `voip` block and replace references to its old sink ID with
the existing renderer ID. Widget references to `screen_video` do not change.
P4 already used its renderer ID as the sink, so its ID stays unchanged; move
its `external_components` provider from Native to this repository.

## Display presentation

The software S3 backend retains its public LVGL image and existing lifetime
contract. P4 uses its display's public owned pixel-transform capability. A normal
`display_id` selects that compatible MIPI DSI provider; final configuration
validation rejects a provider without the required capability. The renderer
contains no panel handle or framebuffer pointer and has no VoIP mode.

For P4, LVGL supplies the active page and logical video rectangle. The main loop
commits one driver-owned color/scale/rotation transaction. JPEG retains one
hardware-decoded RGB565 buffer; H.264 retains its decoder and I420 conversion and
hands one packed YUV buffer to the same owner. No video image, scaled RGB copy or
alternate presentation path exists. The source buffer stays pending until all
accepted DMA has completed. The decoder worker never writes the panel.

`display_rotation` remains accepted with a deprecation warning and is removed
from operational configuration. The driver composes its configured rotation
once. Existing renderer IDs and `attach_video_container` hooks stay unchanged.
A first decoded frame may open its page; that page's initial LVGL refresh must
finish before direct video commits. Stop revokes the generation and invalidates
the page from the main loop. Ordinary page/control drawing remains with LVGL.

When a new destination no longer covers the previously presented rectangle,
the renderer invalidates the old bounds on the active screen. LVGL restores
the application's background and widgets; `LV_EVENT_REFR_READY` then allows
the main loop to commit the pending frame. The decoded buffer stays leased
through that repaint and the display transaction. This handles aspect changes,
shrink and movement without painting a hardcoded background or clearing each
video frame. A stop or fast redial still passes through generation admission.

Geometry preserves the former 1/16 quantization and RGB output budget. With
800x800 decode bounds, 320x240 input in a 1280x720 rectangle produces 920x690
logical pixels, despite the absence of an intermediate output allocation.
H.264 reference-picture decoding, bounded producer leases and IDR recovery after
loss remain unchanged. Host tests establish ownership and geometry, not optical
panel quality or measured acoustic behavior.

## Encoded-frame ownership

`EncodedFrameRenderer` exposes capabilities, `prepare`, `start`, `set_active`,
`stop` and `submit_frame`. It contains codec/dimension limits, not SDP payload
numbers or RTP state. Frame timestamps are microseconds with explicit validity;
zero is a valid timestamp. RTP clock extension and SDP capability validation live
only in the Native adapter.

If `submit_frame` returns false, the caller still owns the frame and its release
callback is not invoked. True transfers a supplied lease, which the renderer
releases exactly once, including on replacement or stop. Without a lease, data
is borrowed only during `submit_frame` and must be copied before asynchronous
use. H.264 requires leases; JPEG copies admitted input synchronously.
