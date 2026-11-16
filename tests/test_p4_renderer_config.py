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
    cpp = (generated / "esphome/components/p4_video_renderer/p4_video_renderer.cpp").read_text()
    assert "frame_renderer->set_display(panel)" in main
    assert "draw_rgb565_transformed" in cpp
    assert "lv_image_create(" not in cpp
    assert "lv_image_set_src(" not in cpp
    assert not list(generated.rglob("sip_transport.cpp"))
