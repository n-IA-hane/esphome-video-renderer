"""Metadata replay tests; no fake refs are fetched or treated as runtime proof."""
from pathlib import Path
import importlib.util
import json
import os
import subprocess

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("renderer_ci_sources", ROOT / "tools/ci_test_sources.py")
sources = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sources)
MIPI = Path(os.environ.get("MIPI_COMPONENTS_SOURCE", ROOT.parent / "esphome-intercom/esphome/components"))
BACKEND = MIPI.resolve().parents[1]


def manifest():
    """Syntax-only source choices for validator tests, never network fetches."""
    catalog = json.loads((BACKEND / "qualification/sources.json").read_text())["repositories"]
    components = {name: {"repository": value["repository"], "sha": "a" * 40}
                  for name, value in catalog.items() if name != "esphome-intercom"}
    components["esphome-video-renderer"]["sha"] = sources.git(ROOT, "rev-parse", "HEAD")
    return {"version": 1, "backend_sha": sources.git(BACKEND, "rev-parse", "HEAD"), "components": components}


def test_bootstrap_selects_an_immutable_recorded_backend():
    recorded = manifest()
    assert sources.backend_ref(sources.read_manifest(json.dumps(recorded)), "dev") == recorded["backend_sha"]
    assert sources.backend_ref(None, "main") == "main"
    with pytest.raises(ValueError, match="immutable"):
        sources.read_manifest('{"backend_sha":"unpublished-ref"}')


def test_replay_reuses_the_complete_authoritative_validator(monkeypatch):
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    recorded = manifest()
    selected = sources.resolve(BACKEND, ROOT, recorded, "dev")
    assert selected["coordinated_manifest_provided"]
    assert selected["voip"]["requested_ref"] == "a" * 40
    assert Path(selected["mipi_components_source"]) == MIPI.resolve()
    del recorded["components"]["esphome-audio-stack"]
    with pytest.raises(ValueError, match="complete coordinated component set"):
        sources.resolve(BACKEND, ROOT, recorded, "dev")


def test_replay_rejects_a_different_renderer_before_any_fetch(monkeypatch):
    monkeypatch.delenv("GITHUB_ACTIONS", raising=False)
    recorded = manifest()
    recorded["components"]["esphome-video-renderer"]["sha"] = "f" * 40
    with pytest.raises(ValueError, match="Renderer checkout differs"):
        sources.resolve(BACKEND, ROOT, recorded, "dev")


def test_ci_checkout_is_source_only_and_declares_the_local_binding():
    workflow = yaml.safe_load((ROOT / ".github/workflows/ci.yml").read_text())
    host = workflow["jobs"]["host"]
    hardware = next(s for s in host["steps"] if s.get("with", {}).get("repository") == "n-IA-hane/esphome-intercom")
    assert hardware["with"]["ref"] == "${{ steps.bootstrap.outputs.backend_ref }}"
    sparse = hardware["with"]["sparse-checkout"].splitlines()
    assert "/esphome/components/mipi_dsi/" in sparse
    assert "/esphome/components/p4_ppa_backport/" in sparse
    assert "/custom_components/" not in sparse and "/esphome/components/voip_stack/" not in sparse
    execution = next(s for s in host["steps"] if s.get("name") == "Test production renderer and optional adapter")
    assert execution["env"]["MIPI_COMPONENTS_SOURCE"].endswith("/.test-deps/esphome-intercom/esphome/components")
    example = yaml.safe_load((ROOT / "examples/p4-frame-input.yaml").read_text())
    provider = next(s for s in example["external_components"] if s["components"] == ["mipi_dsi", "p4_ppa_backport"])
    assert provider["source"] == "${mipi_components_source}"
    assert not any("voip_stack" in s["components"] for s in example["external_components"])
    assert "voip_stack" not in example and "network" not in example


def test_bootstrap_cli_only_writes_metadata(tmp_path):
    output = tmp_path / "selected.json"
    env = dict(os.environ, COMPONENT_MANIFEST="")
    result = subprocess.run([os.sys.executable, str(ROOT / "tools/ci_test_sources.py"),
                             "--channel", "dev", "--output", str(output)],
                            env=env, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert json.loads(output.read_text()) == {"bootstrap_backend_ref": "dev"}


def test_p4_compile_job_uses_the_selected_provider_and_rejects_transport_components():
    workflow = yaml.safe_load((ROOT / ".github/workflows/ci.yml").read_text())
    job = workflow["jobs"]["p4-firmware"]
    hardware = next(step for step in job["steps"]
                    if step.get("with", {}).get("repository") == "n-IA-hane/esphome-intercom")
    assert hardware["with"]["ref"] == "${{ steps.bootstrap.outputs.backend_ref }}"
    assert "/esphome/components/mipi_dsi/" in hardware["with"]["sparse-checkout"]
    assert "/custom_components/" not in hardware["with"]["sparse-checkout"]
    compile_step = next(step for step in job["steps"]
                        if step.get("name") == "Compile and link the P4 encoded-frame producer without VoIP")
    script = compile_step["run"]
    assert 'compile examples/p4-frame-input.yaml' in script
    assert '--only-generate' not in script
    assert '-s mipi_components_source "$MIPI_COMPONENTS_SOURCE"' in script
    assert 'actual == (expected / "mipi_dsi" / name).read_bytes()' in script
    assert 'not selected & forbidden' in script
    assert all(f'"{component}"' in script for component in
               ("voip_stack", "api", "wifi", "network", "esp_video_camera"))
    assert not any(step.get("with", {}).get("repository") == "n-IA-hane/esphome-voip-stack"
                   for step in job["steps"])


def test_p4_example_follows_core_sdk_and_acceleration_defaults():
    example = yaml.safe_load((ROOT / "examples/p4-frame-input.yaml").read_text())
    framework = example["esp32"]["framework"]
    assert framework["type"] == "esp-idf"
    assert "version" not in framework and "source" not in framework
    assert "sdkconfig_options" not in framework
    assert not example["esphome"].get("build_flags")
    assert "p4_ppa_backport" in example
