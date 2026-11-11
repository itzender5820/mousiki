#!/usr/bin/env python3
"""Converts the PNG pictures of the README and of the manual to JPEG (smaller files) and fixes their references in README.md and in
Mousiki_User_Manual/Mousiki_User_Guide.md.

    python3 tools/png_to_jpg.py                    # convert, delete the PNGs, fix the references (same size in pixels, quality 80)
    python3 tools/png_to_jpg.py --dry-run          # only say what would happen (sizes before / after)
    python3 tools/png_to_jpg.py --max-width 1400 --quality 75
    python3 tools/png_to_jpg.py --keep             # keep the PNGs next to the new JPEGs

Run it again whenever new PNG screenshots are added to readme_images/ or Mousiki_User_Manual/images*/: a picture that is already a JPEG is
left alone, a PNG is converted, its references are changed to .jpg and the PNG is removed.

Notes
  * The pictures are never scaled or cropped unless --max-width is given: the JPEG has the pixels of the PNG. A terminal screenshot is flat
    colour and text, which PNG stores very well, so the saving is small (about 5-10 % at quality 80); --max-width 1600 saves a third more.
  * A PNG with real transparency stays a PNG (a JPEG cannot be transparent); the script says so.
  * Needs Pillow:  python3 -m pip install pillow
"""
import argparse
import re
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow is needed:  python3 -m pip install pillow")

ROOT = Path(__file__).resolve().parent.parent
FOLDERS = [ROOT / "readme_images", ROOT / "Mousiki_User_Manual" / "images", ROOT / "Mousiki_User_Manual" / "images_radio", ROOT / "Mousiki_User_Manual" / "images_audio_book"]
DOCS = [ROOT / "README.md", ROOT / "Mousiki_User_Manual" / "Mousiki_User_Guide.md"]


def has_alpha(im):
    if im.mode in ("RGBA", "LA") or (im.mode == "P" and "transparency" in im.info):
        return im.convert("RGBA").getchannel("A").getextrema()[0] < 255
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--max-width", type=int, default=0, help="scale pictures wider than this down (default 0 = never: the pixels are kept as they are)")
    ap.add_argument("--quality", type=int, default=80, help="JPEG quality 1..95 (default 80)")
    ap.add_argument("--keep", action="store_true", help="keep the PNG files")
    ap.add_argument("--dry-run", action="store_true", help="change nothing, only report")
    a = ap.parse_args()

    done = {}   # old file name -> new file name (same folder)
    before = after = 0
    kept_png = []
    for folder in FOLDERS:
        if not folder.is_dir():
            continue
        for png in sorted(folder.glob("*.png")):
            im = Image.open(png)
            if has_alpha(im):
                kept_png.append(png.name)
                continue
            rgb = im.convert("RGB")
            if a.max_width and rgb.width > a.max_width:
                rgb = rgb.resize((a.max_width, max(1, round(rgb.height * a.max_width / rgb.width))), Image.LANCZOS)
            jpg = png.with_suffix(".jpg")
            before += png.stat().st_size
            if a.dry_run:
                import io
                b = io.BytesIO()
                rgb.save(b, "JPEG", quality=a.quality, optimize=True, progressive=True, subsampling=2)
                after += len(b.getvalue())
            else:
                rgb.save(jpg, "JPEG", quality=a.quality, optimize=True, progressive=True, subsampling=2)
                after += jpg.stat().st_size
                if not a.keep:
                    png.unlink()
            done[png.name] = jpg.name
    print(f"{len(done)} pictures: {before / 1e6:.1f} MB -> {after / 1e6:.1f} MB")
    if kept_png:
        print("left as PNG (transparent):", ", ".join(kept_png))
    if a.dry_run:
        return
    # the references: ![..](images/x.png), src="images/x.png", readme_images/x.png ... (only the pictures that were converted)
    names = sorted(done, key=len, reverse=True)
    for doc in DOCS:
        if not doc.is_file():
            continue
        t = doc.read_text(encoding="utf-8")
        n = 0
        for old in names:
            new = done[old]
            t2, k = re.subn(r"(?<![A-Za-z0-9_.-])((?:readme_images|images(?:_radio|_audio_book)?)/)" + re.escape(old) + r"(?![A-Za-z0-9_])", lambda m: m.group(1) + new, t)
            t, n = t2, n + k
        doc.write_text(t, encoding="utf-8")
        print(f"{doc.name}: {n} references changed")


if __name__ == "__main__":
    main()
