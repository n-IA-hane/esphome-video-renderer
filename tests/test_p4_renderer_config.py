"""Exercise LVGL dependencies of the protocol-independent P4 image renderer."""

import os
from pathlib import Path
import subprocess
import sys

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("codec", ["jpeg", "h264"])
@pytest.mark.parametrize("capability", ["owned", "stock"])
def test_p4_encoded_pixels_use_the_display_owned_capability(tmp_path, codec, capability):
    config = (ROOT / "examples/p4-frame-input.yaml").read_text()
    config = config.replace("frame_input_build_path: .esphome/build/p4-frame-input",
                            f"frame_input_build_path: {tmp_path / 'build'}")
    config = config.replace("path: ../esphome/components",
                            f"path: {ROOT / 'esphome/components'}")
    config = config.replace("    - sample-frame.h",
                            f"    - {ROOT / 'examples/sample-frame.h'}")
    config = config.replace("codec: jpeg", f"codec: {codec}")
    if codec == "h264":
        config = config.replace("  height: 120", "  height: 128")
    mipi = Path(os.environ.get("MIPI_COMPONENTS_SOURCE", ROOT.parent / "esphome-intercom/esphome/components"))
    config = config.replace("mipi_components_source: github://n-IA-hane/esphome-intercom@dev",
                            f"mipi_components_source: {mipi}")
    if capability == "stock":
        config = config.replace("  - source: ${mipi_components_source}\n    components: [mipi_dsi, p4_ppa_backport]\n", "")
        config = config.replace("p4_ppa_backport:\n", "")
    path = tmp_path / "p4.yaml"
    path.write_text(config)
    result = subprocess.run(
        [os.environ.get("ESPHOME_PYTHON", sys.executable), "-m", "esphome",
         "compile", str(path), "--only-generate"],
        capture_output=True, text=True, timeout=90,
    )
    if capability == "stock":
        assert result.returncode != 0
        assert "display-owned RGB565 pixel transforms" in result.stdout + result.stderr
        assert not (tmp_path / "build/src/main.cpp").exists()
        return
    assert result.returncode == 0, result.stdout + result.stderr
    generated = tmp_path / "build/src"
    main = (generated / "main.cpp").read_text()
    component = generated / "esphome/components/p4_video_renderer"
    presentation = (component / "p4_presentation.cpp").read_text()
    cpp = "\n".join(path.read_text() for path in component.glob("*.cpp"))
    assert "frame_renderer->set_display(panel)" in main
    assert "draw_rgb565_transformed" in presentation
    assert "lv_image_create(" not in cpp
    assert "lv_image_set_src(" not in cpp
    assert not list(generated.rglob("sip_transport.cpp"))


def test_p4_scale_fits_bounds_and_rgb565_byte_budget(tmp_path):
    source = tmp_path / "scale.cpp"
    source.write_text(r"""
#include "p4_presentation_geometry.h"
#include <cassert>

int main() {
  using esphome::p4_video_renderer::fit_scale_units;
  struct Size { int w, h; };
  constexpr Size frames[] = {{320, 240}, {160, 120}, {640, 360}, {319, 239}, {800, 800}};
  constexpr Size views[] = {{1280, 800}, {800, 1280}, {319, 239}, {80, 50}, {1, 1}};
  constexpr size_t budgets[] = {0, 1, 4096, 639 * 479 * 2, 640 * 480 * 2, 800 * 800 * 2};
  for (auto frame : frames) {
    for (auto view : views) {
      for (auto bytes : budgets) {
        // Independently enumerate the hardware's discrete scale settings.
        int best = 0;
        for (int step = 1; frame.w * step <= view.w * 16 && frame.h * step <= view.h * 16; ++step) {
          const size_t pixels = size_t(frame.w * step / 16) * (frame.h * step / 16);
          if (pixels > 0 && pixels * 2 <= bytes)
            best = step;
        }
        assert(fit_scale_units(frame.w, frame.h, view.w, view.h, bytes) == best);
      }
    }
  }
  assert(fit_scale_units(0, 120, 800, 800, 4096) == 0);
  assert(fit_scale_units(160, 120, 0, 800, 4096) == 0);
}
""")
    binary = tmp_path / "scale"
    subprocess.run(["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-I",
                    str(ROOT / "esphome/components/p4_video_renderer"), str(source),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
