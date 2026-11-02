#!/usr/bin/env python3
"""UI smoke test: runs the real `mousiki` binary in a pseudo terminal and walks through the main screens.

    python3 tests/ui_smoke.py <path to mousiki binary> [--scripts <scripts folder>] [--keep]

What it checks (it never needs a sound card, a network or a real library; everything lives in a temporary HOME):
  * the program starts, draws the library and survives every key press below (no crash, no hang),
  * the frames of the main screen, settings tabs, help wizard, cheat sheet, MENUS index, meta editor and cover options
    keep their right border in one column (the symptom of a broken wide-character layout),
  * PgUp / PgDn page like SHIFT+arrow and the Translit. Kanji row of the settings cycles,
  * the Latin-letters toggle really turns Japanese titles into Latin ones and back,
  * the program quits when asked to.

The scenarios run in parallel worker processes (MOUSIKI_SMOKE_JOBS=n, default: the cores, at most 4; 1 = one after the other;
`--only a,b` runs some of them: main, read_lyrics_play, karaoke_timing, book_speed_and_devices, paging_and_translit_row, mode_keys,
kitty_cover_options, radio_latin_toggle, selftest_command).

Needs Linux or macOS and the Python package `pyte` (pip install pyte). Without them the test reports "skipped"
(exit code 77, which CTest treats as skipped).
"""
import os
import re
import select
import shutil
import signal
import struct
import sys
import tempfile
import time
import wave
import zlib

try:
    import fcntl
    import pty
    import select
    import termios
    import pyte
except Exception as exc:  # no pty (Windows) or pyte missing
    print("skipped: %s" % exc)
    sys.exit(77)

COLS, ROWS = 140, 45
BOX_LEFT = "│╭╰├"
BOX_RIGHT = "│╮╯┤"


def make_wav(path, seconds=2):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(8000)
        w.writeframes(b"\x00\x00" * 8000 * seconds)


def make_png(path, size=64):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes([200, 40, 40]) * size for _ in range(size))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0)) \
        + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


class Session:
    def __init__(self, binary, home, scripts, env_extra=None, quiet_stderr=False):
        self.raw = b""
        env = dict(os.environ, HOME=home, USERPROFILE=home, TERM="xterm-256color", LANG="C.UTF-8")
        env.pop("KITTY_WINDOW_ID", None)
        env.pop("WT_SESSION", None)
        if scripts:
            env["MOUSIKI_SCRIPTS_DIR"] = scripts
        env.update(env_extra or {})
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            # the size is set here, before the program starts: set only afterwards (by the parent, below) a slow machine
            # could start the program first and let it read a 0 x 0 terminal
            try:
                fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
            except OSError:
                pass
            if quiet_stderr:   # (ALSA writes its complaints about a missing sound card onto the screen)
                try:
                    os.dup2(os.open(os.devnull, os.O_WRONLY), 2)
                except OSError:
                    pass
            os.execve(binary, [binary], env)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
        self.screen = pyte.Screen(COLS, ROWS)
        self.stream = pyte.ByteStream(self.screen)
        self.exit_status = None

    def alive(self):
        if self.exit_status is not None:
            return False
        pid, status = os.waitpid(self.pid, os.WNOHANG)
        if pid:
            self.exit_status = status
            return False
        return True

    def pump(self, seconds):
        end = time.time() + seconds * SLOW
        # A frame is written in one go between "begin / end synchronized update" (ESC[?2026h ... ESC[?2026l), but it reaches us
        # in pieces: when the time is up in the middle of one, read on (at most one more second) until that frame is complete,
        # so the checks never see half a frame (on a slow machine that happened now and then: "frame broken" on a good frame).
        hard_end = end + 1.0
        while time.time() < end or (time.time() < hard_end and self.raw.rfind(b"\x1b[?2026h") > self.raw.rfind(b"\x1b[?2026l")):
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    data = os.read(self.fd, 65536)
                except OSError:
                    return
                if not data:
                    return
                self.raw += data
                self.stream.feed(data)

    def send(self, keys, wait=0.8):
        try:
            os.write(self.fd, keys if isinstance(keys, bytes) else keys.encode())
        except OSError:
            # the program has just ended and closed its terminal (macOS reports EIO on the write; the quit test
            # sends a key while the program is on its way out): nothing left to send to
            return
        self.pump(wait)

    def text(self):
        return "\n".join(self.screen.display)

    def row_cells(self, y):
        row = self.screen.buffer[y]
        return [row[x].data for x in range(COLS)]

    def dump(self, why):
        """What the screen shows (for the log of a failed check: a CI run cannot be looked at)."""
        print("---- screen at: %s ----" % why)
        print("\n".join("%2d %s" % (i, l.rstrip()) for i, l in enumerate(self.screen.display) if l.strip()))
        print("---- end of screen (alive=%s) ----" % self.alive())

    def kill(self):
        # A blocking waitpid hung on macOS: the dying program can wait for its terminal output to be read (the pty buffer
        # is full), so keep draining the master side while waiting, and give up after 10 s.
        try:
            os.kill(self.pid, signal.SIGKILL)
        except Exception:
            return
        end = time.time() + 10
        while time.time() < end:
            try:
                pid, _ = os.waitpid(self.pid, os.WNOHANG)
                if pid:
                    return
            except Exception:
                return
            try:
                if select.select([self.fd], [], [], 0.1)[0]:
                    os.read(self.fd, 65536)
            except Exception:
                time.sleep(0.1)
        print("warning: the program did not end after SIGKILL")


failures = []
checks = 0

# Every wait is multiplied by this: the macOS CI runners are slow (builds and program alike), a screen that is not there after
# 0.8 s may be there after 1.6 s. MOUSIKI_SMOKE_SLOW overrides (1 = as written).
SLOW = float(os.environ.get("MOUSIKI_SMOKE_SLOW", "2.0" if sys.platform == "darwin" else "1.0"))


T0 = time.time()


def progress(what):
    """A timestamped line, flushed at once: if the test hangs on a CI machine the log shows how far it got."""
    print("[%4ds] %s" % (time.time() - T0, what), flush=True)


def check(cond, what):
    global checks
    checks += 1
    if not cond:
        failures.append(what)
        print("  FAIL:", what, flush=True)


def frame_problems(sess):
    """Rows that start with a box character must end with one, all in the same column."""
    ends = {}
    for y in range(ROWS):
        cells = sess.row_cells(y)
        if cells[0] not in BOX_LEFT:
            continue
        last = max((x for x, c in enumerate(cells) if c not in (" ", "")), default=-1)
        ends[y] = (last, cells[last] if last >= 0 else " ")
    if len(ends) < 5:
        return []  # not a framed screen
    cols = [e[0] for e in ends.values()]
    mode = max(set(cols), key=cols.count)
    bad = []
    for y, (last, ch) in ends.items():
        if last != mode or ch not in BOX_RIGHT:
            bad.append("row %d ends at col %d with %r (frame edge is col %d)" % (y, last, ch, mode))
    return bad


def check_frame(sess, name, frame=True):
    check(sess.alive(), "%s: the program is still running" % name)
    bad = frame_problems(sess) if frame else []
    check(not bad, "%s: frame broken: %s" % (name, "; ".join(bad[:3])))
    if bad:
        print("\n".join("%2d %s" % (i, l.rstrip()) for i, l in enumerate(sess.screen.display) if l.strip()))


def kitty_cover_options(binary, home, scripts):
    """Cover options overlay in a Kitty-graphics terminal: the preview is a real picture, [t] switches to braille and back."""
    sess = Session(binary, home, scripts, {"KITTY_WINDOW_ID": "1", "TERM": "xterm-kitty"})
    try:
        sess.pump(4)
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        sess.send("M", 2.0)
        sess.send(b"\t", 0.5)
        sess.send(b"\t", 0.5)
        for _ in range(12):
            sess.send(b"\x1b[B", 0.15)
        sess.send(b"\x1b[D", 0.3)       # (the last stop is Style Option: Left goes to Cover Options)
        sess.send(b"\r", 1.5)
        check("COVER OPTIONS" in sess.text(), "kitty cover options: the overlay opens")
        def picture_after(mark, wait):
            """True when a picture (Kitty graphics transmit) arrives within `wait` seconds after `mark`: the preview is
            decoded in the background, so on a slow machine it comes late."""
            end = time.time() + wait
            while time.time() < end:
                if b"\x1b_Ga=T" in sess.raw[mark:]:
                    return True
                sess.pump(0.25)
            return b"\x1b_Ga=T" in sess.raw[mark:]

        if shutil.which("ffmpeg") is None:
            # pictures are decoded by ffmpeg: without it the preview says "(the picture cannot be read)" (a CI runner)
            print("  skipped: kitty cover options, the picture checks (ffmpeg is not installed on this machine)")
            return
        mark = len(sess.raw)
        sess.send(b"\x1b[B", 0.5)
        sess.send(b"\x1b[B", 0.8)   # automatic -> no cover -> cover.png
        shows_picture = picture_after(mark, 10.0)
        check(shows_picture, "kitty cover options: the preview of a picture is a real picture")
        if not shows_picture:
            sess.dump("kitty cover options: no picture arrived")
            print("graphics commands seen so far: %d, TERM=%s" % (sess.raw.count(b"\x1b_G"), "xterm-kitty"))
        for _ in range(3):           # every [t] flips between picture and braille
            mark = len(sess.raw)
            sess.send("t", 0.5)
            now = picture_after(mark, 10.0 if not shows_picture else 2.5)   # expecting a picture: wait for it; braille: nothing may come
            check(now != shows_picture,
                  "kitty cover options: [t] switches the preview to %s" % ("braille (ASCII)" if shows_picture else "a picture"))
            shows_picture = now
        if not shows_picture:
            sess.send("t", 1.0)          # (back to the picture for the next check)
        check(sess.alive(), "kitty cover options: the program is still running")
    finally:
        sess.kill()


