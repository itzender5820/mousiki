#!/usr/bin/env python3
"""Writes the software bill of materials (SBOM) of a Mousiki release: CycloneDX 1.5, JSON.

  python tools/make_sbom.py --out packaging/dist/Mousiki-5.0.0-sbom.cdx.json [--version 5.0.0]

Lists Mousiki itself, the libraries shipped as source (third_party/VENDORED.json) and the bundled tools (packaging/pins.json:
yt-dlp, FFmpeg, Python, SDL2 ... with their exact download address and SHA-256). Standard library only; no network."""
import argparse
import datetime
import json
import re
import sys
import uuid
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import vendored_versions  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def project_version():
    m = re.search(r"project\(mousiki\s+VERSION\s+([0-9][0-9.]*)", (ROOT / "CMakeLists.txt").read_text(encoding="utf-8"))
    return m.group(1) if m else "unknown"


def license_entry(expr):
    return [{"expression": expr}] if re.search(r"\b(OR|AND|WITH)\b", expr) else [{"license": {"id": expr}}]


def vendored_components():
    out = []
    for lib in vendored_versions.load():
        ver = lib["version"]
        comp = {
            "type": "library",
            "bom-ref": "vendored/" + lib["name"],
            "name": lib["name"],
            "version": ver,
            "description": lib.get("use", ""),
            "licenses": license_entry(lib.get("license", "NOASSERTION")),
            "externalReferences": [{"type": "vcs", "url": "https://github.com/" + lib["upstream"]}],
            "properties": [{"name": "mousiki:shipped-as", "value": "source in " + lib.get("path", "third_party/")}],
        }
        if ver != "unknown":
            comp["purl"] = "pkg:github/%s@%s" % (lib["upstream"], ver)
        out.append(comp)
    return out


def tool_version(name, url):
    """Best effort: the version number out of the pinned file name or address."""
    for text in (url, name):
        m = re.search(r"(\d{4}\.\d{2}\.\d{2}|\d+\.\d+(?:\.\d+)*)", text.replace("-", " "))
        if m:
            return m.group(1)
    return "unknown"


def tool_components():
    path = ROOT / "packaging" / "pins.json"
    if not path.is_file():
        return []
    files = json.loads(path.read_text(encoding="utf-8")).get("files", {})
    out = []
    for name, pin in sorted(files.items()):
        out.append({
            "type": "application",
            "bom-ref": "tool/" + name,
            "name": name,
            "version": tool_version(name, pin.get("url", "")),
            "hashes": [{"alg": "SHA-256", "content": pin["sha256"]}],
            "externalReferences": [{"type": "distribution", "url": pin["url"]}],
            "properties": [{"name": "mousiki:shipped-as", "value": "bundled program (packaging/pins.json)"}],
        })
    return out


def build(version):
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": "urn:uuid:" + str(uuid.uuid4()),
        "version": 1,
        "metadata": {
            "timestamp": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "tools": [{"name": "tools/make_sbom.py"}],
            "component": {
                "type": "application",
                "bom-ref": "mousiki-plus",
                "name": "Mousiki Plus",
                "version": version,
                "licenses": [{"license": {"id": "Apache-2.0"}}],
                "purl": "pkg:github/StSchwerdtfeger/mousiki-plus@v" + version,
                "externalReferences": [{"type": "vcs", "url": "https://github.com/StSchwerdtfeger/mousiki-plus"}],
            },
        },
        "components": vendored_components() + tool_components(),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--version", default=None)
    a = ap.parse_args()
    version = (a.version or project_version()).lstrip("v")
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    doc = build(version)
    out.write_text(json.dumps(doc, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print("wrote %s (%d components)" % (out, len(doc["components"])))
    return 0


if __name__ == "__main__":
    sys.exit(main())
