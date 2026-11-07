#!/usr/bin/env python3
"""Builds Mousiki_User_Manual/Mousiki_User_Guide.pdf from Mousiki_User_Guide.md with Chromium (through Playwright).

    python3 tools/make_manual_pdf.py                      # preset "compact"
    python3 tools/make_manual_pdf.py --preset dense       # fewer pages still
    python3 tools/make_manual_pdf.py --font-size 12 --table-font-size 10.5 --margin 9
    python3 tools/make_manual_pdf.py --compare            # page count of every preset, nothing is overwritten

The page count is what the numbers below decide; try a preset, look at the PDF, then adjust single values.
Needs:  python3 -m pip install markdown playwright   and   python3 -m playwright install chromium   (or pass --chrome <path to chrome / chromium>).
On a system whose pip is "externally managed" use a virtual environment (the script prints the commands).

Presets (all sizes in CSS pixels, margin in millimetres):
    normal   16 px text, tables 15 px, code 13 px, margin 15 mm     -- like a web page printed
    compact  13.5 px text, tables 12 px, code 11.5 px, margin 12 mm
    dense    12.5 px text, tables 11 px, code 10.5 px, margin 10 mm

The picture widths are set in the manual itself (<img width="...">, in CSS pixels; the widest a picture can be on the page
is the text width: A4 minus the margins). Internal links (#anchors) use the same names as GitHub makes from the headings.
"""
import argparse
import html as htmllib
import os
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANUAL = ROOT / "Mousiki_User_Manual" / "Mousiki_User_Guide.md"
DEFAULT_OUT = ROOT / "Mousiki_User_Manual" / "Mousiki_User_Guide.pdf"

PRESETS = {
    #            text  line  table code  margin heading-space paragraph-space cell-pad
    "normal":  dict(font=16.0, line=1.50, table=15.0, code=13.0, margin=15.0, hspace=1.0, pspace=0.9, pad=4),
    "compact": dict(font=13.5, line=1.38, table=12.0, code=11.5, margin=12.0, hspace=0.9, pspace=0.6, pad=2),
    "dense":   dict(font=12.5, line=1.32, table=11.0, code=10.5, margin=10.0, hspace=0.8, pspace=0.5, pad=1),
}


def github_slugger():
    """Heading -> anchor the way GitHub does it (lower case, punctuation dropped, spaces -> '-', repeats get -1, -2 ...)."""
    seen = {}

    def slug(value, separator="-"):
        s = re.sub(r"<[^>]+>", "", value)
        s = htmllib.unescape(s).strip().lower()
        s = re.sub(r"[^\w\- ]", "", s, flags=re.UNICODE).replace(" ", "-")
        n = seen.get(s, 0)
        seen[s] = n + 1
        return s if n == 0 else "%s-%d" % (s, n)
    return slug


def nested_lists_for_python_markdown(md_text):
    """GitHub nests a list item under the one above it when it is indented 2 spaces; Python-Markdown wants 4. Without this the
    contents list (and every nested list) comes out flat. Only list lines outside code blocks are touched."""
    out, fence = [], False
    for line in md_text.split("\n"):
        if line.startswith("```"):
            fence = not fence
        m = re.match(r"^( +)([-*+] |\d+\. )", line)
        if m and not fence:
            line = " " * (len(m.group(1)) * 2) + line[len(m.group(1)):]
        out.append(line)
    return "\n".join(out)


