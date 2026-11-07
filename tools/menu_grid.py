#!/usr/bin/env python3
"""Draws the menu map (the < key's organigram) of the three players as boxes ("grid") and puts it into
 * Mousiki_Help_Wizard.txt   between the lines "=>>> MENU GRID BEGIN" and "=>>> MENU GRID END" (narrow: fits 72 columns)
 * Mousiki_User_Manual/Mousiki_User_Guide.md   between "<!-- MENU GRID BEGIN -->" and "<!-- MENU GRID END -->" (wide)
The menus below are the single source of the grid: edit them here (and in src/organigram_trees.h) and run  python3 tools/menu_grid.py
"""
import os, re, sys

TREES = {
    'MUSIC PLAYER': [
        '  Main',
        '    System',
        '      Settings [s]',
        '      Console / logs / status [t]',
        '      Help Wizard [SHIFT+w]',
        '      Cheat Sheet [?]',
        '    Sound',
        '      Equalizer [SHIFT+e]',
        '      Loudness Normalization [SHIFT+v]',
        '      Sleep Timer [SHIFT+z]',
        '      Audio Device [°]',
        '    Library',
        '      Big Folder Overlay [SHIFT+f]',
        '      Big List Overlay [SHIFT+l]',
        '      Big Queue Overlay [SHIFT+k]',
        '    Other Lists',
        '      Playlists [SHIFT+p]',
        '      Listening History [SHIFT+h]',
        '    Meta and Multi Media Data',
        '      Meta Data Menu [SHIFT+m]',
        '        Fetch and Convert',
        '          Fetch Cover [ALT+C]',
        '          Fetch Video Overlay [ALT+V]',
        '          File Conversion [SHIFT+c]',
        '        Cover and Video Look',
        '          Style Options (cover look) [SHIFT+s]',
        '    Visualizations',
        '      Spectrum Bars [SHIFT+b]',
        '      Spectrogram',
        '        Options [SHIFT+i]',
        '        Overlay [SHIFT+u]',
        '        Extra Window [(]',
        '      Oscilloscope',
        '        Options [SHIFT+o]',
        '        Overlay [=]',
        '        Extra Window [)]',
        '      Video',
        '        Options [&]',
        '        Overlay [v]',
        '      Creator [^]',
        '    Lyrics',
        '      Refetch Lyrics [l]',
        '      Read and Translate Overlay [CTRL+L]',
        '      Karaoke Mode [k]',
        '      Lyrics Timing [ALT+L]',
    ],
    'AUDIO BOOK': [
        '  Main',
        '    System',
        '      Settings [s]',
        '      Console / logs / status [t]',
        '      Help Wizard [SHIFT+w]',
        '      Cheat Sheet [?]',
        '      File Conversion [SHIFT+c]',
        '    Library and Meta Data',
        '      Library Overlay [SHIFT+s]',
        '      Tag Editor [SHIFT+m]',
        '      Reading History [SHIFT+h]',
        '      Chapter Marks [ALT+L]',
        '    Bookmarks and Notes',
        '      Bookmark List [SHIFT+k]',
        '      New Note [SHIFT+n]',
        '      Set Bookmark [SHIFT+b]',
        '      Notes Overlay [SHIFT+l]',
        '    Sound',
        '      Equalizer [SHIFT+e]',
        '      Loudness Normalization [SHIFT+v]',
        '      Sleep Timer [SHIFT+z]',
        '      Audio Device [°]',
        '    Visualizations',
        '      Spectrum Bars [SHIFT+y]',
        '      Spectrogram',
        '        Options [SHIFT+i]',
        '        Overlay [SHIFT+u]',
        '      Oscilloscope',
        '        Options [SHIFT+o]',
        '        Overlay [=]',
    ],
    'RADIO': [
        '  Main',
        '    System',
        '      Settings [s]',
        '      Help Wizard [SHIFT+w]',
        '      Cheat Sheet [?]',
        '    Sound',
        '      Equalizer [SHIFT+e]',
        '      Loudness Normalization [SHIFT+v]',
        '      Sleep Timer [SHIFT+z]',
        '      Record [y]',
        '    Stations and Presets',
        '      Presets Menu [SHIFT+p]',
        '      Station Lists [SHIFT+k]',
        '      Radio Browser [SHIFT+s]',
        '      Big Stations Overlay [SHIFT+l]',
        '      Listening History [h]',
        '    Visualizations',
        '      Spectrum Bars [SHIFT+b]',
        '      Spectrogram',
        '        Options [SHIFT+i]',
        '        Overlay [SHIFT+u]',
        '        Extra Window [(]',
        '      Oscilloscope',
        '        Options [SHIFT+o]',
        '        Overlay [=]',
        '        Extra Window [)]',
    ],
}

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def parse(lines):
    """indented lines -> nested [(text, [children])]; the first level is "Main"."""
    top = []
    stack = [(-1, top)]
    for l in lines:
        ind = (len(l) - len(l.lstrip())) // 2
        node = (l.strip(), [])
        while stack and stack[-1][0] >= ind:
            stack.pop()
        stack[-1][1].append(node)
        stack.append((ind, node[1]))
    return top