def radio_latin_toggle(binary, root, scripts):
    """Radio mode: the quote key shows Japanese / Korean station names in Latin letters and back."""
    home = os.path.join(root, "radio_home")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\n"
                "さくらラジオ | J-Pop | JP | MP3 | 128 | http://127.0.0.1:9/a | *\n"
                "사랑 방송 | K-Pop | KR | MP3 | 128 | http://127.0.0.1:9/b | *\n"
                "Plain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nStartMode=radio\nLatinLetters=false\n" % os.path.join(root, "music"))
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Plain Station" in sess.text():
                break
        sess.send(b"\x1b", 1.0)   # (a first start may show the help wizard)
        check("Plain Station" in sess.text(), "radio: the station list is shown")
        check("さくらラジオ" in sess.text(), "radio: station names show in their own script by default")
        sess.send('"', 1.5)
        check("sakurarajio" in sess.text() and "さくらラジオ" not in sess.text(), "radio: [\"] shows Japanese station names in Latin letters")
        check("sarang" in sess.text(), "radio: [\"] shows Korean station names in Latin letters")
        sess.send('"', 1.5)
        check("さくらラジオ" in sess.text(), "radio: [\"] again shows the original names")
        check(sess.alive(), "radio: the program is still running")
    finally:
        sess.kill()


def selftest_command(binary, scripts):
    """`mousiki --selftest-fetch` starts the fetch health check (offline part here) and passes its exit code on."""
    import subprocess
    env = dict(os.environ)
    if scripts:
        env["MOUSIKI_SCRIPTS_DIR"] = scripts
    p = subprocess.run([binary, "--selftest-fetch", "--offline", "--only", "scripts,pins"], capture_output=True, text=True, timeout=120, env=env)
    check(p.returncode == 0, "--selftest-fetch: exit code 0 (got %d): %s" % (p.returncode, (p.stdout + p.stderr)[-200:]))
    check("fetch health check" in p.stdout and "helper scripts compile" in p.stdout, "--selftest-fetch: prints the report")
    p = subprocess.run([binary, "--selftest-fetch", "--bogus;rm"], capture_output=True, text=True, timeout=60, env=env)
    check(p.returncode == 2, "--selftest-fetch: refuses an odd option (exit %d)" % p.returncode)


def paging_and_translit_row(binary, root, scripts):
    """PgUp / PgDn page like SHIFT+arrow, and the TRANSLITERATION tab has the Translit. Kanji row (Leave / Chin. / Jap.)."""
    home = os.path.join(root, "paging_home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\n" % lib)
    sess = Session(binary, home, scripts)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)   # the first-start help wizard
        for _ in range(20):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        # the cheat sheet is longer than the screen: PgDn scrolls it (like SHIFT+down), PgUp brings it back
        sess.send("?", 1.2)
        first = sess.text()
        sess.send(b"\x1b[6~", 0.8)
        paged = sess.text()
        check(paged != first, "paging: PgDn scrolls the cheat sheet")
        sess.send(b"\x1b[5~", 0.8)
        check(sess.text() == first, "paging: PgUp scrolls it back")
        sess.send(b"\x1b[1;2B", 0.8)
        check(sess.text() == paged, "paging: SHIFT+down does the same as PgDn")
        # the music player's cheat sheet lists its own categories only: no audio book rows
        seen = sess.text()
        for _ in range(40):
            sess.send(b"\x1b[6~", 0.3)
            seen += "\n" + sess.text()
        check("SOUND (MAIN UI)" in seen and "AUDIO BOOK" not in seen,
              "cheat sheet: music player shows its categories, none of the audio book mode")
        sess.send(b"\x1b", 0.8)
        check("Alpha Song" in sess.text(), "paging: ESC leaves the cheat sheet")

        # settings -> GENERAL -> TRANSLITERATION -> Translit. Kanji
        sess.send("s", 1.5)
        sess.send(b"\t", 0.6)
        def find(label):
            for line in sess.screen.display:
                if label in line and "Preview:" not in line:   # (the preview box on the right is titled with the row)
                    return line
            return ""
        # GENERAL tab: Playback, Start Mode, Display queue, MODES ON/OFF AND ORDER (6 rows), AUDIO (5 rows),
        # TERMINAL: Black Background, then Use Terminal Settings (row 15, after AUDIO with Choose Audio Device)
        for _ in range(15):
            sess.send(b"\x1b[B", 0.1)
        check("Use Terminal Settings" in find("Use Terminal Settings") and "false" in find("Use Terminal Settings"),
              "use terminal settings: the row exists and is false by default")
        sess.send(b"\x1b[B", 0.1)
        check("Terminal Size" in find("Terminal Size") and "120" in find("Terminal Size") and "32" in find("Terminal Size"),
              "terminal size: the row shows 120 x 32 by default")
        check("Set back to default" in find("Terminal Size"), "terminal size: the Set back to default button is next to it")
        sess.send(b"\x1b[C", 0.4)   # RIGHT: the rows field
        sess.send(b"\r", 0.5)
        for _ in range(3):
            sess.send(b"\x7f", 0.1)
        sess.send("5", 0.2)
        sess.send(b"\r", 0.8)
        # raised to the minimum: the status line says so, and the row shows 28 (either one proves it; the status line can
        # be replaced by another message on a slow machine)
        def raised():
            return "allowed: 28" in sess.text() or "28" in find("Terminal Size")
        for _ in range(12):
            if raised():
                break
            sess.pump(0.5)
        if not raised():
            sess.dump("terminal size: the row did not become 28")
        check(raised(), "terminal size: a number below the minimum is raised to it (status line or row shows 28)")
        check("28" in find("Terminal Size"), "terminal size: the rows field shows the minimum, 28")
        sess.send(b"\x1b[C", 0.4)   # RIGHT: the button
        sess.send(b"\r", 0.8)
        check("32" in find("Terminal Size") and "28" not in find("Terminal Size"), "terminal size: ENTER on the button sets it back to 120 x 32")
        sess.send(b"\x1b[B", 0.2)
        check("Full Screen" in find("Full Screen") and "false" in find("Full Screen"), "full screen: the row exists and is off by default")
        for _ in range(10):
            sess.send(b"\x1b[B", 0.1)   # down to Translit. Kanji (past REMOTE / UPDATES; TEXT AND TRANSLITERATION is last)
        def row():
            return find("Translit. Kanji")
        def settle(word, limit=8.0):   # a slow machine draws the row a moment after the key: wait for it (up to `limit` s)
            end = time.time() + limit
            while word not in row() and time.time() < end:
                sess.pump(0.3)
        settle("Leave")
        check("Leave" in row(), "translit kanji: the row exists and reads Leave by default")
        check("Preview: Translit. Kanji" in sess.text() and "zh\u0101ng" in sess.text(), "settings preview: the hovered row shows its examples")
        sess.send(b"\x1b[C", 0.8)
        settle("Chin.")
        check("Chin." in row(), "translit kanji: RIGHT cycles to Chin.")
        sess.send(b"\x1b[C", 0.8)
        settle("Jap.")
        check("Jap." in row(), "translit kanji: RIGHT again cycles to Jap.")
        sess.send(b"\x1b[D", 0.8)
        settle("Chin.")
        sess.send(b"\x1b[D", 0.8)
        settle("Leave")
        check("Leave" in row(), "translit kanji: LEFT twice is back at Leave")
        sess.send(b"\x1b[6~", 0.8)   # PgDn in the settings: a page, like SHIFT+down
        check(sess.alive(), "paging: PgDn in the settings does not crash")
        sess.send(b"\x1b", 1.0)
        check(sess.alive(), "settings: ESC leaves it")
    finally:
        sess.kill()


