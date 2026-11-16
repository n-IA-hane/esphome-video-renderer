#!/usr/bin/env python3
"""Bind renderer CI to selected real sources, reusing Intercom's manifest validator."""
from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess

RENDERER = Path(__file__).resolve().parents[1]
SHA = re.compile(r"[0-9a-f]{40}")


def read_manifest(raw: str) -> dict | None:
    if not raw.strip():
        return None
    manifest = json.loads(raw)
    if not isinstance(manifest, dict) or not SHA.fullmatch(str(manifest.get("backend_sha", ""))):
        raise ValueError("The coordinated manifest must select an immutable Intercom backend_sha")
    return manifest


def backend_ref(manifest: dict | None, channel: str) -> str:
    """Select only the bootstrap checkout; full validation stays with its owner."""
    return manifest["backend_sha"] if manifest is not None else channel


def git(path: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


def resolve(backend: Path, renderer: Path, manifest: dict | None, channel: str,
            voip: Path | None = None) -> dict:
    spec = importlib.util.spec_from_file_location(
        "_intercom_coordinated_sources", backend / "scripts/coordinated_sources.py"
    )
    if spec is None or spec.loader is None:
        raise ValueError("The selected hardware provider lacks its coordinated source validator")
    authority = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(authority)
    # Do not maintain a second repository/manifest validator in this project.
    refs = authority.requested_refs(manifest, channel)
    repositories = authority.requested_repositories(manifest, channel)
    actual_backend = git(backend, "rev-parse", "HEAD")
    actual_renderer = git(renderer, "rev-parse", "HEAD")
    if manifest is not None:
        if manifest["backend_sha"] != actual_backend:
            raise ValueError("Intercom checkout differs from the coordinated manifest")
        if manifest["components"]["esphome-video-renderer"]["sha"] != actual_renderer:
            raise ValueError("Renderer checkout differs from the coordinated manifest")
    components = backend / "esphome/components"
    if not all((components / "mipi_dsi" / name).is_file()
               for name in ("display.py", "mipi_dsi.h", "mipi_dsi.cpp")):
        raise ValueError("The selected Intercom source has no complete MIPI provider")
    sources = {
        "version": 1, "coordinated_manifest_provided": manifest is not None,
        "backend": {"repository": "n-IA-hane/esphome-intercom", "sha": actual_backend},
        "renderer": {"repository": "n-IA-hane/esphome-video-renderer", "sha": actual_renderer},
        "voip": {"repository": repositories["esphome-voip-stack"],
                 "requested_ref": refs["esphome-voip-stack"]},
        "mipi_components_source": str(components.resolve()),
    }
    for name, path in (("backend", backend), ("renderer", renderer)):
        dirty = bool(git(path, "status", "--porcelain", "--untracked-files=no"))
        sources[name]["tracked_changes"] = dirty
        if os.environ.get("GITHUB_ACTIONS") == "true" and dirty:
            raise ValueError(f"CI must use a clean immutable {name} checkout")
    if voip is not None:
        actual = git(voip, "rev-parse", "HEAD")
        if SHA.fullmatch(refs["esphome-voip-stack"]) and actual != refs["esphome-voip-stack"]:
            raise ValueError("Native checkout differs from the coordinated manifest")
        sources["voip"]["sha"] = actual
    return sources


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel", choices=("dev", "main"), default="dev")
    parser.add_argument("--backend-source", type=Path)
    parser.add_argument("--voip-source", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--github-output", type=Path)
    args = parser.parse_args()
    manifest = read_manifest(os.environ.get("COMPONENT_MANIFEST", ""))
    outputs = {"backend_ref": backend_ref(manifest, args.channel)}
    data = {"bootstrap_backend_ref": outputs["backend_ref"]}
    if args.backend_source is not None:
        data = resolve(args.backend_source.resolve(), RENDERER, manifest,
                       args.channel, args.voip_source.resolve() if args.voip_source else None)
        outputs["voip_ref"] = data["voip"]["requested_ref"]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, indent=2) + "\n")
    if args.github_output is not None:
        with args.github_output.open("a") as stream:
            for name, value in outputs.items():
                stream.write(f"{name}={value}\n")


if __name__ == "__main__":
    main()
