"""Compile the production presentation TU with observable LVGL/display boundaries."""

from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[1]


@pytest.mark.parametrize("codec", ["JPEG", "H264"])
def test_exposed_pixels_restore_before_committing_the_pending_frame(tmp_path, codec):
    component = ROOT / "esphome/components/p4_video_renderer"
    fixtures = ROOT / "tests/fixtures"
    source = (component / "p4_presentation.cpp").read_text()
    assert source.count('#include "p4_video_renderer.h"') == 1
    source = source.replace('#include "p4_video_renderer.h"',
                            '#include "p4_presentation_test_boundary.h"')
    production = tmp_path / "p4_presentation.cpp"
    production.write_text(source)
    helpers = tmp_path / "esphome/core/helpers.h"
    helpers.parent.mkdir(parents=True)
    helpers.write_text("#pragma once\n")
    binary = tmp_path / "presentation_contract"
    subprocess.run([
        "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DUSE_ESP_IDF",
        f"-DUSE_P4_VIDEO_RENDERER_{codec}", "-I", str(fixtures), "-I", str(tmp_path),
        "-I", str(component), str(production), str(fixtures / "p4_presentation_contract.cpp"),
        "-o", str(binary),
    ], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True)
