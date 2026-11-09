#!/usr/bin/env python3
import os, sys, re, shutil, time, tempfile
"""Dev tool (not run by CI): drives the radio with ~250 scripted keys (every overlay and menu) and writes the screen after every key, with
the animated parts and the numbers masked, plus the files it wrote, to a transcript. Use it for a change of the radio code that must not change
what the radio does: run it with the old and with the new binary and diff the two transcripts (they are identical when nothing changed).
  python3 tools/radio_compare.py build/mousiki_radio transcript.txt [scripts-dir]"""
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tests"))
import ui_smoke as U

ESC = b"\x1b"
UP, DOWN, RIGHT, LEFT = b"\x1b[A", b"\x1b[B", b"\x1b[C", b"\x1b[D"
HOME_, END_, PGUP, PGDN = b"\x1b[H", b"\x1b[F", b"\x1b[5~", b"\x1b[6~"
SHL, SHR = b"\x1b[1;2D", b"\x1b[1;2C"
TAB, ENTER, BKSP, SPACE = b"\t", b"\r", b"\x7f", b" "

def seq():
    s = []
    def g(name, *keys):
        s.append((name, list(keys)))
    g("main-keys", b"n", b"n", b"b", b"#", b"j", b"m", b"m", b"p", b"+", b"+", b"-", b"=", ESC, b"x", b"x", SHR, SHL, UP, DOWN, LEFT, RIGHT)
    g("cheat", b"?", DOWN, DOWN, PGDN, PGUP, UP, ESC)
    g("wizard", b"W", RIGHT, RIGHT, LEFT, ESC)
    g("settings-walk", b"s", DOWN, DOWN, DOWN, RIGHT, LEFT, UP, TAB, DOWN, RIGHT, TAB, DOWN, DOWN, RIGHT, TAB, DOWN, RIGHT, TAB, DOWN, ENTER, ESC, ESC, ESC)
    g("settings-tabs", b"s", TAB, TAB, TAB, TAB, TAB, TAB, TAB, TAB, TAB, TAB, ESC, ESC)
    g("settings-edit", b"s", DOWN, DOWN, ENTER, b"abc", BKSP, ESC, ESC, ESC)
    g("history", b"h", DOWN, DOWN, UP, ENTER, ESC, ESC)
    g("history-shift", b"H", DOWN, ESC)
    g("stations-overlay", b"L", DOWN, DOWN, UP, b"a", b"http://x.example/s", ESC, b"d", ESC, b"e", ESC, ESC)
    g("stations-search", b"L", b"/", b"rad", BKSP, ESC, ESC)
    g("record", b"y", DOWN, ESC)
    g("sleep", b"Z", DOWN, DOWN, ENTER, ESC, b"Z", ESC)
    g("equaliser", b"E", RIGHT, RIGHT, UP, DOWN, LEFT, TAB, ESC)
    g("bars", b"B", DOWN, RIGHT, LEFT, ESC)
    g("spectro", b"I", DOWN, RIGHT, ESC)
    g("scope-big", b")", RIGHT, LEFT, UP, DOWN, ESC)
    g("scope2", b"(", UP, DOWN, ESC)
    g("loudness", b"V", DOWN, RIGHT, ESC)
    g("osc-overlay", b"O", DOWN, RIGHT, LEFT, ESC)
    g("update", b"U", ESC)
    g("menus", b"<", RIGHT, RIGHT, DOWN, LEFT, UP, ESC, ESC)
    g("menus-enter", b"<", DOWN, ENTER, ESC, ESC)
    g("browse", b"S", DOWN, TAB, UP, ESC, ESC)
    g("lists-menu", b"K", DOWN, TAB, DOWN, TAB, ESC, ESC)
    g("lists-new", b"K", b"n", b"abc", ENTER, ESC, ESC)
    g("presets", b"P", DOWN, DOWN, UP, RIGHT, LEFT, b"1", ENTER, ESC)
    g("presets-search", b"P", b"/", b"ro", ENTER, ESC, ESC)
    g("search", b"/", b"pl", BKSP, ESC, ESC)
    g("search-keep", b"/", b"a", ENTER, UP, DOWN, ESC, ESC)
    g("timeshift", b".", b".", b"j", b"j", b"p")
    g("timeshift2", b"T", b"T", ESC)
    g("quote", b'"', ESC)
    g("record2", b"R", ESC)
    g("surf", b"n", b"n", b"n", b"b", b"b", b"#", b"#")
    g("fav", b"s", ESC, b"x", b"x")
    g("window-keys", b"8", b"9", b"*")
    g("shift-keys", b"!", b"@", b"$", b"%", b"^", b"&", b"*", ESC)
    g("settings-theme", b"s", ENTER, DOWN, DOWN, UP, RIGHT, ESC, ESC, ESC)
    g("settings-theme2", b"s", b"l", ENTER, DOWN, RIGHT, LEFT, TAB, ESC, ESC, ESC, ESC)
    g("settings-theme-edit", b"s", b"e", DOWN, RIGHT, ENTER, ESC, ESC, ESC)
    g("settings-theme-io", b"s", b"E", b"x", ESC, b"I", ESC, ESC)
    g("settings-paths", b"s", TAB, TAB, TAB, DOWN, ENTER, b"xy", LEFT, BKSP, ESC, ESC, ESC)
    g("settings-keys", b"s", TAB, TAB, TAB, TAB, DOWN, ENTER, b"z", ESC, DOWN, ENTER, ESC, ESC, ESC)
    g("settings-about", b"s", TAB, TAB, TAB, TAB, TAB, DOWN, DOWN, UP, ESC, ESC)
    g("settings-leave", b"s", DOWN, RIGHT, ESC, b"n", ESC, ESC)
    g("settings-leave2", b"s", DOWN, RIGHT, ESC, b"y")
    g("settings-ctrl", b"s", b"\x15", b"\x1b[1;6U", b"j", b"k", b"h", b"l", ESC, ESC)
    g("end", b"p", b"p")
    return s