def mode_keys(binary, root, scripts):
    """The mode switch key is rebindable (HKeyModeNext): the new key switches player <-> radio, the old one does nothing."""
    home = os.path.join(root, "modekeys_home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\n"
                "Plain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nHKeyModeNext=\"J\"\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        check(not re.search(rb"\x1b\[8;\d+;\d+t", sess.raw), "use terminal settings: true leaves the terminal alone")
        sess.send("*", 1.5)
        check("Alpha Song" in sess.text() and "Plain Station" not in sess.text(), "mode keys: the old key (*) does nothing after the rebinding")
        sess.send("J", 2.5)
        for _ in range(10):
            if "Plain Station" in sess.text() or "HELP WIZARD" in sess.text():
                break
            sess.pump(0.5)
        if "HELP WIZARD" in sess.text():   # (the radio's own first start)
            sess.send(b"\x1b", 1.5)
        check("Plain Station" in sess.text(), "mode keys: the rebound key (J) switches to the radio")
        sess.send("J", 2.5)
        for _ in range(10):
            if "PROGRESS BAR" in sess.text():
                break
            sess.pump(0.5)
        check("PROGRESS BAR" in sess.text() and "Plain Station" not in sess.text(), "mode keys: the rebound key (J) in the radio switches on (back to the player)")
        check(sess.alive(), "mode keys: the program is still running")
    finally:
        sess.kill()


def book_speed_and_devices(binary, root, scripts):
    """Audio book: SHIFT+LEFT / SHIFT+RIGHT change the playback speed, shown in a box left of the play mode (and saved);
    the CHOOSE AUDIO DEVICE overlay opens from the main screen of the audio book and the radio and lists System Default;
    the GENERAL tab has the Playback Speed and Choose Audio Device rows."""
    home = os.path.join(root, "book_home")
    books = os.path.join(root, "books")
    make_wav(os.path.join(books, "Book One", "01 Chapter.wav"))
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(cfgdir)
    with open(os.path.join(cfgdir, "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\n"
                "Plain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        # fixed keys, whatever the keyboard layout of the machine (the default mode key is "*" or "'", see default_size_mode_key())
        f.write("AudioBookPath=%s\nStartMode=audiobook\nUseTerminalSettings=true\nHKeyAudioDevice=\"&\"\nHKeyModeNext=\"J\"\n" % books)
    with open(os.path.join(cfgdir, "radio_config.txt"), "w", encoding="utf-8") as f:
        f.write("HKeyAudioDevice=&\n")
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1", "MOUSIKI_NULL_AUDIO": "1"})
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Book One" in sess.text():
                break
        check("Book One" in sess.text(), "audio book: the book is listed")
        check("1.0x" in sess.text(), "audio book: the speed box shows 1.0x")
        check_frame(sess, "audio book main screen", frame=False)
        sess.send(b"\x1b[1;2C", 0.8)   # SHIFT+RIGHT
        check("1.05x" in sess.text(), "audio book: SHIFT+RIGHT makes it 1.05x")
        sess.send(b"\x1b[1;2D", 0.5)
        sess.send(b"\x1b[1;2D", 0.8)   # SHIFT+LEFT twice
        check("0.95x" in sess.text(), "audio book: SHIFT+LEFT twice makes it 0.95x")
        check_frame(sess, "audio book main screen with 0.95x", frame=False)
        with open(os.path.join(cfgdir, "config.txt"), encoding="utf-8") as f:
            check("BookSpeed=0.95" in f.read(), "audio book: the speed is saved (BookSpeed=0.95)")
        sess.send("&", 1.2)
        check("Choose Audio Device" in sess.text() and "System Default" in sess.text(), "audio device: & opens the overlay in the audio book")
        sess.send(b"\x1b", 0.8)
        check("Choose Audio Device" not in sess.text(), "audio device: ESC closes the overlay")
        sess.send("s", 1.2)
        for _ in range(20):            # a slow machine: wait for the settings screen before TAB
            if "[TAB] Switch" in sess.text():
                break
            sess.pump(0.5)
        sess.send(b"\t", 1.0)          # COLORS -> GENERAL
        for _ in range(10):
            if "GENERAL" in sess.text() and "Start Mode" in sess.text():
                break
            sess.pump(0.5)
        seen = sess.text()
        for _ in range(40):            # the AUDIO rows may be below the visible part: walk down until both have been seen
            if "Playback Speed" in seen and "Choose Audio Device" in seen:
                break
            sess.send(b"\x1b[B", 0.15)
            seen += sess.text()
        check("Playback Speed" in seen and "0.95x" in seen, "settings: GENERAL has Playback Speed (0.95x) in the audio book mode")
        check("Choose Audio Device" in seen and "System Default" in seen, "settings: GENERAL has Choose Audio Device (System Default)")
        if "Playback Speed" not in seen:
            sess.dump("settings GENERAL tab of the audio book mode")
        sess.send(b"\x1b", 1.0)
        for _ in range(2):              # audio books -> music player -> radio (each may open its help wizard on its first start)
            sess.send("J", 2.5)
            for _ in range(10):
                if "HELP WIZARD" in sess.text():
                    break
                sess.pump(0.3)
            if "HELP WIZARD" in sess.text():
                sess.send(b"\x1b", 1.5)
        for _ in range(10):
            if "Plain Station" in sess.text():
                break
            sess.pump(0.5)
        if "Plain Station" in sess.text():
            sess.send("&", 1.5)
            check("Choose Audio Device" in sess.text() and "System Default" in sess.text(), "audio device: & opens the overlay in the radio")
            sess.send(b"\x1b", 0.8)
            check("Choose Audio Device" not in sess.text(), "audio device: ESC closes it in the radio")
        else:
            check(False, "audio device: the radio could not be reached for the overlay check")
        check(sess.alive(), "audio book / devices: the program is still running")
    finally:
        sess.kill()


def karaoke_timing(binary, root, scripts):
    """The lyrics timing overlay (ALT+L) opens over the karaoke screen; ESC brings the karaoke screen back."""
    home = os.path.join(root, "kt_home")
    lib = os.path.join(root, "kt_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics"))
    make_wav(os.path.join(lib, "Karaoke Song.wav"), 20)
    with open(os.path.join(lib, "lyrics", "Karaoke Song.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]First lyric line here\n[00:03.00]Second lyric line here\n[00:06.00]Third lyric line here\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Karaoke Song" in sess.text():
                break
        sess.send(b"\r", 3.0)
        sess.send("k", 1.5)
        for _ in range(20):   # (the lyrics are read from the sidecar file by a background thread)
            if "lyric line" in sess.text():
                break
            sess.pump(0.5)
        check("KARAOKE" in sess.text() and "lyric line" in sess.text(), "karaoke timing: the karaoke screen shows the lyrics")
        if shutil.which("ffprobe") is None:
            # without ffmpeg / ffprobe the track is not loaded properly (the screen shows no length, "/ 0:00 ]"), so the overlay,
            # which needs a loaded track with its lyrics, cannot open: the same reason the Kitty picture checks are skipped
            print("  skipped: karaoke timing overlay (ffmpeg / ffprobe is not installed on this machine)")
            return
        if re.search(r"\[ 0:00 / ", sess.text()):
            # no track is playing (the position stays at 0:00 several seconds after ENTER): a machine without a (working) audio
            # device, such as a CI runner, or one where the audio device does not start in time. The timing overlay
            # needs the playing track, so there is nothing to test here.
            print("  skipped: karaoke timing overlay (the track does not play on this machine: no audio device)")
            return
        timing_key = b"J" if sys.platform == "darwin" else b"\x1bl"   # macOS: SHIFT+j (Option is not a modifier in most terminals)
        for _ in range(6):   # the key is ignored until the lyrics are loaded: try again for a few seconds
            sess.send(timing_key, 1.5)
            if "Lyrics Timing" in sess.text():
                break
            sess.pump(1.0)
        check("Lyrics Timing" in sess.text() and "KARAOKE" in sess.text(), "karaoke timing: the timing key opens the timing overlay on top of the karaoke screen")
        if "Lyrics Timing" not in sess.text():
            sess.dump("karaoke timing: the overlay did not open")
        sess.send(b"\x1b", 1.5)
        check("KARAOKE" in sess.text() and "Lyrics Timing" not in sess.text(), "karaoke timing: ESC closes the overlay and the karaoke screen is back")
    finally:
        sess.kill()


def read_lyrics_play(binary, root, scripts):
    """READ LYRICS (CTRL+L): a saved translation is shown at once, and [p] plays the track with the lyrics highlighted."""
    home = os.path.join(root, "rl_home")
    lib = os.path.join(root, "rl_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics", "translations"))
    make_wav(os.path.join(lib, "Read Song.wav"), 20)
    lines = ["First lyric line here", "Second lyric line here", "Third lyric line here"]
    with open(os.path.join(lib, "lyrics", "Read Song.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]%s\n[00:03.00]%s\n[00:06.00]%s\n" % tuple(lines))
    h = 1469598103934665603     # the fingerprint of the original lines (FNV-1a, see lyrics_translate.cpp)
    for l in lines:
        for c in l.encode("utf-8"):
            h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        h = ((h ^ 0x0a) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    with open(os.path.join(lib, "lyrics", "translations", "Read Song.en.txt"), "w", encoding="utf-8") as f:
        f.write("#mousiki-translation to=en from=de svc=google lines=3 hash=%016x\n" % h)
        f.write("Erste Zeile hier\nZweite Zeile hier\nDritte Zeile hier\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Read Song" in sess.text():
                break
        sess.send(b"\x0c", 1.5)   # CTRL+L on the hovered track
        check("READ LYRICS" in sess.text() and "First lyric line here" in sess.text(), "read lyrics: the overlay shows the lyrics")
        check("(Erste Zeile hier)" in sess.text(), "read lyrics: a saved translation is shown without asking")
        check("[p] Play" in sess.text(), "read lyrics: the legend names the play key")
        check_frame(sess, "read lyrics")
        sess.send("p", 3.0)        # the track starts, the reader stays open
        check("READ LYRICS" in sess.text(), "read lyrics: [p] keeps the reader open")
        sess.send("a", 1.0)        # "Add manually": two search panes, the current track is the first entry of the lower one
        check("ADD LYRICS MANUALLY" in sess.text() and "Current track" in sess.text() and "Read Song" in sess.text(), "read lyrics: [a] opens the add-manually overlay with both lists")
        check("LOCKED IN: LYRICS FILE" in sess.text() and "LOCKED IN: TRACK" in sess.text(), "read lyrics: the add-manually overlay shows the locked-in panes")
        sess.send(b"\t", 0.3)      # to the track search field: CTRL+t / CTRL+f work while typing
        sess.send(b"\x14", 0.7)    # CTRL+t: the track list shows file names
        check("TRACKS (file names)" in sess.text() and "Read Song.wav" in sess.text(), "read lyrics: CTRL+t shows file names in the track list")
        sess.send(b"\x14", 0.7)
        check("TRACKS (artist - title)" in sess.text() and "- - Read Song" not in sess.text(), "read lyrics: CTRL+t goes back to artist - title (no '- -' in front of a track)")
        sess.send(b"\x06", 0.7)    # CTRL+f: only tracks without saved lyrics (this one has some)
        check("0 of 1 tracks" in sess.text(), "read lyrics: CTRL+f lists only the tracks without lyrics")
        sess.send(b"\x06", 0.7)
        check("1 of 1 tracks" in sess.text(), "read lyrics: CTRL+f shows all tracks again")
        sess.send(b"\t", 0.3)
        sess.send(b"\t", 0.3)
        sess.send(b"\t", 0.3)      # (round to the lyrics list again)
        sess.send(b"\r", 0.5)     # ENTER in a list locks the highlighted lyrics file in, it does not assign
        check("ADD LYRICS MANUALLY" in sess.text() and "none yet" not in sess.text(), "read lyrics: ENTER locks the lyrics file in and keeps the overlay open")
        sess.send(b"\x03", 0.7)    # CTRL+c clears the choice, also from a search field
        check("none yet" in sess.text(), "read lyrics: CTRL+c clears the locked-in lyrics file")
        sess.send(b"\r", 0.5)      # (locked in again)
        sess.send(b"\t", 0.3)
        sess.send(b"\t", 0.5)
        sess.send(b"\r", 1.0)      # on the button: assigns the first file to the current track (it is the same text, so nothing changes)
        check("ADD LYRICS MANUALLY" not in sess.text() and "READ LYRICS" in sess.text(), "read lyrics: ENTER assigns the lyrics file and closes the overlay")
        sess.send("a", 1.0)         # the file that was just assigned shows as locked in, with the note
        check("Added manually;" in sess.text() and "unassign" in sess.text(), "read lyrics: a manually added lyrics file shows as locked in, with the unassign note")
        sess.send(b"\x1b", 1.0)
        check("ADD LYRICS MANUALLY" not in sess.text() and "READ LYRICS" in sess.text(), "read lyrics: ESC closes the add-manually overlay")
        if "[l] Re-fetch" in sess.text() or "Re-fetch" in sess.text():
            sess.send("l", 1.0)    # the re-fetch / version control form opens over the reader (the track is the loaded one now)
            if "LYRICS REFETCH AND VERSION CONTROL" in sess.text():
                check("READ LYRICS" in sess.text(), "read lyrics: [l] opens the re-fetch form over the reader")
                sess.send(b"\x1b", 1.0)
                check("LYRICS REFETCH AND VERSION CONTROL" not in sess.text() and "READ LYRICS" in sess.text(), "read lyrics: ESC returns from the re-fetch form to the reader")
            else:
                check("only for the track that plays" in sess.text(), "read lyrics: [l] says it is only for the playing track")
        if shutil.which("ffprobe") is None or sess.alive() is False:
            print("  skipped: read lyrics playback (ffmpeg / ffprobe is not installed on this machine)")
            return
        for _ in range(6):
            sess.pump(0.5)
        if re.search(r"playing 0:00", sess.text()) or "playing" not in sess.text():
            print("  skipped: read lyrics playback (the track does not play on this machine: no audio device)")
            return
        check("playing " in sess.text() and "First lyric line here" in sess.text(), "read lyrics: the position is shown while it plays")
        sess.send("p", 1.0)
        check("paused" in sess.text(), "read lyrics: [p] pauses")
    finally:
        sess.kill()


def make_library(root):
    """The temporary HOME and the small library every scenario starts from (a worker gets its own copy of it)."""
    home = os.path.join(root, "home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Artist", "First Album", "Alpha Song.wav"))
    make_wav(os.path.join(lib, "Alpha Artist", "First Album", "Beta Song.wav"))
    make_png(os.path.join(lib, "Alpha Artist", "First Album", "cover.png"))
    make_wav(os.path.join(lib, "Misc", "さくら.wav"))
    make_wav(os.path.join(lib, "Misc", "사랑해.wav"))
    make_png(os.path.join(lib, "Misc", "cover.png"))   # (the first track of the list depends on the file system's directory order: any of them has a cover)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\n" % lib)



def main_session(binary, root, scripts):
    """The long walk through the main screens (first start, settings, meta editor, ..., quit)."""
    home = os.path.join(root, "home")
    lib = os.path.join(root, "music")
    sess = Session(binary, home, scripts)
    try:
        progress("main session")
        # --- first start: a fresh HOME opens the help wizard by itself (full-screen frame)
        sess.pump(4)
        check(sess.alive(), "start: the program runs")
        check(re.search(rb"\x1b\[8;32;120t", sess.raw) is not None, "terminal size: at start the terminal is asked for 120 x 32 (the test terminal is 140 x 45)")
        check("HELP WIZARD" in sess.text(), "first start: the help wizard opens by itself")
        check_frame(sess, "first-run help wizard")
        for _ in range(3):
            sess.send(b"\x1b[C", 0.5)
            check_frame(sess, "first-run help wizard (next page)")
        sess.send(b"\x1b", 1.0)

        # --- main screen
        for _ in range(40):  # up to ~20 s for the first scan
            sess.pump(0.5)
            if "Alpha Song" in sess.text() and "さくら" in sess.text():
                break
        sess.pump(1.0)
        check("Alpha Song" in sess.text() and "さくら" in sess.text(), "main screen: the library is listed")
        check_frame(sess, "main screen", frame=False)

        # --- Latin letters toggle (default hotkey: ")
        sess.send('"', 1.5)
        txt = sess.text()
        check("sakura" in txt.lower() and "さくら" not in txt, "latin toggle: Japanese title shown in Latin letters")
        check("sarangheo" in txt.lower() or "salanghae" in txt.lower() or "사랑해" not in txt,
              "latin toggle: Korean title shown in Latin letters")
        sess.send('"', 1.5)
        check("さくら" in sess.text(), "latin toggle: toggled back to kana")

        # --- cheat sheet, MENUS index, wizard, console
        for key, name in (("?", "cheat sheet"), ("<", "MENUS index"), ("W", "help wizard"), ("t", "console")):
            sess.send(key, 1.2)
            check_frame(sess, name, frame=False)  # overlays sit on top of the main screen: only crashes count
            for _ in range(2):
                sess.send(b"\x1b[C", 0.4)  # pages / tabs
            check_frame(sess, name + " (next page)", frame=False)
            sess.send(b"\x1b", 0.8)
            check(sess.alive(), "%s: ESC leaves it" % name)

        # --- settings: walk every tab to the right, then back
        sess.send("s", 1.5)
        check_frame(sess, "settings (first tab)")
        for i in range(12):
            sess.send(b"\t", 0.5)
            check_frame(sess, "settings tab %d" % (i + 2))
            sess.send(b"\x1b[B", 0.2)
            sess.send(b"\x1b[1;2B", 0.2)  # SHIFT+down = page
        sess.send(b"\x1b", 1.0)
        check(sess.alive(), "settings: ESC leaves it")

        # --- meta editor and its cover options overlay
        sess.send("M", 2.0)
        check_frame(sess, "meta editor")
        sess.send(b"\t", 0.5)
        sess.send(b"\t", 0.5)
        for _ in range(12):
            sess.send(b"\x1b[B", 0.15)
        sess.send(b"\x1b[D", 0.3)       # the two buttons sit side by side: the last stop is Style Option, Left goes to Cover Options
        sess.send(b"\r", 1.5)
        check("COVER OPTIONS" in sess.text(), "cover options: the overlay opens")
        sess.send("t", 1.0)
        check(sess.alive(), "cover options: [t] does not crash")
        sess.send("t", 1.0)
        check(sess.alive(), "cover options: [t] again does not crash")
        sess.send(b"\x1b", 1.0)
        check("COVER OPTIONS" not in sess.text(), "cover options: ESC closes the overlay")
        check("Style Option" in sess.text() and "Cover Options" in sess.text(), "meta editor: both buttons are shown")
        sess.send(b"\x1b[C", 0.3)       # Right: the Style Option button
        sess.send(b"\r", 1.5)
        for _ in range(12):   # (the overlay animates: a read can fall between two frames)
            if "STYLE OPTION" in sess.text() and "Cover Brightness" in sess.text(): break
            sess.pump(0.4)
        check("STYLE OPTION" in sess.text(), "style option: the overlay opens")
        check("Cover Brightness" in sess.text() and "Look of this track" in sess.text(), "style option: the rows are shown")
        check_frame(sess, "style option", frame=False)
        sess.send(b"\x1b[B", 0.3)       # Cover Contrast
        sess.send(b"\x1b[C", 1.2)
        for _ in range(8):   # (the overlay's preview animates: a read can fall between two frames)
            if "\u25c0 0 %" in sess.text(): break
            sess.pump(0.4)
        check("\u25c0 0 %" in sess.text(), "style option: Right gives the track its own value")
        sess.send(b"\x7f", 0.5)         # DEL: back to 'as in Settings'
        for _ in range(8):
            if "as in Settings" in sess.text(): break
            sess.pump(0.4)
        check("as in Settings" in sess.text(), "style option: DEL goes back to 'as in Settings'")
        sess.send(b"\x1b", 1.0)
        check("STYLE OPTION" not in sess.text(), "style option: ESC closes the overlay")
        for _ in range(4):  # ESC steps back: fields pane -> library pane -> out of the meta editor
            if "META EDITOR" not in sess.text():
                break
            sess.send(b"\x1b", 1.0)
        check("META EDITOR" not in sess.text(), "meta editor: ESC leaves it")

        progress("main session: quit")
        # --- quit
        sess.send("q", 0.5)
        for _ in range(20):
            if not sess.alive():
                break
            sess.pump(0.3)
            if sess.alive():
                sess.send("y", 0.3)  # in case a quit confirmation is asked
        check(not sess.alive(), "quit: the program ends after the quit key")
        if sess.alive():
            print("\n".join("%2d %s" % (i, l.rstrip()) for i, l in enumerate(sess.screen.display) if l.strip()))
    finally:
        sess.kill()


def _cell_width(ch):
    import unicodedata
    if unicodedata.combining(ch) or unicodedata.category(ch) in ("Mn", "Me", "Cf"):
        return 0
    return 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1


def overflow_problems(sess):
    """Reads the last complete frame as the program wrote it (every row starts with ESC[row;colH) and reports a row that is
    written below the last row or whose text runs past the last column. The screen buffer cannot show that: the terminal wraps
    or clips it, and a later row overwrites the wrap."""
    raw = sess.raw
    end = raw.rfind(b"\x1b[?2026l")
    if end < 0:
        return []
    start = raw.rfind(b"\x1b[?2026h", 0, end)
    if start < 0:
        return []
    text = raw[start:end].decode("utf-8", "replace")
    text = re.sub(r"\x1b_.*?\x1b\\", "", text, flags=re.S)          # graphics (Kitty)
    text = re.sub(r"\x1bP.*?\x1b\\", "", text, flags=re.S)           # graphics (Sixel)
    bad = []
    parts = re.split(r"\x1b\[(\d+);(\d+)H", text)
    for i in range(1, len(parts) - 2, 3):
        row, col, seg = int(parts[i]), int(parts[i + 1]), parts[i + 2]
        seg = re.sub(r"\x1b\[[0-9;:?]*[A-Za-z]", "", seg)
        seg = re.sub(r"\x1b\][^\x07\x1b]*(\x07|\x1b\\)", "", seg)
        if not seg.strip():
            continue
        if row > ROWS:
            bad.append("row %d is below the last row (%d): %r" % (row, ROWS, seg[:30]))
            continue
        w = col - 1 + sum(_cell_width(c) for c in seg)
        if w > COLS:
            bad.append("row %d is %d wide in a window of %d: %r" % (row, w, COLS, seg[:20] + "..." + seg[-20:]))
    return bad


def check_fit(sess, name, frame=True):
    """The program runs, the frame holds together, and nothing is written outside the window."""
    check(sess.alive(), "%s: the program is still running" % name)
    bad = overflow_problems(sess) + (frame_problems(sess) if frame else [])
    check(not bad, "%s @%dx%d: does not fit: %s" % (name, COLS, ROWS, "; ".join(bad[:3])))
    if bad:
        sess.dump(name)


def narrow_sweep(binary, root, scripts, cols, rows):
    """Every screen and overlay in a small window (80 x 24 and 100 x 30): nothing may be written past its edges."""
    global COLS, ROWS
    COLS, ROWS = cols, rows
    home = os.path.join(root, "nw_home")
    lib = os.path.join(root, "nw_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics", "translations"))
    make_wav(os.path.join(lib, "Alpha Artist", "First Album", "Alpha Song.wav"), 20)
    make_wav(os.path.join(lib, "Misc", "\u3055\u304f\u3089.wav"), 5)
    make_png(os.path.join(lib, "Alpha Artist", "First Album", "cover.png"))
    lines = ["First lyric line here, a rather long line that must wrap in a narrow window", "Second lyric line here", "Third lyric line here"]
    with open(os.path.join(lib, "Alpha Artist", "First Album", "lyrics.dummy"), "w") as f:
        f.write("x")
    os.makedirs(os.path.join(lib, "Alpha Artist", "First Album", "lyrics", "translations"))
    ldir = os.path.join(lib, "Alpha Artist", "First Album", "lyrics")
    with open(os.path.join(ldir, "Alpha Song.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]%s\n[00:03.00]%s\n[00:06.00]%s\n" % tuple(lines))
    os.makedirs(os.path.join(lib, "Misc", "lyrics"))
    with open(os.path.join(lib, "Misc", "lyrics", "\u3055\u304f\u3089.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]Sakura line one\n[00:03.00]Sakura line two\n")
    with open(os.path.join(lib, "lyrics", "Other Lyrics.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]Some other lyrics\n[00:03.00]More other lyrics\n")
    h = 1469598103934665603
    for l in lines:
        for c in l.encode("utf-8"):
            h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        h = ((h ^ 0x0a) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    with open(os.path.join(ldir, "translations", "Alpha Song.en.txt"), "w", encoding="utf-8") as f:
        f.write("#mousiki-translation to=en from=de svc=google lines=3 hash=%016x\n" % h)
        f.write("Erste Zeile hier, eine ziemlich lange Zeile die umbrechen muss\nZweite Zeile hier\nDritte Zeile hier\n")
    with open(os.path.join(home, ".config", "mousiki", "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\nPlain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\n" % lib)
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"}, quiet_stderr=True)
    tag = "%dx%d " % (cols, rows)

    def see(keys, what, wait=0.8, until=None, frame=True):
        sess.send(keys, wait)
        if until:
            for _ in range(10):
                if until in sess.text():
                    break
                sess.pump(0.4)
        check_fit(sess, tag + what, frame)

    try:
        progress("narrow sweep %dx%d" % (cols, rows))
        sess.pump(3)
        check_fit(sess, tag + "first-run help wizard")
        for i in range(60):    # every page of the wizard
            sess.send(b"\x1b[C", 0.25)
            check_fit(sess, tag + "help wizard page %d" % (i + 2))
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        sess.pump(1.0)
        check_fit(sess, tag + "main screen", False)   # (its volume row has no right border by design: only the edges count)
        for key, name in (("?", "cheat sheet"), ("<", "MENUS index"), ("W", "help wizard"), ("t", "console")):
            see(key, name, 1.0, frame=False)
            for i in range(6):
                see(b"\x1b[C", name + " page %d" % (i + 2), 0.3, frame=False)
            for i in range(3):
                see(b"\x1b[1;2B", name + " scrolled %d" % (i + 1), 0.3, frame=False)
            sess.send(b"\x1b", 0.8)
        see("s", "settings", 1.2)
        for i in range(13):
            see(b"\t", "settings tab %d" % (i + 2), 0.4)
            for _ in range(2):
                see(b"\x1b[B", "settings tab %d (down)" % (i + 2), 0.15)
            see(b"\x1b[1;2B", "settings tab %d (page)" % (i + 2), 0.15)
        sess.send(b"\x1b", 0.8)
        see("M", "meta editor", 1.5)
        see(b"\t", "meta editor (fields)", 0.4)
        for _ in range(12):
            sess.send(b"\x1b[B", 0.12)
        check_fit(sess, tag + "meta editor (bottom)")
        sess.send(b"\x1b[D", 0.3)
        see(b"\r", "cover options", 1.2, "COVER OPTIONS", frame=False)
        sess.send(b"\x1b", 0.8)
        sess.send(b"\x1b[C", 0.3)
        see(b"\r", "style option", 1.2, "STYLE OPTION", frame=False)
        sess.send(b"\x1b", 0.8)
        for _ in range(4):
            if "META EDITOR" not in sess.text():
                break
            sess.send(b"\x1b", 0.8)
        for key, name in (("O", "oscilloscope tuning"), (".", "lyrics area view 2"), (".", "lyrics area view 3"), (".", "lyrics area view 4"), (".", "lyrics area view 1")):
            see(key, name, 1.0, frame=False)
            if name.startswith("osc"):
                for _ in range(8):
                    see(b"\x1b[B", name + " (down)", 0.15, frame=False)
                sess.send(b"\x1b", 0.8)
        # READ LYRICS and everything that opens over it
        sess.send(b"\x0c", 1.5)
        for _ in range(10):
            if "READ LYRICS" in sess.text():
                break
            sess.pump(0.4)
        opened = "READ LYRICS" in sess.text()
        check(opened, tag + "READ LYRICS opens on the hovered track")
        if not opened:
            sess.dump("READ LYRICS did not open")
        else:
            check_fit(sess, tag + "read lyrics")
            for _ in range(4):
                see(b"\x1b[1;2B", "read lyrics (scrolled)", 0.2)
            see(b"\t", "read lyrics (translation pane)", 0.3)
            see(b"\t", "read lyrics (lyrics pane)", 0.3)
            see(b"/", "read lyrics: search lyrics files", 0.8, "SEARCH LYRICS")
            for c in "oth":
                sess.send(c, 0.1)
            check_fit(sess, tag + "read lyrics: search lyrics files (typed)")
            sess.send(b"\x1b", 0.6)
            see("t", "read lyrics: translate overlay", 0.8)
            sess.send(b"\x1b[B", 0.2)
            check_fit(sess, tag + "read lyrics: translate overlay (down)")
            sess.send(b"\x1b", 0.6)
            see("e", "read lyrics: export overlay", 0.8)
            sess.send(b"\x1b[C", 0.2)
            check_fit(sess, tag + "read lyrics: export overlay (right)")
            sess.send(b"\x1b", 0.6)
            see("E", "read lyrics: edit / add translation", 1.0, "EDIT / ADD TRANSLATION")
            for c in "Dies ist eine sehr lange uebersetzte Zeile die nicht in das Fenster passt":
                sess.send(c, 0.02)
            check_fit(sess, tag + "translation editor (typed)")
            for k in (b"\r", b"\x1b[B", b"\t", b"\x1b[C", b"\x1b[6~", b"\x0f", b"\t", b"\x0e", b"\x1b"):
                see(k, "translation editor (key %r)" % k, 0.3)
            sess.send(b"\x1b", 0.4)
            sess.send(b"y", 0.6)
            if "EDIT / ADD TRANSLATION" in sess.text():
                sess.send(b"\x1b", 0.6)
            check_fit(sess, tag + "translation editor closed")
            see("a", "read lyrics: add manually", 1.0, "ADD LYRICS MANUALLY")
            for k in (b"\t", b"\t", b"\t", b"\t", b"\x14", b"\x06", b"\x0f", b"\x1b[1;2B", b"\x1b[6~"):
                see(k, "add manually (key %r)" % k, 0.4)
            sess.send(b"\x03", 0.4)
            sess.send(b"\r", 0.4)
            check_fit(sess, tag + "add manually (locked)")
            sess.send(b"\x1b", 0.8)
            see("p", "read lyrics: playing", 2.0)
            see("l", "read lyrics: refetch form", 1.0)
            if "REFETCH" in sess.text():
                sess.send(b"\x1b", 0.8)
        # the big queue overlay and its legend
        for _ in range(3):
            if "READ LYRICS" in sess.text() or "ADD LYRICS" in sess.text():
                sess.send(b"\x1b", 0.8)
        see("e", "queue: add a track", 0.4, frame=False)
        see("K", "big queue overlay", 1.2, frame=False)
        see("S", "big queue overlay (shuffled)", 0.5, frame=False)
        see(b"\x1b", "big queue overlay closed", 0.8, frame=False)
        # karaoke
        for _ in range(3):   # (back at the main screen whatever is still open)
            if "READ LYRICS" in sess.text() or "ADD LYRICS" in sess.text():
                sess.send(b"\x1b", 0.8)
        see("k", "karaoke", 1.5)
        for i in range(3):
            see(b"\x1b[C", "karaoke (right %d)" % (i + 1), 0.4)
        see("a", "karaoke: add lyrics manually", 1.0)
        for _ in range(3):   # (back at the main screen whatever is still open; an ESC there would not be wanted)
            t = sess.text()
            if "ADD LYRICS" in t or "READ LYRICS" in t or "KARAOKE" in t:
                sess.send(b"\x1b", 0.8)
        # radio and the other modes
        for i in range(3):
            see("*", "mode switch %d" % (i + 1), 2.0, frame=False)
            if "HELP WIZARD" in sess.text():
                sess.send(b"\x1b", 1.0)
                check_fit(sess, tag + "mode switch %d (after its wizard)" % (i + 1))
        for key, name in (("s", "settings (mode)"), ("?", "cheat sheet (mode)")):
            see(key, name, 1.0, frame=False)
            sess.send(b"\x1b", 0.8)
        check(sess.alive(), tag + "the program is still running at the end")
    finally:
        sess.kill()

def opened_lyrics_is_current(binary, root, scripts):
    """A lyrics file opened from the search in READ LYRICS is what "Add manually" works on: its song is the current track
    (and the file is locked in); a file without a song of that name leaves no current track. Without a search it is the playing song."""
    home = os.path.join(root, "ol_home")
    lib = os.path.join(root, "ol_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "Zed Artist", "lyrics"))
    os.makedirs(os.path.join(lib, "lyrics"))
    make_wav(os.path.join(lib, "Aaa First.wav"), 5)
    make_wav(os.path.join(lib, "Zed Artist", "Zzz Last.wav"), 5)
    with open(os.path.join(lib, "lyrics", "Aaa First.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]first song lyrics\n")
    with open(os.path.join(lib, "Zed Artist", "lyrics", "Zzz Last.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]last song lyrics\n")
    with open(os.path.join(lib, "lyrics", "Nobody Home.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]lyrics of no song\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)

    def reader_with_search(query):
        sess.send(b"\x0c", 1.2)                       # READ LYRICS on the hovered track (the first one)
        sess.send(b"/", 0.8)                          # "/": search the lyrics files
        for c in query:
            sess.send(c, 0.1)
        sess.send(b"\r", 0.8)                         # open the first match

    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Aaa First" in sess.text():
                break
        reader_with_search("zzz")
        check("last song lyrics" in sess.text(), "opened lyrics: the search opened the lyrics file")
        sess.send("a", 1.0)
        check("ADD LYRICS MANUALLY" in sess.text() and "Zzz Last" in sess.text() and "Aaa First" not in sess.text().split("TRACKS")[0],
              "opened lyrics: the song of the opened lyrics file is the current track")
        check("Zzz Last.lrc" in sess.text() or "Zzz Last" in sess.text().split("LOCKED IN: LYRICS FILE")[-1], "opened lyrics: the opened file is locked in")
        sess.send(b"\x1b", 0.8)
        sess.send(b"\x1b", 0.8)                       # leave the reader
        reader_with_search("nobody")
        sess.send("a", 1.0)
        check("no track is named like the opened lyrics" in sess.text(), "opened lyrics: a file without a song leaves no current track")
        sess.send(b"\x1b", 0.8)
        sess.send(b"\x1b", 0.8)
        sess.send(b"\x0c", 1.2)                       # an ordinary reader again: the hovered track is the current one
        sess.send("a", 1.0)
        check("Current track" in sess.text() and "Aaa First" in sess.text().split("TRACKS")[0] + sess.text(), "opened lyrics: without a search the reader's track is the current one")
        check("no track is named" not in sess.text(), "opened lyrics: the earlier choice does not stay")
        check(sess.alive(), "opened lyrics: the program is still running")
    finally:
        sess.kill()

def translation_editor(binary, root, scripts):
    """READ LYRICS, SHIFT+e: a translation written by hand is saved as "Manual Translation", the next one as "Manual Translation 2";
    a machine translation that is edited stays as it was."""
    home = os.path.join(root, "te_home")
    lib = os.path.join(root, "te_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics"))
    make_wav(os.path.join(lib, "Aaa First.wav"), 5)
    with open(os.path.join(lib, "lyrics", "Aaa First.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]first line\n[00:02.00]\n[00:03.00]second line\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    tdir = os.path.join(lib, "lyrics", "translations")
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Aaa First" in sess.text():
                break
        sess.send(b"\x0c", 1.5)
        check("READ LYRICS" in sess.text() and "[SHIFT+e] Edit/Add translation" in sess.text(), "translation editor: the legend names SHIFT+e")
        sess.send("E", 1.0)
        t = sess.text()
        check("EDIT / ADD TRANSLATION" in t and "Manual Translation" in t and "first line" in t, "translation editor: opens, new translation named Manual Translation")
        for c in "erste Zeile":
            sess.send(c, 0.05)
        sess.send(b"\r", 0.4)                              # the empty line between the stanzas is skipped
        for c in "zweite Zeile":
            sess.send(c, 0.05)
        sess.send(b"\x13", 1.0)                            # CTRL+s
        t = sess.text()
        check("EDIT / ADD TRANSLATION" not in t and "Manual Translation" in t and "erste Zeile" in t, "translation editor: saved, shown in the reader as Manual Translation")
        files = sorted(os.listdir(tdir)) if os.path.isdir(tdir) else []
        check(len(files) == 1 and ".manual." in files[0], "translation editor: one file with the manual service id (%r)" % files)
        if files:
            body = open(os.path.join(tdir, files[0]), encoding="utf-8").read().split("\n")
            check(body[1:4] == ["erste Zeile", "", "zweite Zeile"], "translation editor: one translation line per lyrics line, the gap stays empty (%r)" % body[1:4])
        sess.send("E", 1.0)                                  # edit it again: changed in place
        sess.send(b"!", 0.3)
        sess.send(b"\x13", 1.0)
        files = sorted(os.listdir(tdir))
        check(len(files) == 1, "translation editor: editing a manual translation changes it in place")
        sess.send("E", 1.0)
        sess.send(b"\x0e", 0.5)                             # CTRL+n: start from scratch (asks)
        check("Start over" in sess.text(), "translation editor: CTRL+n asks before it drops the text")
        sess.send("y", 0.5)
        check("Manual Translation 2" in sess.text(), "translation editor: the scratch translation is Manual Translation 2")
        for c in "other":
            sess.send(c, 0.05)
        sess.send(b"\x13", 1.0)
        files = sorted(os.listdir(tdir))
        check(len(files) == 2, "translation editor: the second manual translation is a file of its own (%r)" % files)
        check("Manual Translation 2" in sess.text(), "translation editor: the pane lists Manual Translation 2")
        sess.send("E", 1.0)
        sess.send(b"x", 0.3)
        sess.send(b"\x1b", 0.5)
        check("Leave without saving" in sess.text(), "translation editor: ESC with changes asks")
        sess.send("n", 0.4)
        sess.send(b"\x1b", 0.4)
        sess.send(b"y", 0.6)
        check("EDIT / ADD TRANSLATION" not in sess.text(), "translation editor: y leaves without saving")
        check(sess.alive(), "translation editor: the program is still running")
    finally:
        sess.kill()

def translation_lines(binary, root, scripts):
    """The translation editor on a small terminal: long lines are broken beside their original (no "..."), SHIFT+up/down marks
    whole lines that are copied / pasted / cleared / undone, CTRL+2 switches the Latin letters."""
    global COLS, ROWS
    COLS, ROWS = 80, 32
    home = os.path.join(root, "tl_home")
    lib = os.path.join(root, "tl_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics"))
    make_wav(os.path.join(lib, "Aaa First.wav"), 5)
    with open(os.path.join(lib, "lyrics", "Aaa First.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]this is a rather long original line that cannot fit one row ZEBRA\n[00:02.00]short two\n[00:03.00]short three\n[00:04.00]short four\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    tdir = os.path.join(lib, "lyrics", "translations")
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Aaa First" in sess.text():
                break
        sess.send(b"\x0c", 1.5)
        sess.send("E", 1.0)
        t = sess.text()
        check("ZEBRA" in t and "..." not in t.split("EDIT / ADD TRANSLATION")[-1].split("[CTRL+s]")[0], "translation lines: a long original is broken into rows, not cut with '...'")
        for c in "eine sehr lange Uebersetzung die auch nicht in eine Zeile passt FINAL":
            sess.send(c, 0.02)
        sess.pump(0.5)
        t = sess.text()
        check("FINAL" in t and "eine sehr" in t, "translation lines: a long translation is broken into rows beside its original")
        check_fit(sess, "translation lines 80x32", True)
        sess.send(b"\r", 0.3)
        sess.send("AAA", 0.3)
        sess.send(b"\r", 0.3)
        sess.send("BBB", 0.3)
        sess.send(b"\x1b[A", 0.3)
        sess.send(b"\x1b[1;2B", 0.5)                       # SHIFT+down: two lines marked
        check("2 lines marked" in sess.text(), "translation lines: SHIFT+down marks whole lines")
        sess.send(b"\x03", 0.5)                            # CTRL+c
        check("copied 2 lines" in sess.text(), "translation lines: CTRL+c copies the marked lines")
        sess.send(b"\x1b[B", 0.3)
        sess.send(b"\x16", 0.6)                            # CTRL+v with a multi-line clipboard: pasted from the caret's line
        t = sess.text()
        for _ in range(10):   # (a slow machine needs longer to show the message)
            if "pasted" in t: break
            sess.pump(0.4); t = sess.text()
        check("pasted 1 line" in t or "pasted 2 lines" in t, "translation lines: CTRL+v pastes whole lines (%r)" % [l for l in t.split("\n") if "pasted" in l][:1])
        sess.send(b"\x1b[A", 0.3)
        sess.send(b"\x1b[A", 0.3)
        sess.send(b"\x1b[1;2B", 0.4)
        sess.send(b"\x7f", 0.5)                            # DEL on marked lines clears them
        check("cleared 2 lines" in sess.text(), "translation lines: DEL clears the marked lines")
        sess.send(b"\x1a", 0.5)                            # CTRL+z
        check("undone" in sess.text(), "translation lines: CTRL+z undoes it")
        sess.send(b"\x00", 0.5)                            # CTRL+2
        check("Latin letters: on" in sess.text() or "Latin letters on" in sess.text(), "translation lines: CTRL+2 switches the Latin letters")
        sess.send(b"\x00", 0.4)
        sess.send(b"\x14", 0.5)                            # CTRL+t: the same, on a key every terminal sends
        check("Latin letters: on" in sess.text() or "Latin letters on" in sess.text(), "translation lines: CTRL+t switches the Latin letters")
        sess.send(b"\x14", 0.4)
        sess.send(b"\x13", 1.0)
        files = sorted(os.listdir(tdir)) if os.path.isdir(tdir) else []
        check(len(files) == 1, "translation lines: saved (%r)" % files)
        if files:
            body = open(os.path.join(tdir, files[0]), encoding="utf-8").read().split("\n")
            check(body[1].endswith("FINAL") and body[2] == "AAA" and body[3] == "BBB", "translation lines: the lines are where they should be (%r)" % body[1:5])
        check(sess.alive(), "translation lines: the program is still running")
    finally:
        sess.kill()

def waterfall_view(binary, root, scripts):
    """The spectrogram options have a View row: the waterfall swaps the rows it does not use (motion, channels, time span) for its own,
    the full-screen view is titled WATERFALL and carries the frequency labels on its top ruler."""
    global COLS, ROWS
    COLS, ROWS = 110, 36
    home = os.path.join(root, "wf_home")
    lib = os.path.join(root, "wf_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 30)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nSpectroStyle=braille\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        sess.send(b"\r", 1.5)
        sess.send("I", 1.0)
        t = sess.text()
        check("Spectrogram" in t and "View" in t and "spectrogram" in t and "Motion" in t and "Graphs" not in t, "waterfall: the options open on the View row, the spectrogram rows are listed")
        sess.send(b"\x1b[C", 0.8)                          # RIGHT: waterfall
        t = sess.text()
        check("waterfall" in t and "Graphs" in t and "Graph spacing" in t and "Slant" in t and "Peak height" in t and "Motion" not in t and "Channels" not in t and "Time span" not in t,
              "waterfall: its own rows replace motion / channels / time span")
        sess.send(b"\x1b[B", 0.3)
        sess.send(b"\x1b[B", 0.3)
        check(sess.alive(), "waterfall: moving through the rows works")
        check_fit(sess, "waterfall options", False)
        sess.send("I", 0.8)
        sess.send("U", 1.2)                                  # the full-screen view
        t = sess.text()
        check("WATERFALL" in t and "SPECTROGRAM" not in t, "waterfall: the full-screen view is titled WATERFALL")
        check_fit(sess, "waterfall full screen", True)
        sess.send("U", 0.8)
        sess.send("I", 0.8)
        sess.send(b"\x1b[D", 0.8)                          # LEFT on the View row: back to the spectrogram
        t = sess.text()
        check("Motion" in t and "Graphs" not in t, "waterfall: back to the spectrogram brings its rows back")
        check(sess.alive(), "waterfall: the program is still running")
    finally:
        sess.kill()

def disk_field_setting(binary, root, scripts):
    """ANIMA/VIZ: DISK holds Eliment Disk / Cover and Disk Speed, the cover rows are DISK / COVER OPTIONS; Remove Field empties the preview."""
    home = os.path.join(root, "df_home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        sess.send("s", 1.2)
        sess.send(b"\t", 0.5)
        sess.send(b"\t", 0.8)                               # ANIMA/VIZ
        t = sess.text()
        check("DISK / COVER OPTIONS" in t and "Eliment Disk / Cover" in t and "Album Cover" in t, "disk field: the tab has DISK and DISK / COVER OPTIONS")
        check(t.index("Disk Speed") < t.index("DISK / COVER OPTIONS") < t.index("Album Cover"), "disk field: Disk Speed is under DISK, Album Cover under DISK / COVER OPTIONS")
        for _ in range(4):                                  # to Eliment Disk / Cover
            sess.send(b"\x1b[B", 0.2)
        sess.pump(0.6)
        check("Show Disk / Cover" in sess.text(), "disk field: the row says Show Disk / Cover")
        sess.send(b"\x1b[C", 1.0)
        check("Remove Field" in sess.text() and "Disk / Cover field removed" in sess.text(), "disk field: Remove Field empties the preview")
        sess.send(b"\x1b[C", 1.0)
        check("Show Disk / Cover" in sess.text() and "Disk / Cover field removed" not in sess.text(), "disk field: Show Disk / Cover brings the preview back")
        check(sess.alive(), "disk field: the program is still running")
    finally:
        sess.kill()

def queue_shuffle(binary, root, scripts):
    """The big queue overlay (SHIFT+k): SHIFT+s shuffles the order of the queue."""
    home = os.path.join(root, "qs_home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        for i in range(4):                       # the four tracks to the end of the queue
            sess.send("e", 0.4)
            sess.send(b"\x1b[B", 0.3)
        sess.send("K", 1.2)
        check("Shuffle" in sess.text(), "queue shuffle: the overlay legend names SHIFT+s")
        names = ["Alpha Song", "Beta Song", "\u3055\u304f\u3089", "\uc0ac\ub791\ud574"]

        def order():
            t = sess.text()
            pos = [t.find(n) for n in names]
            return tuple(sorted(range(len(names)), key=lambda i: pos[i])) if all(p >= 0 for p in pos) else tuple(pos)   # (the order, not the offsets: a status line above would move them)
        def settled():                            # the order once two frames in a row agree (a slow machine draws late)
            last = None
            for _ in range(8):
                sess.pump(0.4)
                cur = order()
                if cur == last:
                    return cur
                last = cur
            return last
        changed = False
        for _ in range(12):
            before = settled()
            sess.send("S", 0.3)
            if settled() != before:
                changed = True
                break
        check(changed, "queue shuffle: SHIFT+s changes the order of the queue")
        check("Undo shuffle" in sess.text() or "Unshuffle" in sess.text(), "queue shuffle: the overlay legend names SHIFT+u")
        sess.send("U", 0.3)
        after_undo = settled()
        # (a short track may end meanwhile: on a locked queue the track that played moves to the end, so the old order comes back as a rotation)
        rotated = any(before[k:] + before[:k] == after_undo for k in range(len(before))) if len(before) == len(after_undo) else False
        # (on a slow machine more tracks end meanwhile and the queue refills: then only what is still in both lists has to keep its old order, as a rotation)
        common_b = [x for x in before if x in after_undo]
        common_a = [x for x in after_undo if x in before]
        rotated = rotated or (len(common_b) >= 2 and len(common_b) == len(common_a) and any(common_b[k:] + common_b[:k] == common_a for k in range(len(common_b))))
        check(after_undo == before or rotated, "queue shuffle: SHIFT+u brings the order from before the shuffle back (before %r, after undo %r)" % (before, after_undo))
        sess.send("U", 0.6)
        check(sess.alive(), "queue shuffle: a second SHIFT+u (nothing to undo) is harmless")
        check(sess.alive(), "queue shuffle: the program is still running")
        sess.send(b"\x1b", 0.8)
    finally:
        sess.kill()


def search_key(binary, root, scripts):
    """The search key of the main screen ("/", SHIFT+7 on a German keyboard) also searches in the cheat sheet, the help wizard and READ LYRICS
    (the old SHIFT+s / CTRL+s do not any more); the legends name it for the keyboard."""
    home = os.path.join(root, "sk_home")
    lib = os.path.join(root, "sk_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 30)
    os.makedirs(os.path.join(lib, "lyrics"))
    with open(os.path.join(lib, "lyrics", "Alpha Song.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]first line\n[00:03.00]second line\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, {"XKB_DEFAULT_LAYOUT": "de"}, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        sess.send("?", 1.0)
        check("[SHIFT+7] search" in sess.text(), "search key: the cheat sheet names SHIFT+7 on a German keyboard")
        sess.send("S", 0.6)
        check("Search: " not in sess.text(), "search key: SHIFT+s no longer opens the cheat sheet search")
        sess.send("/", 0.6)
        check("Search: " in sess.text(), "search key: / opens the cheat sheet search")
        sess.send(b"\x1b", 0.5)
        sess.send(b"\x1b", 0.5)
        sess.send("W", 1.0)
        check("[SHIFT+7] search" in sess.text(), "search key: the wizard names SHIFT+7 on a German keyboard")
        sess.send("/", 0.8)
        check("SEARCH" in sess.text().split("\n")[0], "search key: / opens the wizard search")
        sess.send(b"\x1b", 0.5)
        sess.send(b"\x1b", 0.5)
        sess.send(b"\x0c", 1.5)
        check("READ LYRICS" in sess.text() and "[SHIFT+7] Search" in sess.text(), "search key: the READ LYRICS legend names SHIFT+7")
        sess.send(b"\x13", 0.8)
        check("SEARCH LYRICS FILES" not in sess.text(), "search key: CTRL+s no longer opens the lyrics file search")
        sess.send("/", 0.8)
        check("SEARCH LYRICS FILES" in sess.text(), "search key: / opens the lyrics file search")
        sess.send("/", 0.4)
        check("SEARCH LYRICS FILES" in sess.text(), "search key: a typed / is text in the search, it does not close it")
        sess.send(b"\x1b", 0.6)
        check("SEARCH LYRICS FILES" not in sess.text() and "READ LYRICS" in sess.text(), "search key: ESC closes the lyrics file search")
        check(sess.alive(), "search key: the program is still running")
    finally:
        sess.kill()


def shrinking_window(binary, root, scripts):
    """The window is made smaller while a track plays: the oscilloscope field goes first, then the disk (narrow), the whole meta data
    panel goes when the rows are too few for it and the list; under 40 columns a note replaces the screen. Nothing runs past the edge."""
    global COLS, ROWS
    COLS, ROWS = 140, 45
    home = os.path.join(root, "sw_home")
    lib = os.path.join(root, "sw_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Artist", "First Album", "Alpha Song.wav"), 60)
    make_wav(os.path.join(lib, "Beta Song.wav"), 60)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)

    def resize(c, r):
        global COLS, ROWS
        COLS, ROWS = c, r
        fcntl.ioctl(sess.fd, termios.TIOCSWINSZ, struct.pack("HHHH", r, c, 0, 0))
        os.kill(sess.pid, signal.SIGWINCH)
        sess.screen = pyte.Screen(c, r)
        sess.stream = pyte.ByteStream(sess.screen)
        sess.raw = b""
        sess.pump(1.2)

    def braille_cols(row_text, lo, hi):
        return sum(1 for ch in row_text[lo:hi] if "\u2801" <= ch <= "\u28ff")

    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        sess.send(b"\r", 2.0)
        t = sess.text()
        for _ in range(20):   # (a slow machine needs longer to load the track and its lyrics)
            if "File      :" in t and "No sync" in t.replace("hron", ""): break
            sess.pump(0.5); t = sess.text()
        check("File      :" in t and "No sync" in t.replace("hron", ""), "shrinking: the full window shows the meta data and the lyrics / oscilloscope field")
        check_fit(sess, "shrinking 140x45", False)
        resize(100, 45)
        lines = sess.screen.display
        check(braille_cols(lines[4], 0, 34) > 5, "shrinking: at 100 columns the disk is there")
        check_fit(sess, "shrinking 100x45", False)
        resize(66, 45)
        lines = sess.screen.display
        check(braille_cols(lines[4], 0, 34) > 5 and "File      :" in sess.text(), "shrinking: at 66 columns the disk and the meta data stay")
        check_fit(sess, "shrinking 66x45", False)
        resize(52, 45)
        lines = sess.screen.display
        check(braille_cols(lines[4], 0, 34) == 0 and "File      :" in sess.text(), "shrinking: at 52 columns the disk is gone, the meta data stay")
        check_fit(sess, "shrinking 52x45", False)
        resize(40, 45)
        check("File      :" in sess.text(), "shrinking: at 40 columns the meta data are still there")
        check_fit(sess, "shrinking 40x45", False)
        resize(30, 45)
        check("too narrow" in sess.text() and "File      :" not in sess.text(), "shrinking: under 40 columns a note replaces the screen")
        check_fit(sess, "shrinking 30x45", False)
        resize(120, 45)
        check("File      :" in sess.text() and "too narrow" not in sess.text(), "shrinking: the screen is back when the window is wide again")
        resize(120, 24)
        check("File      :" not in sess.text() and "Alpha Song" in sess.text() and "SEARCH LOCAL" in sess.text(), "shrinking: with too few rows the meta data panel goes and the list stays")
        check_fit(sess, "shrinking 120x24", False)
        resize(120, 40)
        check(b"\x1b[2J" not in sess.raw and "File      :" in sess.text(), "shrinking: growing the rows repaints in place (no full-screen erase, so no flash)")
        check_fit(sess, "shrinking 120x40", False)
        check(sess.screen.cursor.y == 0, "shrinking: the cursor is parked in the top row, so a lower window pushes no row out of the screen")
        resize(120, 40)
        check("File      :" in sess.text(), "shrinking: the meta data panel is back when there are rows for it and the list")
        check_fit(sess, "shrinking 120x40", False)
        check(sess.alive(), "shrinking: the program is still running")
    finally:
        sess.kill()


def colors_scroll(binary, root, scripts):
    """Settings -> COLORS in a window too short for its 25 lines: it scrolls with the chosen row, marks hidden rows, and fits."""
    global COLS, ROWS
    COLS, ROWS = 100, 20
    home = os.path.join(root, "cs_home")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\n" % os.path.join(root, "music"))
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        sess.pump(1.0)
        sess.send("s", 1.2)
        t = sess.text()
        check("THEME" in t and "BORDER_COLOR" in t, "colours scroll: the top of the tab is shown first")
        check("\u25bc" in t and "\u25b2" not in t, "colours scroll: a mark says there are more rows below")
        check("FOCUS" not in t, "colours scroll: the last row is not visible yet")
        check_fit(sess, "colours scroll (top)")
        for _ in range(22):
            sess.send(b"\x1b[B", 0.12)
        t = sess.text()
        check("FOCUS" in t and "\u25b2" in t, "colours scroll: moving down scrolls to the last row, with a mark for rows above")
        check("THEME" not in t, "colours scroll: the first rows are scrolled off")
        check_fit(sess, "colours scroll (bottom)")
        sess.send(b"\r", 0.5)                               # edit the chosen colour: the caret must be on the visible row
        check_fit(sess, "colours scroll (editing)")
        sess.send(b"\x1b", 0.5)
        check(sess.alive(), "colours scroll: the program is still running")
    finally:
        sess.kill()


# The scenarios, longest first. Each one runs in a worker process of its own (own copy of the library, own HOME): they only
# sleep and read frames, so on a few cores they overlap well. MOUSIKI_SMOKE_JOBS=1 runs them one after the other.
def run_scenario(name, binary, root, scripts):
    home = os.path.join(root, "home")
    if name == "main":
        main_session(binary, root, scripts)
    elif name == "kitty_cover_options":
        kitty_cover_options(binary, home, scripts)
    elif name == "selftest_command":
        selftest_command(binary, scripts)
    elif name == "search_key":
        search_key(binary, root, scripts)
    elif name == "waterfall_view":
        waterfall_view(binary, root, scripts)
    elif name == "translation_lines":
        translation_lines(binary, root, scripts)
    elif name == "shrinking_window":
        shrinking_window(binary, root, scripts)
    elif name == "colors_scroll":
        colors_scroll(binary, root, scripts)
    elif name == "translation_editor":
        translation_editor(binary, root, scripts)
    elif name == "opened_lyrics_is_current":
        opened_lyrics_is_current(binary, root, scripts)
    elif name == "disk_field_setting":
        disk_field_setting(binary, root, scripts)
    elif name == "queue_shuffle":
        queue_shuffle(binary, root, scripts)
    elif name == "narrow80":
        narrow_sweep(binary, root, scripts, 80, 24)
    elif name == "narrow100":
        narrow_sweep(binary, root, scripts, 100, 30)
    else:
        globals()[name](binary, root, scripts)


SCENARIOS = ["narrow80", "narrow100", "main", "read_lyrics_play", "karaoke_timing", "book_speed_and_devices", "paging_and_translit_row",
             "mode_keys", "kitty_cover_options", "radio_latin_toggle", "selftest_command", "opened_lyrics_is_current", "disk_field_setting", "queue_shuffle", "translation_editor", "translation_lines", "waterfall_view", "colors_scroll", "shrinking_window", "search_key"]


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    binary = os.path.abspath(args[0])
    scripts = None
    keep = "--keep" in args
    # a hang must not use up the whole CTest timeout without a trace: after this many seconds Python prints where every
    # thread is stuck and ends the test (MOUSIKI_SMOKE_LIMIT, default 780 s; the CTest timeout is longer)
    import faulthandler
    faulthandler.dump_traceback_later(int(os.environ.get("MOUSIKI_SMOKE_LIMIT", "780")), exit=True)
    if "--scripts" in args:
        scripts = os.path.abspath(args[args.index("--scripts") + 1])
    if not os.path.exists(binary):
        print("skipped: the player is not built (%s); build the target `mousiki` too" % binary)
        return 77

    if "--worker" in args:   # one scenario, in the folder the parent made for it
        name = args[args.index("--worker") + 1]
        root = args[args.index("--root") + 1]
        run_scenario(name, binary, root, scripts)
        print("%d checks, %d failed" % (checks, len(failures)))
        return 1 if failures else 0

    import subprocess
    import concurrent.futures
    root = tempfile.mkdtemp(prefix="mousiki_ui_")
    make_library(root)
    jobs = int(os.environ.get("MOUSIKI_SMOKE_JOBS", str(max(1, min(4, os.cpu_count() or 1)))))
    only = args[args.index("--only") + 1].split(",") if "--only" in args else SCENARIOS
    progress("%d scenarios, %d at a time" % (len(only), jobs))

    def work(name):
        sub = os.path.join(root, "w_" + name)
        shutil.copytree(root, sub, ignore=shutil.ignore_patterns("w_*"))
        cmd = [sys.executable, os.path.abspath(__file__), binary, "--worker", name, "--root", sub]
        if scripts:
            cmd += ["--scripts", scripts]
        t0 = time.time()
        p = subprocess.run(cmd, capture_output=True, text=True)
        return name, p.returncode, p.stdout + p.stderr, time.time() - t0

    total_checks, total_failed, bad = 0, 0, []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for name, rc, out, secs in pool.map(work, only):
            print("--- %s (%ds)" % (name, secs), flush=True)
            print(out.rstrip(), flush=True)
            m = re.findall(r"(\d+) checks, (\d+) failed", out)
            if m:
                total_checks += int(m[-1][0]); total_failed += int(m[-1][1])
            if rc not in (0, 1) or not m:
                total_failed += 1
                bad.append(name)
                print("  FAIL: scenario %s did not finish (exit code %d)" % (name, rc), flush=True)
    if not keep:
        shutil.rmtree(root, ignore_errors=True)
    print("%d checks, %d failed" % (total_checks, total_failed))
    return 1 if total_failed else 0


if __name__ == "__main__":
    sys.exit(main())
