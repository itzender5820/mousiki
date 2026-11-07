#!/usr/bin/env python3
"""Rewraps the lines of Mousiki_Help_Wizard.txt that are wider than the help wizard shows (107 columns at the default 120x32 terminal),
keeping the indentation (and the hanging indent of the aligned tables). [KEY] groups and [Text][[Legend]] markers are never split.
Usage: python3 tools/wizard_rewrap.py [--check]"""
import re, sys, unicodedata, os

LIMIT = 107
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
path = os.path.join(root, "Mousiki_Help_Wizard.txt")

def plain(l):
    l = re.sub(r"\[([^\[\]]*)\]\[\[(?:Header|Legend)\]\]", r"\1", l)
    return re.sub(r"\[\[[^\]]*\]\]", "", l)

def width(s):
    return sum(2 if unicodedata.east_asian_width(c) in "WF" else 1 for c in plain(s))

TOKEN = re.compile(r"(?:\[\[[^\]]*\]\]|\[[^\[\]]*\](?:\[\[[^\]]*\]\])?|[^\s\[]|\[)+")

def tokens(text):
    return TOKEN.findall(text)

def hang_indent(line):
    lead = len(line) - len(line.lstrip(" "))
    if lead > 0:
        return lead
    m = re.match(r"^(\S.*?\S?)(\s{3,})(?=\S)", line)      # "Label:      text": the text column
    if m and len(m.group(1)) + len(m.group(2)) <= 60 and len(m.group(2)) >= 3:
        return len(m.group(1)) + len(m.group(2))
    return 0

def wrap(line, indent, first_prefix_len=None):
    """greedy; returns lines; the first keeps its own prefix (everything before the text column), the rest start with `indent` blanks"""
    head = line[:indent] if len(line) >= indent and line[:indent].strip() == "" or hang_indent(line) == indent else ""
    lead_len = len(line) - len(line.lstrip(" "))
    # the first line: its own prefix up to the text column
    if lead_len == 0 and indent > 0:
        m = re.match(r"^(\S.*?\S?\s{3,})(?=\S)", line)
        prefix = line[:indent] if not m else m.group(1)
        body = line[len(prefix):]
        prefix = prefix.ljust(indent) if len(prefix) < indent else prefix
    else:
        prefix, body = line[:indent], line[indent:]
    out, cur, w = [], prefix, width(prefix)
    first = True
    for tok in tokens(body):
        tw = width(tok)
        if cur.strip() and w + (0 if cur.endswith(" ") else 1) + tw > LIMIT:
            out.append(cur.rstrip())
            cur, w = " " * indent, indent
        if cur.strip() and not cur.endswith(" "):
            cur += " "; w += 1
        cur += tok; w += tw
    out.append(cur.rstrip())
    return out

def main():
    check = "--check" in sys.argv
    lines = open(path, encoding="utf-8").read().split("\n")
    out, i, changed = [], 0, 0
    while i < len(lines):
        l = lines[i]
        s = l.strip()
        skip = (s.startswith("=>>>") or re.search(r"[─-╿]", l) or "yt-dlp" in l or width(l) <= LIMIT)
        if skip:
            out.append(l); i += 1; continue
        ind = hang_indent(l)
        w = wrap(l, ind)
        # cascade into following continuation lines (same indent, start like a continuation)
        tail = w[-1]
        j = i + 1
        while j < len(lines):
            n = lines[j]
            if not n.strip() or hang_indent(n) != ind or (n.startswith(" " * ind) is False and ind):
                break
            body = n.strip()
            short_tail = width(tail.strip()) < 60
            looks_new = re.match(r"^([A-Z0-9][A-Z0-9 /&()'.,-]*:|\[|-|\d+[.)] |=>|NOTE)", body)
            if not (ind >= 20 or body[:1].islower() or body[:1] in "(\"'"):
                break
            merged = tail + " " + body
            w2 = wrap(merged, ind)
            w = w[:-1] + w2
            tail = w[-1]
            j += 1
            if len(w2) == 1:
                break
        out += w
        changed += 1
        i = j
    if check:
        print("lines to rewrap:", changed)
        return
    open(path, "w", encoding="utf-8").write("\n".join(out))
    print("rewrapped:", changed)

main()