NORM_BRAILLE = re.compile("[⠀-⣿▀-▟■-◿]")
def norm(t):
    t = NORM_BRAILLE.sub(" ", t)
    t = re.sub(r"/tmp/radcmp_\w+", "/tmp/radcmp_X", t)
    t = re.sub(r"[0-9]", "#", t)
    t = re.sub(r"[ ]+", " ", t)
    return "\n".join(l.rstrip() for l in t.split("\n")).strip()

def main(binary, out, scripts=os.path.join(HERE, "..", "scripts"), wait=0.5):
    binary = os.path.abspath(binary)
    scripts = os.path.abspath(scripts)
    root = tempfile.mkdtemp(prefix="radcmp_")
    home = os.path.join(root, "home")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\n"
                "Plain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n"
                "Second Radio | Jazz | DE | AAC | 96 | http://127.0.0.1:9/d\n"
                "Third FM | Pop | FR | MP3 | 64 | http://127.0.0.1:9/e | *\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("# test\nStartMode=radio\nWizardSeen=true\n")
    U.COLS, U.ROWS = 160, 50
    sess = U.Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1", "MOUSIKI_RADIO_SEED": "7"}, quiet_stderr=True)
    log = []
    try:
        for _ in range(40):
            sess.pump(0.5)
            if "Plain Station" in sess.text(): break
        sess.send(ESC, 1.0)
        for name, keys in seq():
            for i, k in enumerate(keys):
                sess.send(k, wait)
                alive = sess.alive()
                log.append("=== %s #%d %r alive=%s\n%s\n" % (name, i, k, alive, norm(sess.text())))
                if not alive: break
            if not sess.alive(): break
            sess.send(ESC, 0.3); sess.send(ESC, 0.3)
        sess.send(b"q", 1.0)
    finally:
        sess.kill()
        for dp, dn, fn in sorted(os.walk(home)):
            for f in sorted(fn):
                fp = os.path.join(dp, f)
                if "cache" in fp or "control" in fp or f.endswith(".pcm"): continue
                try: body = open(fp, encoding="utf-8", errors="replace").read()
                except OSError: continue
                log.append("=== FILE %s\n%s\n" % (os.path.relpath(fp, home), re.sub(r"/tmp/radcmp_\w+", "/tmp/radcmp_X", body)))
        shutil.rmtree(root, ignore_errors=True)
    open(out, "w", encoding="utf-8").write("\n".join(log))
    print(out, len(log), "snapshots")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], *(sys.argv[3:4] or []))