def tree_lines(children, prefix=""):
    out = []
    for i, (t, ch) in enumerate(children):
        last = i == len(children) - 1
        out.append(prefix + ("\u2514 " if last else "\u251c ") + t)
        out += tree_lines(ch, prefix + ("  " if last else "\u2502 "))
    return out

def top_lines(children):
    """the entries of a category: the first level without a glyph, below them the tree"""
    out = []
    for t, ch in children:
        out.append(t)
        out += tree_lines(ch, " ")
    return out

def box(title, children, mark):
    body = top_lines(children)
    w = max([len(title) + 4] + [len(b) + 2 for b in body])
    top = "\u250c\u2500 " + (("[%s][[Header]]" % title) if mark else title) + " " + "\u2500" * (w - len(title) - 3) + "\u2510"
    rows = ["\u2502 " + b + " " * (w - len(b) - 1) + "\u2502" for b in body]
    return [top] + rows + ["\u2514" + "\u2500" * w + "\u2518"], w + 2

def vis(s):
    return len(re.sub(r"\[\[Header\]\]", "", re.sub(r"\[([^\[\]]*)\]\[\[Header\]\]", r"\1", s)))

def grid(name, lines, limit, mark):
    main = parse(lines)[0]
    cats = [box(t, ch, mark) for t, ch in main[1]]
    out = []
    row, used = [], 0
    rows = []
    for b in cats:
        if row and used + 1 + b[1] > limit:
            rows.append(row); row, used = [], 0
        used += (1 if row else 0) + b[1]
        row.append(b)
    if row: rows.append(row)
    for r in rows:
        h = max(len(b[0]) for b in r)
        for k in range(h):
            line = ""
            for j, b in enumerate(r):
                cell = b[0][k] if k < len(b[0]) else " " * b[1]
                if j: line += " "
                line += cell
            out.append(line.rstrip())
    return out

def build(limit, mark):
    res = {}
    for name, ls in TREES.items():
        res[name] = grid(name, ls, limit, mark)
    return res

def wizard_text():
    g = build(72, True)
    out = []
    for name, title in (("MUSIC PLAYER", "MUSIC PLAYER"), ("AUDIO BOOK", "AUDIO BOOK PLAYER"), ("RADIO", "RADIO PLAYER")):
        out.append("[%s: Main][[Header]]" % title)
        out += g[name]
        out.append("")
    return "\n".join(out).rstrip("\n")

def manual_text():
    g = build(150, False)
    out = []
    for name, title in (("MUSIC PLAYER", "Music player"), ("AUDIO BOOK", "Audio book player"), ("RADIO", "Radio player")):
        out.append("**%s** - `Main`:" % title)
        out.append("")
        out.append("```text")
        out += g[name]
        out.append("```")
        out.append("")
    return "\n".join(out).rstrip("\n")

def put(path, begin, end, text):
    s = open(path, encoding="utf-8").read()
    a, b = s.index(begin), s.index(end)
    s = s[:a + len(begin)] + "\n" + text + "\n" + s[b:]
    open(path, "w", encoding="utf-8").write(s)

if __name__ == "__main__":
    put(os.path.join(root, "Mousiki_Help_Wizard.txt"), "=>>> MENU GRID BEGIN", "=>>> MENU GRID END", wizard_text())
    put(os.path.join(root, "Mousiki_User_Manual", "Mousiki_User_Guide.md"), "<!-- MENU GRID BEGIN -->", "<!-- MENU GRID END -->", manual_text())
    print("menu grid written")
