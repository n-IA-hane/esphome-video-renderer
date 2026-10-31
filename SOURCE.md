# Source and extraction scope

The component was extracted mechanically from
[`n-IA-hane/esphome-voip-stack`](https://github.com/n-IA-hane/esphome-voip-stack)
at commit `38b982cdd7102e094a228279b65600885a408b22`.
The original path was `esphome/components/esp_video_renderer/`.

All five Python/C++ component files are unchanged at extraction. The initial
repository adds independent documentation, examples, requirements, version
metadata, host tests and CI around that implementation. Focused tests and the
pixel qualification tool came from the same source; test dependency discovery
was adapted to the repository boundary.

The original MIT notice is retained in `LICENSE`. The source history attributes
the component commits to `n-IA-hane`; no new third-party authorship is claimed.
`esp_jpeg`, ESPHome, LVGL and ESP-IDF are external dependencies downloaded by
the builder and retain their own licensing.

SIP/SDP/RTP, camera sources, hardware P4 rendering and call state remain outside
this repository. Only `voip_adapter.h`, compiled by explicit configuration,
depends on the existing VoIP Stack interface.

## Initial component SHA-256

| File | SHA-256 |
| --- | --- |
| `__init__.py` | `c2cb8d55180c7da2fed55a7726a582fad7c02577cf7413d596515a1f76004e01` |
| `esp_video_renderer.cpp` | `49af65b1124f6eace61290b6aa1ac8be2b5ac4b4a8758f0e8035b471b6d6a83b` |
| `esp_video_renderer.h` | `97b5dfc93a08a5cea279b4fe046dc19cc637680f01218fac46128bdfb70903a4` |
| `jpeg_frame.h` | `ddf2726f52f272c2e85f2a9b216230a144729352bd1e246ffb555a0904d03e97` |
| `voip_adapter.h` | `95375d66dd831f34e1c00c02f67121630ed3ec2662140ff6b9cc81ff5f3f75cd` |
