"""Protocol-neutral encoded-frame renderer interfaces and codec dependencies."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import git
from esphome.components import esp32
from esphome.core import CORE

CODEOWNERS = ["@n-IA-hane"]
CONFIG_SCHEMA = cv.Schema({})
ns = cg.esphome_ns.namespace("video_renderer")
EncodedFrameRenderer = ns.class_("EncodedFrameRenderer")


def add_h264_component():
    """One existing pinned codec provider, shared by producers and renderers."""
    repository = CORE.data.get("video_renderer_h264_repository")
    if repository is None:
        repository, _ = git.clone_or_update(
            url="https://github.com/n-IA-hane/esp-h264-component.git",
            ref="176cda67716c24deef0e76e4451048981e154bd4",
            refresh=None, domain="idf_components",
        )
        CORE.data["video_renderer_h264_repository"] = repository
    esp32.add_idf_component(name="espressif/esp_h264", path=str(repository / "esp_h264"))
