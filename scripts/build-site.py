#!/usr/bin/env python3
"""Build the static landing page without dependencies or network requests."""
import argparse
import html
import json
import shutil
import re
from pathlib import Path
from urllib.parse import urlsplit

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--url", default="https://bbatarelo.github.io/emu-apple-silicon")
args = parser.parse_args()
url = args.url.rstrip("/")
parts = urlsplit(url)
if parts.scheme != "https" or not parts.netloc or parts.query or parts.fragment:
    parser.error("--url must be an absolute HTTPS URL without query or fragment")
version = (ROOT / "VERSION").read_text().strip()
if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?", version):
    raise SystemExit("VERSION must contain a semantic version")
output = ROOT / "build/site"
output.mkdir(parents=True, exist_ok=True)
page = (ROOT / "site/index.html").read_text()
for token, value in {"@VERSION@": version, "@SITE_URL@": url}.items():
    if token not in page:
        raise SystemExit(f"Missing template token: {token}")
    page = page.replace(token, html.escape(value, quote=True))
metadata = {
    "@context": "https://schema.org",
    "@type": "SoftwareSourceCode",
    "name": "E-MU Apple Silicon driver",
    "description": "Independent macOS audio drivers for E-MU Tracker Pre and 0404 USB, with 0404 MIDI support.",
    "url": url + "/",
    "codeRepository": "https://github.com/bbatarelo/emu-apple-silicon",
    "version": version,
    "runtimePlatform": "Apple Silicon macOS",
    "programmingLanguage": ["C", "Rust"],
    "license": "https://github.com/bbatarelo/emu-apple-silicon/blob/main/LICENSE"
}
page = page.replace("@STRUCTURED_DATA@", json.dumps(metadata).replace("<", "\\u003c"))
if re.search(r"@[A-Z_]+@", page):
    raise SystemExit("Unresolved template token")
(output / "index.html").write_text(page)
(output / "style.css").write_bytes((ROOT / "site/style.css").read_bytes())
shutil.copytree(ROOT / "site/assets", output / "assets", dirs_exist_ok=True)
(output / ".nojekyll").write_text("")
(output / "sitemap.xml").write_text(
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">'
    f'<url><loc>{html.escape(url)}/</loc></url></urlset>\n')
print(f"Built {output} — version {version}, canonical URL {url}/")