def build_html(md_text, base_dir, p):
    import markdown
    md_text = nested_lists_for_python_markdown(md_text)
    body = markdown.markdown(
        md_text, extensions=["tables", "fenced_code", "toc", "sane_lists"],
        extension_configs={"toc": {"slugify": github_slugger()}})
    css = """
body{font:%(font)spx/%(line)s -apple-system,"Segoe UI",Roboto,Helvetica,Arial,sans-serif;color:#1f2328;margin:0}
h1,h2,h3,h4,h5{line-height:1.25;margin:%(hspace)sem 0 .35em;break-after:avoid}
h1{font-size:1.75em} h2{font-size:1.4em}
h2,h3,h4,h5{text-decoration:underline;text-decoration-thickness:1px;text-underline-offset:.18em}
h3{font-size:1.18em} h4{font-size:1.08em} h5{font-size:1em;font-style:italic}
p,ul,ol,pre,blockquote{margin:0 0 %(pspace)sem} li{margin:0} li>p{margin:0 0 .3em}
table{border-collapse:collapse;font-size:%(table)spx;margin:0 0 %(pspace)sem;max-width:100%%}
td,th{border:1px solid #d0d7de;padding:%(pad)spx %(padx)spx;vertical-align:top} th{background:#f6f8fa} tr{break-inside:avoid}
code{font:%(code)spx/1.3 ui-monospace,Consolas,"DejaVu Sans Mono",monospace;background:#eff1f3;border-radius:3px;padding:0 .2em}
pre{background:#f6f8fa;padding:6px 8px;white-space:pre-wrap;word-break:break-word;border-radius:4px;break-inside:avoid-page}
pre code{background:none;padding:0}
img{max-width:100%%;height:auto;break-inside:avoid} a{color:#0969da;text-decoration:none}
blockquote{border-left:3px solid #d0d7de;padding:0 .8em;color:#57606a}
hr{border:0;border-top:1px solid #d0d7de;margin:.8em 0}
@page{size:A4;margin:%(margin)smm}
""" % dict(p, padx=p["pad"] + 3)
    return '<!doctype html><html><head><meta charset="utf-8"><title>Mousiki User Guide v5.0.0</title><base href="%s"><style>%s</style></head><body>%s</body></html>' % (
        base_dir.as_uri() + "/", css, body)


def render(html_text, out_pdf, chrome, margin_mm, numbers):
    from playwright.sync_api import sync_playwright
    with tempfile.NamedTemporaryFile("w", suffix=".html", delete=False, encoding="utf-8", dir=str(MANUAL.parent)) as f:
        f.write(html_text)
        tmp = Path(f.name)
    try:
        with sync_playwright() as pw:
            kw = {"executable_path": chrome} if chrome else {}
            browser = pw.chromium.launch(**kw)
            # the page is laid out as wide as the printed text (A4 minus the margins), so measurements below match the print
            page = browser.new_page(viewport={"width": int((210 - 2 * margin_mm) / 25.4 * 96), "height": 1200})
            page.emulate_media(media="print")
            page.goto(tmp.as_uri())
            page.wait_for_load_state("networkidle")
            # Blocks with box drawing characters (the menu maps) must never wrap and need touching lines: no wrapping, line height 1,
            # and the text size is lowered until the widest line fits the page.
            page.evaluate("""() => {
                for (const pre of document.querySelectorAll('pre')) {
                    if (!/[\\u2500-\\u257f]/.test(pre.textContent)) continue;
                    const code = pre.querySelector('code') || pre;
                    pre.style.whiteSpace = 'pre'; pre.style.overflow = 'hidden';
                    code.style.lineHeight = '1.0'; pre.style.lineHeight = '1.0';
                    let size = parseFloat(getComputedStyle(code).fontSize);
                    while (pre.scrollWidth > pre.clientWidth + 1 && size > 4) { size -= 0.25; code.style.fontSize = size + 'px'; }
                }
            }""")
            footer = ('<div style="font-size:8px;width:100%;text-align:center;color:#666">'
                      '<span class="pageNumber"></span> / <span class="totalPages"></span></div>') if numbers else "<span></span>"
            page.pdf(path=str(out_pdf), prefer_css_page_size=True, print_background=True,
                     display_header_footer=numbers, header_template="<span></span>", footer_template=footer,
                     outline=True, tagged=True)   # outline = the bookmarks (headings) a PDF viewer shows in its side bar
            browser.close()
        fix_bookmarks(out_pdf, html_text)
    finally:
        tmp.unlink(missing_ok=True)


def fix_bookmarks(pdf, html_text):
    """Chromium builds the bookmarks (the PDF outline) from the heading elements, but some viewers show titles twice
    ("Library, pairing and the video viewLibrary, pairing and the video view"). The outline is rewritten here with the clean heading
    texts from the HTML; the pages and positions Chromium found are kept. Needs pypdf (pip install pypdf); without it nothing changes."""
    try:
        from pypdf import PdfReader, PdfWriter
        from pypdf.generic import Fit
    except ImportError:
        print("note: pypdf is not installed, the bookmark titles stay as Chromium wrote them (pip install pypdf)")
        return
    import html as htmllib
    heads = [(int(m.group(1)), re.sub(r"\s+", " ", htmllib.unescape(re.sub(r"<[^>]+>", "", m.group(2)))).strip())
             for m in re.finditer(r"<h([1-6])[^>]*>(.*?)</h\1>", html_text, re.S)]
    reader = PdfReader(str(pdf))
    flat = []
    def walk(items):
        for it in items:
            if isinstance(it, list): walk(it)
            else: flat.append(it)
    walk(reader.outline)
    if len(flat) != len(heads):
        print("note: %d bookmarks but %d headings, bookmarks left as they are" % (len(flat), len(heads)))
        return
    writer = PdfWriter(clone_from=reader)
    root = writer._root_object
    if "/Outlines" in root: del root["/Outlines"]
    stack = []   # (level, outline item)
    for it, (level, title) in zip(flat, heads):
        page = reader.get_destination_page_number(it)
        top = it.top if getattr(it, "top", None) is not None else None
        while stack and stack[-1][0] >= level: stack.pop()
        parent = stack[-1][1] if stack else None
        fit = Fit.xyz(left=it.left, top=top, zoom=0) if top is not None else Fit.fit()
        node = writer.add_outline_item(title, page, parent=parent, fit=fit)
        stack.append((level, node))
    tmp = Path(str(pdf) + ".tmp")
    with open(tmp, "wb") as f: writer.write(f)
    tmp.replace(pdf)


