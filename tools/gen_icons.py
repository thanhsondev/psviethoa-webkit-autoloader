#!/usr/bin/env python3
"""Generate all derived icon assets from assets/icon.svg at build time.

The master SVG is the PSVietHoa brand mark (raster art) with no background, so
every generated asset gets a dark background and ~6% padding added.

Outputs:
  assets/icon0.png                        PS5 homescreen icon (512x512)
  assets/icon.ico                         Windows .exe icon (16-256px)
  frontend/installer-page/favicon.svg     installer page favicon (padded + bg)
  frontend/autoloader/favicon.svg         autoloader page favicon (padded + bg)
  frontend/installer-page/logo.svg        raw master art, for in-page use
  frontend/autoloader/logo.svg            raw master art, for in-page use

Rendering: rsvg-convert when available (installed in the psviethoa-webkit-autoloader-sdk
docker image), qlmanage as the built-in macOS fallback. Run via `make icons`.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
MASTER = os.path.join(ROOT, "assets", "icon.svg")
ICON0 = os.path.join(ROOT, "assets", "icon0.png")
ICON_ICO = os.path.join(ROOT, "assets", "icon.ico")
FAVICON_INSTALLER = os.path.join(ROOT, "frontend", "installer-page", "favicon.svg")
FAVICON_AUTOLOADER = os.path.join(ROOT, "frontend", "autoloader", "favicon.svg")
LOGO_INSTALLER = os.path.join(ROOT, "frontend", "installer-page", "logo.svg")
LOGO_AUTOLOADER = os.path.join(ROOT, "frontend", "autoloader", "logo.svg")

VIEWBOX = 1024
ART_RADIUS = 512.0  # outermost extent of the master art (full-width raster, 1024/2)
PAD_FRACTION = 0.06
SCALE = (1.0 - 2.0 * PAD_FRACTION) * (VIEWBOX / 2.0) / ART_RADIUS
TRANSLATE = VIEWBOX * (1.0 - SCALE) / 2.0
ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]

BG_GRADIENT = """    <radialGradient id="wkalBg" cx="50%" cy="50%" r="75%">
      <stop offset="0%" stop-color="#0e182b"/>
      <stop offset="100%" stop-color="#060a13"/>
    </radialGradient>"""


def build_wrapper_svg(master_src):
    """Master SVG re-wrapped with a dark background and ~10% padding."""
    defs = re.search(r"<defs>(.*?)</defs>", master_src, re.S)
    art = re.search(r"</defs>(.*?)</svg>", master_src, re.S)
    if not defs or not art:
        sys.exit("Error: could not parse master icon.svg (defs/art not found).")
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<svg xmlns="http://www.w3.org/2000/svg" '
        'xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 {vb} {vb}">\n'
        "  <defs>\n"
        "{bg}\n"
        "{defs}\n"
        "  </defs>\n"
        '  <rect width="{vb}" height="{vb}" fill="url(#wkalBg)"/>\n'
        '  <g transform="translate({t} {t}) scale({s})">\n'
        "{art}\n"
        "  </g>\n"
        "</svg>\n"
    ).format(
        vb=VIEWBOX,
        bg=BG_GRADIENT,
        defs=defs.group(1).strip(),
        art=art.group(1).strip(),
        t=round(TRANSLATE, 3),
        s=round(SCALE, 6),
    )


def rsvg_render(svg_path, size):
    """Render the SVG to PNG bytes at the given size via rsvg-convert."""
    return subprocess.run(
        ["rsvg-convert", "-w", str(size), "-h", str(size), svg_path],
        check=True,
        capture_output=True,
    ).stdout


def ql_render(svg_path, size):
    """Render via QuickLook; use a unique temp filename to dodge its cache."""
    tmpdir = tempfile.mkdtemp(prefix="wkal-icon-")
    tmp_svg = os.path.join(tmpdir, os.urandom(4).hex() + ".svg")
    shutil.copyfile(svg_path, tmp_svg)
    subprocess.run(["qlmanage", "-t", "-s", str(size), "-o", tmpdir, tmp_svg],
                   check=True, capture_output=True)
    rendered = os.path.join(tmpdir, os.path.basename(tmp_svg) + ".png")
    with open(rendered, "rb") as f:
        return f.read()


def find_renderer():
    if shutil.which("rsvg-convert"):
        return rsvg_render
    if sys.platform == "darwin" and shutil.which("qlmanage"):
        return ql_render
    sys.exit(
        "Error: no SVG renderer found.\n"
        "  Run the build inside the psviethoa-webkit-autoloader-sdk docker image "
        "(has rsvg-convert),\n"
        "  or use macOS where qlmanage is available."
    )


def build_ico(pngs):
    """Assemble PNG blobs into a Windows .ico (Vista+ PNG-in-ICO format)."""
    header = struct.pack("<HHH", 0, 1, len(pngs))
    entries = b""
    offset = 6 + 16 * len(pngs)
    for size, data in pngs:
        w = 0 if size >= 256 else size
        entries += struct.pack("<BBBBHHII", w, w, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    return header + entries + b"".join(data for _, data in pngs)


def main():
    with open(MASTER, "r") as f:
        master_src = f.read()

    wrapper = build_wrapper_svg(master_src)
    render = find_renderer()

    with tempfile.TemporaryDirectory(prefix="wkal-icon-") as tmp:
        wrapper_path = os.path.join(tmp, "icon-bg.svg")
        with open(wrapper_path, "w") as f:
            f.write(wrapper)

        # PS5 homescreen icon (512x512) and Windows .exe icon (16-256px)
        with open(ICON0, "wb") as f:
            f.write(render(wrapper_path, 512))
        pngs = [(size, render(wrapper_path, size)) for size in ICO_SIZES]
        with open(ICON_ICO, "wb") as f:
            f.write(build_ico(pngs))

        # Favicon SVGs (same wrapper, no rasterization needed)
        for path in (FAVICON_INSTALLER, FAVICON_AUTOLOADER):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(wrapper)

        # In-page logo SVGs (raw master art, no wrapper background)
        for path in (LOGO_INSTALLER, LOGO_AUTOLOADER):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(master_src)

    print("Generated icon assets from assets/icon.svg:")
    for path in (ICON0, ICON_ICO, FAVICON_INSTALLER, FAVICON_AUTOLOADER,
                 LOGO_INSTALLER, LOGO_AUTOLOADER):
        print(f"  {os.path.relpath(path, ROOT)} ({os.path.getsize(path)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
