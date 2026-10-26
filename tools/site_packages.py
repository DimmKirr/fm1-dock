#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The packages the page installs (docs/install/fm1install.js): each repository's latest release, its .fwsc
copied into the site, with fw/index.json. A page cannot download a release asset from GitHub itself (no CORS),
so the site keeps a copy. Each copy is checked against the SHA-256 GitHub gives for the release asset, and as
tools/fm1_install.py checks a package before it installs one (identity, update loader, every file's CRC).

  tools/site_packages.py OUT_DIR ID=OWNER/REPO ...      (GH_TOKEN, or gh logged in)
  e.g. tools/site_packages.py _site/fw fm1-dock=joakimeriksson/fm1-dock fm1-sid=joakimeriksson/fm1-sid
"""
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fm1_install  # noqa: E402


def gh_json(path):
    return json.loads(subprocess.run(["gh", "api", path], check=True, capture_output=True, text=True).stdout)


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    out = Path(argv[0])
    out.mkdir(parents=True, exist_ok=True)
    index = {"updated": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "firmwares": {}}
    for arg in argv[1:]:
        fid, _, repo = arg.partition("=")
        rel = gh_json(f"repos/{repo}/releases/latest")
        assets = [a for a in rel["assets"] if a["name"].endswith(".fwsc")]
        if len(assets) != 1:
            raise SystemExit(f"{repo} {rel['tag_name']}: {len(assets)} .fwsc assets, expected one")
        a = assets[0]
        subprocess.run(["gh", "release", "download", rel["tag_name"], "-R", repo, "-p", a["name"], "-D", str(out),
                        "--clobber"], check=True)
        path = out / a["name"]
        raw = path.read_bytes()
        sha = hashlib.sha256(raw).hexdigest()
        if a.get("digest") and a["digest"] != "sha256:" + sha:
            raise SystemExit(f"{repo} {a['name']}: SHA-256 {sha}, GitHub says {a['digest']}")
        try:
            product, _ = fm1_install.load_package(str(path), False)
        except fm1_install.InstallError as e:
            raise SystemExit(f"{repo} {a['name']}: {e}")
        index["firmwares"][fid] = {"name": rel["name"] or a["name"], "version": rel["tag_name"].removeprefix("v"),
                                   "tag": rel["tag_name"], "file": a["name"], "sha256": sha, "size": len(raw),
                                   "product": product, "release": rel["html_url"]}
        print(f"{fid:10} {a['name']:28} {product:14} {sha[:16]}")
    (out / "index.json").write_text(json.dumps(index, indent=1) + "\n")


if __name__ == "__main__":
    main(sys.argv[1:])