def page_count(pdf):
    try:
        from pypdf import PdfReader
        return len(PdfReader(str(pdf)).pages)
    except Exception:
        data = Path(pdf).read_bytes()
        return len(re.findall(rb"/Type\s*/Page[^s]", data))


def main():
    ap = argparse.ArgumentParser(description="Mousiki manual: markdown -> PDF (Chromium)")
    ap.add_argument("--preset", choices=sorted(PRESETS), default="compact")
    ap.add_argument("--font-size", type=float, help="text size in CSS px")
    ap.add_argument("--line-height", type=float, help="line height, e.g. 1.35")
    ap.add_argument("--table-font-size", type=float, help="table text size in CSS px")
    ap.add_argument("--code-font-size", type=float, help="code text size in CSS px")
    ap.add_argument("--margin", type=float, help="page margin in mm")
    ap.add_argument("--cell-padding", type=int, help="table cell padding in px (vertical)")
    ap.add_argument("--paragraph-space", type=float, help="space under paragraphs / lists / tables in em")
    ap.add_argument("--heading-space", type=float, help="space above headings in em")
    ap.add_argument("--no-page-numbers", action="store_true")
    ap.add_argument("--input", default=str(MANUAL))
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--chrome", help="path to a Chrome / Chromium to use instead of Playwright's own")
    ap.add_argument("--compare", action="store_true", help="only print the page count of every preset (writes to a temp folder)")
    a = ap.parse_args()
    try:
        import markdown  # noqa: F401
        import playwright  # noqa: F401
    except ImportError as e:
        py = sys.executable or "python3"
        sys.exit("The Python module '%s' is missing for %s.\nInstall what the script needs with:\n"
                 "    %s -m pip install markdown playwright\n    %s -m playwright install chromium\n"
                 "(If pip says the environment is 'externally managed': make a private one first:\n"
                 "    python3 -m venv ~/.mousiki-pdf && ~/.mousiki-pdf/bin/pip install markdown playwright && "
                 "~/.mousiki-pdf/bin/python -m playwright install chromium\n"
                 "and run the script with ~/.mousiki-pdf/bin/python.)" % (e.name, py, py, py))

    md_path = Path(a.input)
    md_text = md_path.read_text(encoding="utf-8")

    def params(name):
        p = dict(PRESETS[name])
        for key, val in (("font", a.font_size), ("line", a.line_height), ("table", a.table_font_size), ("code", a.code_font_size),
                         ("margin", a.margin), ("pad", a.cell_padding), ("pspace", a.paragraph_space), ("hspace", a.heading_space)):
            if val is not None:
                p[key] = val
        return p

    if a.compare:
        with tempfile.TemporaryDirectory() as d:
            for name in PRESETS:
                p = params(name)
                out = Path(d) / (name + ".pdf")
                render(build_html(md_text, md_path.parent, p), out, a.chrome, p["margin"], False)
                print("%-8s %4d pages  (text %.1f px, tables %.1f px, margin %.0f mm)" % (name, page_count(out), p["font"], p["table"], p["margin"]))
        return
    p = params(a.preset)
    out = Path(a.out)
    render(build_html(md_text, md_path.parent, p), out, a.chrome, p["margin"], not a.no_page_numbers)
    print("%s: %d pages (preset %s: text %.1f px, tables %.1f px, code %.1f px, margin %.0f mm)" % (
        out, page_count(out), a.preset, p["font"], p["table"], p["code"], p["margin"]))


if __name__ == "__main__":
    main()
