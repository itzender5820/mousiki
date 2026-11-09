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
import subprocess
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
            try:
                os.chdir(home)   # (the program also reads/writes ./radio_config.txt: never the one of the project folder)
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
        sess.send(b"\x1b[A", 0.3)       # (the last stop is Video Option, under the two buttons: Up is Style Option, Left goes to Cover Options)
        sess.send(b"\x1b[D", 0.3)
        open_mark = len(sess.raw)
        sess.send(b"\r", 1.5)
        t0 = sess.text()
        check("COVER OPTIONS" in t0, "kitty cover options: the overlay opens")
        check("The disk shows" in t0, "kitty cover options: the disk modes are listed (the rest of the list may be overwritten by the picture's graphics data in this screen model)")
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
        mark = open_mark             # (the start is on "Automatic": its preview is the picture the search finds, here cover.png)
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


def radio_organigram(binary, root, scripts):
    """Radio mode: "<" shows the menu organigram (Main, its categories, the boxes of the open category); ENTER on Equalizer opens the equalizer."""
    home = os.path.join(root, "radioorg_home")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\nPlain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nStartMode=radio\n" % os.path.join(root, "music"))
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Plain Station" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        sess.send("<", 1.5)
        t = sess.text()
        check("Main" in t and "System" in t and "Sound" in t and "Stations and Presets" in t and "Visualizations" in t, "radio organigram: Main and its categories")
        check("Settings" in t and "Cheat Sheet" in t, "radio organigram: the first category is open")
        sess.send(b"\x1b[C", 0.8)
        check("Equalizer" in sess.text() and "Record" in sess.text(), "radio organigram: RIGHT opens the Sound category")
        sess.send(b"\x1b[B", 0.6)
        sess.send(b"\r", 1.2)
        check("Plain Station" in sess.text() or "EQUALIZER" in sess.text().upper(), "radio organigram: ENTER leaves the index")
        check("Visualizations" not in sess.text(), "radio organigram: the index is closed")
        check(sess.alive(), "radio organigram: the program is still running")
    finally:
        sess.kill()


def radio_waterfall(binary, root, scripts):
    """Radio mode: the spectrogram options (SHIFT+i) start with the View row; the waterfall shows its own rows and the full screen says WATERFALL."""
    home = os.path.join(root, "radiowf_home")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "stations.txt"), "w", encoding="utf-8") as f:
        f.write("# name | genre | country | codec | kbps | url | [*] favorite\n"
                "Plain Station | Rock | US | MP3 | 128 | http://127.0.0.1:9/c | *\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nStartMode=radio\n" % os.path.join(root, "music"))
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Plain Station" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        sess.send("I", 1.2)
        t = sess.text()
        check("Spectrogram" in t and "View" in t and "spectrogram" in t, "radio spectrogram options: the View row is there, set to spectrogram")
        check("Graphs" not in t, "radio spectrogram options: the waterfall's rows are hidden in the spectrogram view")
        sess.send(b"\x1b[C", 1.0)
        t = sess.text()
        check("waterfall" in t and "Graphs" in t, "radio spectrogram options: View -> waterfall shows the waterfall rows")
        sess.send("I", 1.0)                                  # closes the overlay
        sess.send("U", 1.5)
        check("WATERFALL" in sess.text(), "radio: the full screen is titled WATERFALL")
        sess.send(b"\x1b", 1.0)
        check(sess.alive(), "radio waterfall: the program is still running")
    finally:
        sess.kill()


def radio_station_edit(binary, root, scripts):
    """Radio mode, big STATIONS overlay (SHIFT+l): SHIFT+c edits every field of an entry, DEL deletes after a warning, SHIFT+u takes it back; the files follow."""
    home = os.path.join(root, "radioedit_home")
    cfgdir = os.path.join(home, ".config", "mousiki")
    os.makedirs(cfgdir)
    sfile = os.path.join(cfgdir, "stations.txt")
    with open(sfile, "w", encoding="utf-8") as f:
        f.write("# my stations\n# name | genre | country | codec | kbps | url | [*] favorite\n"
                "Alpha Radio | Rock | US | MP3 | 128 | http://127.0.0.1:9/a | *\n"
                "Bravo Radio | Jazz | DE | AAC | 64 | http://127.0.0.1:9/b\n"
                "Charlie Radio | Pop | FR | MP3 | 96 | http://127.0.0.1:9/c\n")
    with open(os.path.join(cfgdir, "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nStartMode=radio\n" % os.path.join(root, "music"))
    def file_text():
        with open(sfile, encoding="utf-8") as f:
            return f.read()
    sess = Session(binary, home, scripts, {"MOUSIKI_RADIO_NULL": "1"})
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "Alpha Radio" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        sess.send("L", 1.2)
        t = sess.text()
        check("[SHIFT+c] Edit entry" in t and "[DEL] Delete" in t and "[SHIFT+u] Undo" in t, "radio stations overlay: the legend shows Edit entry / Delete / Undo")
        sess.send(b"\x1b[B", 0.4)                           # Bravo
        sess.send("C", 1.0)
        t = sess.text()
        check("Edit entry" in t and "PRESET NAME:" in t and "URL:" in t and "KBPS:" in t, "radio stations overlay: SHIFT+c opens the entry editor with all fields")
        sess.send("X", 0.4)                                  # typed into NAME
        sess.send(b"\t", 0.3)
        sess.send("Bravo2", 0.4)                              # PRESET NAME
        sess.send(b"\r", 1.2)
        txt = file_text()
        check("Bravo RadioX" in txt and "# my stations" in txt, "radio stations overlay: the edited name is saved, the comment at the top stays")
        check("Bravo2" in sess.text(), "radio stations overlay: the preset name shows in the list")
        sess.send(b"\x1b[3~", 0.8)                           # DEL
        t = sess.text()
        check("Delete station" in t and "all presets" in t, "radio stations overlay: DEL asks first")
        sess.send("n", 0.6)
        check("Bravo RadioX" in file_text(), "radio stations overlay: N keeps the station")
        sess.send(b"\x1b[3~", 0.8)
        sess.send("y", 1.2)
        txt = file_text()
        check("Bravo" not in txt and "Alpha Radio" in txt and "Charlie Radio" in txt, "radio stations overlay: Y removes the station from stations.txt")
        sess.send("U", 1.2)
        txt = file_text()
        check("Bravo RadioX" in txt and txt.index("Alpha Radio") < txt.index("Bravo RadioX") < txt.index("Charlie Radio"), "radio stations overlay: SHIFT+u brings it back at its place")
        sess.send("U", 1.0)
        check("Bravo Radio |" in file_text() and "RadioX" not in file_text(), "radio stations overlay: SHIFT+u again takes the edit back")
        check(sess.alive(), "radio stations overlay: the program is still running")
    finally:
        sess.kill()


def scope_spectro_overlays(binary, root, scripts):
    """The full-screen spectrogram (SHIFT+u): the scale sits at the upper left, UP / DOWN switch spectrogram <-> waterfall, LEFT / RIGHT image <-> braille;
    the oscilloscope overlay (=): two centred legend lines, LEFT / RIGHT image <-> braille, UP / DOWN oscilloscope music on / off."""
    home = os.path.join(root, "sso_home")
    lib = os.path.join(root, "sso_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 30)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nHKeyScopeBig=\"=\"\nHKeySpectroFull=\"U\"\n" % lib)
    sess = Session(binary, home, scripts, {"MOUSIKI_NULL_AUDIO": "1"}, quiet_stderr=True)
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
        sess.send("U", 1.5)
        lines = sess.text().split("\n")
        top = lines[0] if lines else ""
        check("SPECTROGRAM" in sess.text(), "spectrogram full screen: opens")
        check(" Hz" in top and "SPECTROGRAM" in top and top.index("Hz") < top.index("SPECTROGRAM"), "spectrogram full screen: the scale is at the upper left of the frame (%s)" % top[:70].strip())
        bottom = [l for l in lines if "Options" in l and "Close" in l]
        check(bottom and "Image / Braille" in bottom[-1] and "Waterfall" in bottom[-1] or any("Image / Braille" in l for l in lines), "spectrogram full screen: the legend names the arrow keys")
        sess.send(b"\x1b[B", 1.2)
        check("WATERFALL" in sess.text(), "spectrogram full screen: DOWN switches to the waterfall")
        sess.send(b"\x1b[A", 1.2)
        check("SPECTROGRAM" in sess.text() and "WATERFALL" not in sess.text(), "spectrogram full screen: UP switches back")
        sess.send(b"\x1b", 1.0)
        sess.send("=", 1.5)
        t = sess.text()
        check("OSCILLOSCOPE" in t, "oscilloscope overlay: opens")
        check("[LEFT/RIGHT]" in t or "Image / Braille" in t, "oscilloscope overlay: the first legend line names the arrow keys")
        check("Grid color" in t or "Color" in t, "oscilloscope overlay: the second legend line is in the bottom border")
        sess.send(b"\x1b[B", 1.0)
        check("oscilloscope music mode: on" in sess.text() or sess.alive(), "oscilloscope overlay: DOWN turns the music mode on")
        sess.send(b"\x1b[A", 0.8)
        sess.send(b"\x1b", 0.8)
        check(sess.alive(), "scope / spectro overlays: the program is still running")
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
        for _ in range(11):
            sess.send(b"\x1b[B", 0.1)   # down to Translit. Kanji (past Reduce Motion When Paused and REMOTE / UPDATES; TEXT AND TRANSLITERATION is last)
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
        sess.send("t", 1.0)   # the console: [v] switches the verbose log on for this session
        check("CONSOLE / LOGS" in sess.text() and "Verbose log" in sess.text(), "console: the legend names [v] Verbose log for this session")
        sess.send("v", 1.0)
        check("(verbose)" in sess.text(), "console: [v] switches the verbose log on (shown in the title)")
        sess.send("v", 1.0)
        check("(verbose)" not in sess.text(), "console: [v] again switches it off")
        sess.send(b"\x1b", 1.0)
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
        sess.send("Y", 1.2)                    # SHIFT+b is the bookmark in the audio book: the spectrum bars overlay has SHIFT+y there
        t = sess.text()
        check("SPECTRUM BARS" in t.upper() and "Style" in t, "audio book: SHIFT+y opens the spectrum bars overlay (%s)" % [l for l in t.split("\n") if "BARS" in l.upper()][:2])
        sess.send(b"\x1b", 0.8)
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
        sess.send(b"\x1b[A", 0.3)       # the last stop is Video Option, under the two buttons: Up is Style Option
        sess.send(b"\x1b[D", 0.3)       # the two buttons sit side by side: Left goes to Cover Options
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
        check("READ LYRICS" in sess.text() and "[SHIFT+e] Edit/Add" in sess.text(), "translation editor: the legend names SHIFT+e")
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

def disk_border_setting(binary, root, scripts):
    """Settings -> DISK -> Disk Border: with ElimentDiskBorder=false the ":  :" column between the disk and the meta data is gone and the
    disk sits further right (in the middle of the space); with the default it is there."""
    cols = {}
    for name, val in (("on", "true"), ("off", "false")):
        home = os.path.join(root, "db_home_" + name)
        lib = os.path.join(root, "music")
        os.makedirs(os.path.join(home, ".config", "mousiki"))
        with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
            f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nElimentDiskBorder=%s\n" % (lib, val))
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
            t = sess.text()
            check((":  :" in t) == (name == "on"), "disk border %s: the \":  :\" column is %s" % (name, "there" if name == "on" else "gone"))
            first = None
            for y in range(1, 12):                           # the first braille column of the disk, left of the meta data
                row = sess.row_cells(y)
                for x, ch in enumerate(row):
                    if "\u2800" <= ch <= "\u28ff" and ch != "\u2800":
                        first = x if first is None else min(first, x)
                        break
            cols[name] = first
        finally:
            sess.kill()
    check(cols.get("on") is not None and cols.get("off") is not None and cols["off"] >= cols["on"], "disk border: with the border off the disk is not further left (on %s, off %s)" % (cols.get("on"), cols.get("off")))


def play_mode_back(binary, root, scripts):
    """m steps through the play modes, j steps back (the console log names the mode)."""
    home = os.path.join(root, "pmb_home")
    lib = os.path.join(root, "pmb_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(lib)
    make_wav(os.path.join(lib, "Aaa First.wav"), 5)
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
            if "Aaa First" in sess.text():
                break
        sess.send("m", 0.8)                                  # list -> repeat
        sess.send("j", 0.8)                                  # back to list
        sess.send("j", 0.8)                                  # list -> queue then stop
        sess.send("t", 1.5)
        t = sess.text()
        check("play mode: repeat" in t and "play mode: list" in t and "play mode: queue then stop" in t, "play mode back: m goes forward, j back (repeat, list, queue then stop)")
        check(sess.alive(), "play mode back: the program is still running")
    finally:
        sess.kill()

def translation_create(binary, root, scripts):
    """READ LYRICS: the lyrics files and the translations are two panes side by side; SHIFT+c creates an empty translation file (Manual
    Translation, then Manual Translation 2); LEFT / RIGHT go through the entries of the focused pane and round the ends."""
    home = os.path.join(root, "tc_home")
    lib = os.path.join(root, "tc_music")
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
        lines = sess.text().split("\n")
        check(any("READ LYRICS" in l and "TRANSLATION" in l for l in lines), "translation create: the lyrics pane and the translation pane are side by side")
        check("[SHIFT+c] Create" in sess.text(), "translation create: the legend names SHIFT+c")
        sess.send("C", 1.0)
        t = sess.text()
        files = sorted(os.listdir(tdir)) if os.path.isdir(tdir) else []
        check(len(files) == 1 and ".manual." in files[0], "translation create: one file with the manual service id (%r)" % files)
        if files:
            body = open(os.path.join(tdir, files[0]), encoding="utf-8").read().split("\n")
            check(body[0].startswith("#mousiki-translation ") and "lines=3" in body[0] and body[1:4] == ["", "", ""], "translation create: the file is empty, one empty line per lyrics line (%r)" % body[:4])
        check("Manual Translation" in t, "translation create: the pane lists it")
        sess.send("C", 1.0)
        files = sorted(os.listdir(tdir))
        check(len(files) == 2 and "Manual Translation 2" in sess.text(), "translation create: the second one is Manual Translation 2 (%r)" % files)
        sess.send(b"\x1b[C", 0.8)                           # RIGHT in the translation pane: round the end to the first
        check("\u25cf Manual Translation" in sess.text(), "translation create: RIGHT goes to the next translation (and round)")
        check(sess.alive(), "translation create: the program is still running")
    finally:
        sess.kill()

def translation_paste_undo(binary, root, scripts):
    """A paste by the terminal (bracketed paste) is one event: with no text field it does nothing (no hotkeys fire, no help wizard), in the
    translation editor its lines go into the rows; CTRL+z / CTRL+y take steps back (up to 10), CTRL+r brings them forward."""
    home = os.path.join(root, "tpu_home")
    lib = os.path.join(root, "tpu_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics"))
    make_wav(os.path.join(lib, "Aaa First.wav"), 5)
    with open(os.path.join(lib, "lyrics", "Aaa First.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.50]one\n[00:02.00]two\n[00:03.00]three\n")
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
            if "Aaa First" in sess.text():
                break
        sess.send(b"\x1b[200~hhh???wwww\x1b[201~", 1.0)      # pasted on the main screen: no text field, so nothing happens
        check("HELP WIZARD" not in sess.text() and "Aaa First" in sess.text(), "paste: on the main screen it fires no hotkeys")
        sess.send(b"\x0c", 1.5)
        sess.send("E", 1.0)
        check("EDIT / ADD TRANSLATION" in sess.text(), "paste: the translation editor is open")
        sess.send(b"\x1b[200~The person I passed by\rWalks politely on the white line,\rLike a stray cat.\x1b[201~", 1.2)
        t = sess.text()
        check("HELP WIZARD" not in t and "pasted 3 lines" in t and "stray cat" in t and "white line" in t, "paste: three lines go into the three rows, no help wizard")
        for _ in range(3):
            sess.send(b"\x1b[A", 0.2)                       # up to the first row
        sess.send(b"\x1b[1;2B", 0.5)                        # SHIFT+down: rows 1 and 2 marked
        sess.send(b"\x1b[200~AAAA\rBBBB\x1b[201~", 1.0)
        t = sess.text()
        check("AAAA" in t and "BBBB" in t and "person I passed" not in t and "white line" not in t and "stray cat" in t, "paste: marked lines are replaced by the pasted lines")
        sess.send(b"\x1a", 0.8)                             # CTRL+z (the marked-lines paste)
        sess.send(b"\x1a", 0.8)                             # CTRL+z
        t = sess.text()
        check("undone" in t and "stray cat" not in t, "undo: CTRL+z takes the paste back")
        sess.send(b"\x12", 0.8)                             # CTRL+r
        t = sess.text()
        check("redone" in t and "stray cat" in t, "undo: CTRL+r brings it forward again")
        sess.send(b"\x1a", 0.6)
        for c in "xyz":
            sess.send(c, 0.1)
        sess.send(b"\x19", 0.8)                             # CTRL+y (the undo key of a QWERTZ keyboard)
        t = sess.text()
        check("xyz" not in t.split("TRANSLATION")[-1] and "undone" in t, "undo: CTRL+y takes a typed run back as one step")
        for i in range(12):                                  # more than 10 steps: only the last 10 are kept
            sess.send("a", 0.05)
            sess.send(b"\x1b[D", 0.05)
            sess.send(b"\x7f", 0.05)
        for i in range(14):
            sess.send(b"\x1a", 0.1)
        sess.send(b"\x1a", 0.6)
        check("nothing to undo" in sess.text(), "undo: after the last step it says nothing is left to undo")
        check(sess.alive(), "paste / undo: the program is still running")
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
        if not ("pasted 1 line" in t or "pasted 2 lines" in t): print("  screen at the failure:\n" + "\n".join("    | " + l.rstrip() for l in t.split("\n") if l.strip()), flush=True)
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
        for _ in range(18):                                 # to Eliment Disk (after Lyrics/Viz Default: When No Lyrics Show, When No Video Shows, Lyrics Alignment, Lyrics Animation, Include Lyrics, Video Size, Effect, Contrast, Colors, Chromatic Aberration, Rounded Corners, Show Clap Loading, Disk/Cover/Thumbnail, Resolution, Buffer, yt-dlp mp3 from mp4)
            sess.send(b"\x1b[B", 0.2)
        sess.pump(0.6)
        check("Show Disk / Cover" in sess.text(), "disk field: the row says Show Disk / Cover")
        sess.send(b"\x1b[C", 1.0)
        check("Remove Field" in sess.text() and "Disk / Cover field removed" in sess.text(), "disk field: Remove Field empties the preview")
        sess.send(b"\x1b[C", 1.0)
        check("Show Disk / Cover" in sess.text() and "Disk / Cover field removed" not in sess.text(), "disk field: Show Disk / Cover brings the preview back")
        check(sess.alive(), "disk field: the program is still running")
        for _ in range(5):                                  # up to Show Clap Loading (yt-dlp mp3 from mp4, Video Buffer, Video Resolution and Disk/Cover/Thumbnail are in between)
            sess.send(b"\x1b[A", 0.2)
        sess.pump(0.8)
        t = sess.text()
        check("Show Clap Loading" in t and "loading: clap" in t, "show clap loading: the preview shows the clap (the default)")
        sess.send(b"\x1b[C", 0.8)
        t = sess.text()
        check("loading: none" in t, "show clap loading: right arrow goes on to none")
        sess.send(b"\x1b[D", 0.4)
        sess.send(b"\x1b[D", 0.8)
        t = sess.text()
        check("loading: text note" in t and "video: starting" in t, "show clap loading: the text note shows the message")
    finally:
        sess.kill()

def video_files_tab(binary, root, scripts):
    """FETCH VIDEOS, second tab VIDEO FILES: the videos are listed, the coupled one is marked and names its song, DEL (twice) deletes the file."""
    import subprocess
    if shutil.which("ffmpeg") is None:
        print("  skipped: video files tab (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "vft_home")
    lib = os.path.join(root, "vft_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 5)
    for n in ("Alpha Song", "Lonely Clip"):
        r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=64x48:rate=5:duration=2", "-f", "lavfi", "-i", "sine=frequency=440:duration=2", "-shortest", "-pix_fmt", "yuv420p", os.path.join(lib, n + ".mp4")], capture_output=True)
        if r.returncode != 0:
            print("  (skipped: ffmpeg could not make the test clip)")
            return
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nVideoMode=true\nHKeyVideoFetch=ALT+V\n" % lib)   # (ALT+V is the default on Linux / Windows only: on macOS it is SHIFT+d)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if "Alpha Song" in sess.text():
                break
        sess.send(b"\x1bv", 1.5)
        check("VIDEO FILES" in sess.text(), "fetch videos: the VIDEO FILES tab is there")
        sess.send(b"\x1b[1;3C", 1.0)
        check("[c] Alpha Song.mp4" in sess.text() and "[ ] Lonely Clip.mp4" in sess.text(), "video files: the coupled video is marked, the other one is video only")
        check("Alpha Song.wav" in sess.text(), "video files: the coupled song is named")
        sess.send(b"\x1b[B", 0.6)                            # to Lonely Clip.mp4 (video only)
        check("[CTRL+3 / m] makes an mp3 song" in sess.text(), "video files: a video only file offers the mp3 command above the note about the deletion")
        sess.send("m", 4.0)
        mp3s = [f for r_, d_, fs_ in os.walk(home) for f in fs_ if f.endswith(".mp3")]
        check(mp3s == ["Lonely Clip.mp3"] and "mp3 saved" in sess.text(), "video files: [m] makes the mp3 of a video only file in the download folder")
        sess.send(b"\x1b[A", 0.6)
        check("[CTRL+3 / m] makes an mp3 song" not in sess.text(), "video files: a coupled video does not offer it")
        sess.send(b"\x1b[3~", 0.8)
        check("DELETE Alpha Song.mp4" in sess.text() and os.path.exists(os.path.join(lib, "Alpha Song.mp4")), "video files: DEL asks first, nothing is deleted yet")
        sess.send(b"\x1b[3~", 1.5)
        check(not os.path.exists(os.path.join(lib, "Alpha Song.mp4")) and os.path.exists(os.path.join(lib, "Lonely Clip.mp4")), "video files: the second DEL deletes that video file only")
        sess.send("u", 1.5)
        check(os.path.exists(os.path.join(lib, "Alpha Song.mp4")) and "restored" in sess.text(), "video files: [u] brings the deleted video back")
        sess.send("u", 0.8)
        check("nothing to undo" in sess.text(), "video files: a second [u] has nothing to undo")
        sess.send(b"\x06", 0.8); sess.send(b"\x06", 0.8); sess.send(b"\x06", 0.8)
        check("newest first" in sess.text(), "video files: CTRL+f reaches the 'newest first' filter")
        sess.send(b"\x1b", 1.0)
        check(sess.alive(), "video files: ESC leaves")
    finally:
        sess.kill()

def settings_video_preview(binary, root, scripts):
    """ANIMA/VIZ: the Use Video row previews the sample video (scripts/preview_video.mp4), looping; a row further down stops it."""
    if shutil.which("ffmpeg") is None or shutil.which("ffprobe") is None:
        print("  skipped: settings video preview (ffmpeg / ffprobe is not installed on this machine)")
        return
    if not os.path.exists(os.path.join(scripts, "preview_video.mp4")):
        print("  skipped: settings video preview (scripts/preview_video.mp4 is not there)")
        return
    home = os.path.join(root, "sv_home")
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
        found = False
        for _ in range(60):                                 # down to the Use Video row (the ON/OFF block further down the tab)
            sess.send(b"\x1b[B", 0.15)
            sess.pump(0.4)
            if "Preview: Use Video" in sess.text():
                sess.pump(1.5)
            if "sample video" in sess.text():
                found = True
                break
        check(found, "settings video preview: the Use Video row shows the sample video")
        for _ in range(12):                                  # (Video Size, Effect, Contrast, Brightness, Colors, Chromatic Aberration, Resolution, Buffer and yt-dlp mp3 from mp4 show it too: on to the next row with another preview)
            sess.send(b"\x1b[B", 0.3)
            sess.pump(1.0)
            if "sample video" not in sess.text():
                break
        check("sample video" not in sess.text(), "settings video preview: another row shows its own preview")
        check(sess.alive(), "settings video preview: the program is still running")
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
        names = ["Alpha Song", "Beta Song"]   # (the Japanese / Korean names are left out: a terminal without wide-character support does not show them, and the order would be offsets)

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
        for _ in range(16):                       # a slow machine draws the overlay late: wait until both names are on the screen
            if all(n in sess.text() for n in names):
                break
            sess.pump(0.5)
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


def video_overlays(binary, root, scripts):
    """The & overlay (VIDEO), VIDEO OPTION, SEARCH YT and the [ Video Option ] button of the meta editor open, show their rows and close."""
    home = os.path.join(root, "vo_home")
    lib = os.path.join(root, "music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nHKeyVideo=Y\nHKeyVideoFetch=ALT+V\n" % lib)   # (ALT+V is the default on Linux / Windows only: on macOS it is SHIFT+d)
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
        sess.send("Y", 1.0)
        t = sess.text()
        check("Include Lyrics" in t and "Disk/Cover/Thumbnail" in t and "Manually Couple Local Files" in t and "Quick Search/Down. YT" in t and "General Fetch Videos Menu" in t and "Delete Current Video" in t and "Refetch this Video" in t, "video overlay: the rows are listed")
        check("Picture Size" in t and "Effect" in t and "Full Screen Video" in t, "video overlay: picture size, effect and full screen video rows are listed")
        check("Contrast" in t and "Colors" in t and "Chromatic Aberration" in t and "Resolution" in t and "Buffer" in t, "video overlay: contrast, colors, chromatic aberration, resolution and buffer rows")
        for _ in range(15):   # (to "Manually Couple Local Files", under "Full Screen Video"; Show Clap Loading is the row before it)
            sess.send(b"\x1b[B", 0.2)
        sess.send("\r", 1.0)
        t = sess.text()
        check("VIDEO OPTION" in t and "SEARCH LOCAL SONGS" in t and "SEARCH LOCAL VIDEOS" in t and "Connect song and video" in t and "Delete COMPLETE VIDEO FILE connected to track" in t, "video option: the two search panes and the buttons are drawn")
        check("all audio files" in t and "[video]" not in t, "video option: the song list shows audio files only, all of them at first")
        sess.send(b"\x06", 0.6)                            # CTRL+f: only the songs without a video yet
        check("VIDEO OPTION" in sess.text() and "no video yet" in sess.text(), "video option: CTRL+f switches to 'no video yet'")
        sess.send(b"\x06", 0.6)
        sess.send("a", 0.4)
        check("VIDEO OPTION" in sess.text(), "video option: typing goes into the search field")
        sess.send(b"\x03", 0.8)                            # CTRL+c clears the choice (it must not close the program)
        check(sess.alive() and "VIDEO OPTION" in sess.text(), "video option: CTRL+c does not quit the program")
        sess.send(b"\x1bv", 1.5)                           # ALT+v: FETCH VIDEOS for the song
        check("FETCH VIDEOS" in sess.text() and "VIDEO OPTION" not in sess.text(), "video option: ALT+v switches to the fetch videos overlay")
        sess.send(b"\x1b", 0.8)
        check("VIDEO OPTION" not in sess.text() and "FETCH VIDEOS" not in sess.text(), "video option: ESC closes it")
        sess.send("Y", 0.8)
        sess.send(b"\x1b[B", 0.2)                          # (the cursor stays on the row that was used last: VIDEO OPTION -> SEARCH YT)
        sess.send("\r", 1.5)
        t = sess.text()
        check("SEARCH YT" in t and "video_down" in t, "search yt: the panel names the download folder")
        sess.send(b"\x1b", 0.8)
        check("SEARCH YT" not in sess.text(), "search yt: ESC closes it")
        sess.send(b"\x1bv", 1.5)
        t = sess.text()
        check("FETCH VIDEOS" in t and "VIDEOS FOR THE SONG" in t and "Alpha Song" in t, "fetch videos: ALT+v opens the full-screen overlay with the songs and the videos pane")
        sess.send(b"\x1b", 0.8)
        check("FETCH VIDEOS" not in sess.text(), "fetch videos: ESC closes it")
        sess.send("M", 1.5)
        check("Video Option" in sess.text(), "meta editor: the [ Video Option ] button is there")
        sess.send(b"\x1b", 0.8)
        check(sess.alive(), "video overlays: the program is still running")
    finally:
        sess.kill()


def video_fetch_flow(binary, root, scripts):
    """FETCH VIDEOS with a fake yt-dlp: the videos of the song under the cursor are searched by themselves, two hits demand a choice,
    only the chosen video is downloaded."""
    import subprocess, stat
    if shutil.which("ffmpeg") is None:
        print("  skipped: fetch videos flow (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "vff_home")
    lib = os.path.join(root, "vff_music")
    fake = os.path.join(root, "vff_bin")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(fake)
    make_wav(os.path.join(lib, "Alpha Song.wav"), 5)
    clip = os.path.join(root, "vff_clip.mp4")
    r = subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc=size=64x48:rate=5:duration=2", "-pix_fmt", "yuv420p", clip], capture_output=True)
    if r.returncode != 0:
        print("  (skipped: ffmpeg could not make the test clip)")
        return
    yt = os.path.join(fake, "yt-dlp")
    with open(yt, "w") as f:
        f.write("""#!/bin/bash
if [[ "$*" == *"ytsearch"* ]]; then
  echo '{"id":"AAAAAAAAAAA","title":"Alpha Song (Official Video)","uploader":"Chan One","duration":212}'
  echo '{"id":"BBBBBBBBBBB","title":"Alpha Song live","channel":"Chan Two","duration":301}'
  exit 0
fi
out=""
while [ $# -gt 0 ]; do if [ "$1" = "-o" ]; then out="$2"; fi; shift; done
f="${out/\\%(ext)s/mp4}"
echo "download:PCT 50.0%"
cp "@CLIP@" "$f"
echo "FILE:$f"
""".replace("@CLIP@", clip))
    os.chmod(yt, os.stat(yt).st_mode | stat.S_IEXEC)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nHKeyVideoFetch=ALT+V\n" % lib)   # (ALT+V is the default on Linux / Windows only: on macOS it is SHIFT+d)
    sess = Session(binary, home, scripts, {"PATH": fake + os.pathsep + os.environ.get("PATH", "")}, quiet_stderr=True)
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
        sess.send(b"\x1bv", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Alpha Song live" in sess.text():
                break
        t = sess.text()
        check("Alpha Song (Official Video)" in t and "Alpha Song live" in t, "fetch videos: the videos of the song under the cursor are searched by themselves")
        check("[?]" in t, "fetch videos: several videos -> the song waits for a choice")
        sess.send("F", 1.0)
        check("a choice" in sess.text() or "nothing chosen" in sess.text(), "fetch videos: nothing is downloaded without a choice")
        sess.send("\r", 0.6)      # into the videos
        sess.send(b"\x1b[B", 0.3)  # the second one
        sess.send("\r", 0.8)      # choose it
        check("[x]" in sess.text(), "fetch videos: the choice is marked")
        t = sess.text()
        check("Download only current selected" in t and "Download all chosen (1)" in t, "fetch videos: the legend has SHIFT+a (only the current one) and SHIFT+f with the number of choices")
        state = os.path.join(home, ".config", "mousiki", "video_fetch_state.json")
        check(os.path.exists(state) and "BBBBBBBBBBB" in open(state, encoding="utf-8").read(), "fetch videos: the choice is saved at once (video_fetch_state.json)")
        sess.send(b"\x06", 0.6)    # CTRL+f: all songs
        sess.send(b"\x06", 0.6)    # songs with a video (none)
        sess.send(b"\x06", 0.8)    # only the chosen ones
        check("SONGS, chosen (1)" in sess.text() and "Alpha Song" in sess.text(), "fetch videos: CTRL+f has a stop that lists only the songs with a chosen video")
        sess.send(b"\x06", 0.8)    # back to missing video
        sess.send("A", 0.5)         # SHIFT+a: only the song under the cursor
        for _ in range(30):
            sess.pump(0.5)
            if "downloaded and paired" in sess.text():
                break
        check("1 of 1 chosen videos downloaded and paired" in sess.text(), "fetch videos: SHIFT+a downloads the chosen video of the song under the cursor")
        links = os.path.join(home, ".config", "mousiki", "video_links.txt")
        ok = os.path.exists(links) and "Alpha Song" in open(links, encoding="utf-8").read()
        check(ok, "fetch videos: the pairing is saved")
    finally:
        sess.kill()


def video_fetch_restore(binary, root, scripts):
    """FETCH VIDEOS keeps the found videos and the choices (video_fetch_state.json): a song searched and chosen the last time shows [x] again, and
    the filter CTRL+f can list the chosen ones only."""
    home = os.path.join(root, "vfr_home")
    lib = os.path.join(root, "vfr_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Beta Song.wav"), 5)
    make_wav(os.path.join(lib, "Gamma Song.wav"), 5)
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nHKeyVideoFetch=ALT+V\n" % lib)   # (ALT+V is the default on Linux / Windows only: on macOS it is SHIFT+d)
    state = {"songs": [{"path": os.path.join(lib, "Beta Song.wav"), "choice": 1, "seconds": 5,
                        "cands": [{"id": "CCCCCCCCCCC", "title": "Beta one", "uploader": "U1", "dur": 10},
                                  {"id": "DDDDDDDDDDD", "title": "Beta two", "uploader": "U2", "dur": 11}]}]}
    import json
    with open(os.path.join(home, ".config", "mousiki", "video_fetch_state.json"), "w", encoding="utf-8") as f:
        json.dump(state, f)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(20):
            sess.pump(0.5)
            if "Beta Song" in sess.text():
                break
        sess.send(b"\x1bv", 1.5)
        t = sess.text()
        check("1 chosen" in t and "[x] Beta Song" in t.replace("  ", " "), "fetch videos: the choice of the last time is there again")
        sess.send(b"\x06", 0.6)
        sess.send(b"\x06", 0.6)
        sess.send(b"\x06", 0.8)
        t = sess.text()
        check("SONGS, chosen (1)" in t and "Beta Song" in t and "Gamma Song" not in t, "fetch videos: the filter lists only the chosen song")
        sess.send("c", 0.8)
        check("SONGS, chosen (0)" in sess.text() and "no video is chosen yet" in sess.text(), "fetch videos: clearing the choice takes the song out of the chosen list")
        sess.send(b"\x1b", 1.0)
        check(not os.path.exists(os.path.join(home, ".config", "mousiki", "video_fetch_state.json")) or "DDDDDDDDDDD" in open(os.path.join(home, ".config", "mousiki", "video_fetch_state.json"), encoding="utf-8").read(), "fetch videos: the state file is kept when the overlay closes")
    finally:
        sess.kill()


def video_pair_tags(binary, root, scripts):
    """A video that is paired with a track (same name beside it) shows that track's artist and title in the library list."""
    if shutil.which("ffmpeg") is None:
        print("  skipped: paired video tags (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "vp_home")
    lib = os.path.join(root, "vp_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(lib)
    import subprocess
    ff = ["ffmpeg", "-v", "error", "-y"]
    r1 = subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=440:duration=3", "-metadata", "artist=Pairartist", "-metadata", "title=Pairtitle",
                              os.path.join(lib, "Pair Song.mp3")], capture_output=True)
    r2 = subprocess.run(ff + ["-f", "lavfi", "-i", "testsrc=size=64x48:rate=5:duration=3", "-pix_fmt", "yuv420p", os.path.join(lib, "Pair Song.mp4")], capture_output=True)
    if r1.returncode != 0 or r2.returncode != 0:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nVideoMode=true\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if sess.text().count("Pairartist") >= 2:
                break
        check(sess.text().count("Pairartist") >= 2, "video pair tags: the paired video row shows the artist of its track")
    finally:
        sess.kill()


def video_library_modes(binary, root, scripts):
    """Video mode OFF: a mere video file (no song of the same name) is a track like any other -- listed, found by a normal search, playable; one that is
    paired with a song is left out of the list and of a normal search; v: lists the videos only, x: songs and videos."""
    import shutil, subprocess
    if not shutil.which("ffmpeg"):
        print("  skipped: video library modes (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "vlm_home")
    lib = os.path.join(root, "vlm_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(lib)
    ff = ["ffmpeg", "-v", "error", "-y"]
    ok = True
    for name in ("Paired Song", "Other Song"):
        ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=440:duration=3", os.path.join(lib, name + ".mp3")], capture_output=True).returncode == 0
    for name in ("Paired Song", "Lone Clip"):
        ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "testsrc=size=64x48:rate=5:duration=3", "-f", "lavfi", "-i", "sine=frequency=330:duration=3",
                                   "-pix_fmt", "yuv420p", "-shortest", os.path.join(lib, name + ".mp4")], capture_output=True).returncode == 0
    if not ok:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nVideoMode=false\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if "Other Song" in sess.text():
                break
        t = sess.text()
        check("Lone Clip" in t, "video mode off: a video file without a song of its name is listed as a track")
        check(t.count("| Paired Song") == 1, "video mode off: a video paired with a song is not listed twice (%d)" % t.count("| Paired Song"))
        sess.send("/", 0.5)
        sess.send("clip", 1.0)
        check("Lone Clip" in sess.text(), "video mode off: a normal search finds the mere video")
        sess.send(b"\r", 0.5)
        sess.send(b"\r", 2.0)                                # (the box keeps the keyboard until ENTER once: then the hovered row plays)
        check("File      : Lone Clip" in sess.text(), "video mode off: the mere video plays (its sound)")
        sess.send(b"\x1b", 0.8)
        sess.send("/", 0.5)
        sess.send("v:", 1.0)
        t = sess.text()
        check("| Lone Clip" in t and "| Other Song" not in t, "video mode off: v: lists the videos only")
        sess.send(b"\x1b", 0.8)
        sess.send("/", 0.5)
        sess.send("x:", 1.0)
        t = sess.text()
        check("| Lone Clip" in t and "| Other Song" in t and t.count("| Paired Song") >= 2, "video mode off: x: lists songs and videos")
        sess.send(b"\x1b", 0.8)
    finally:
        sess.kill()


def video_translation_tab(binary, root, scripts):
    """The big video overlay: TAB goes off -> lyrics -> lyrics + translation -> translation only, through every saved translation; the
    legend names the one shown. (Needs ffmpeg for the test video.)"""
    if shutil.which("ffmpeg") is None:
        print("  skipped: video translation TAB (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "vtt_home")
    lib = os.path.join(root, "vtt_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(os.path.join(lib, "lyrics", "translations"))
    ff = ["ffmpeg", "-v", "error", "-y"]
    ok = subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=440:duration=30", os.path.join(lib, "Clip Song.mp3")], capture_output=True).returncode == 0
    ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "testsrc=size=160x90:rate=10:duration=30", "-pix_fmt", "yuv420p", os.path.join(lib, "Clip Song.mp4")], capture_output=True).returncode == 0
    if not ok:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    lines = ["First lyric line here", "Second lyric line here", "Third lyric line here"]
    with open(os.path.join(lib, "lyrics", "Clip Song.lrc"), "w", encoding="utf-8") as f:
        f.write("[00:00.10]%s\n[00:20.00]%s\n[00:25.00]%s\n" % tuple(lines))
    h = 1469598103934665603
    for l in lines:
        for c in l.encode("utf-8"):
            h = ((h ^ c) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        h = ((h ^ 0x0a) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    for code, texts in (("en", ["Erste Zeile hier", "Zweite Zeile hier", "Dritte Zeile hier"]), ("es", ["Primera linea aqui", "Segunda linea aqui", "Tercera linea aqui"])):
        with open(os.path.join(lib, "lyrics", "translations", "Clip Song.%s.txt" % code), "w", encoding="utf-8") as f:
            f.write("#mousiki-translation to=%s from=de svc=google lines=3 hash=%016x\n" % (code, h))
            f.write("\n".join(texts) + "\n")
    with open(os.path.join(home, ".config", "mousiki", "config.txt"), "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nVideoMode=true\nTranslateTo=en\nLyricsAnimation=0\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if "Clip Song" in sess.text():
                break
        sess.send(b"\r", 0.5)
        sess.send(b"\r", 3.0)
        sess.send("v", 2.5)
        t = sess.text()
        check("First lyric line" in t and "Lyrics on" in t, "video translation: the overlay starts with the lyric line")
        sess.send(b"\t", 1.2)
        t = sess.text()
        check("Lyrics + transl. en" in t and "First lyric line" in t and "Erste" in t, "video translation: TAB 1 = lyrics + the first translation (en, the Translate To language)")
        sess.send(b"\t", 1.2)
        t = sess.text()
        check("Lyrics transl. en" in t and "Erste" in t and "First lyric line" not in t, "video translation: TAB 2 = the translation only")
        sess.send(b"\t", 1.2)
        t = sess.text()
        check("Lyrics + transl. es" in t and "First lyric line" in t and "Primera" in t, "video translation: TAB 3 = lyrics + the next translation (es)")
        sess.send(b"\t", 1.2)
        t = sess.text()
        check("Lyrics transl. es" in t and "Primera" in t and "First lyric line" not in t, "video translation: TAB 4 = the second translation only")
        sess.send(b"\t", 1.2)
        check("Lyrics off" in sess.text(), "video translation: TAB 5 = lyrics off")
        sess.send(b"\t", 1.2)
        check("Lyrics on" in sess.text() and "First lyric line" in sess.text(), "video translation: TAB 6 = the lyric line again")
        check(sess.alive(), "video translation: the program is still running")
    finally:
        sess.kill()

def video_fullscreen_sizes(binary, root, scripts):
    """The & overlay: Picture Size (regular / cropped / full) and Effect rows change and are remembered in config.txt; the full screen video (v)
    opens over a playing track with a video, UP / DOWN change the effect, TAB switches the lyric line, ESC closes; it opens without a video too."""
    import shutil, subprocess
    if not shutil.which("ffmpeg"):
        print("  skipped: video full screen (ffmpeg is not installed on this machine)")
        return
    home = os.path.join(root, "vfs_home")
    lib = os.path.join(root, "vfs_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    os.makedirs(lib)
    ff = ["ffmpeg", "-v", "error", "-y"]
    ok = subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=440:duration=20", os.path.join(lib, "Clip Song.mp3")], capture_output=True).returncode == 0
    ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "testsrc=size=160x90:rate=10:duration=20", "-pix_fmt", "yuv420p", os.path.join(lib, "Clip Song.mp4")], capture_output=True).returncode == 0
    ok &= subprocess.run(ff + ["-f", "lavfi", "-i", "sine=frequency=330:duration=20", os.path.join(lib, "Plain Song.mp3")], capture_output=True).returncode == 0
    if not ok:
        print("  (skipped: ffmpeg could not make the test files)")
        return
    cfg = os.path.join(home, ".config", "mousiki", "config.txt")
    with open(cfg, "w", encoding="utf-8") as f:
        f.write("LocalMusicPath=%s\nUseTerminalSettings=true\nVideoMode=true\nHKeyVideo=Y\n" % lib)
    sess = Session(binary, home, scripts, quiet_stderr=True)
    try:
        for _ in range(30):
            sess.pump(0.5)
            if "HELP WIZARD" in sess.text():
                break
        sess.send(b"\x1b", 1.0)
        for _ in range(30):
            sess.pump(0.5)
            if "Clip Song" in sess.text():
                break
        # the menu rows: Picture Size is the fifth row, Rounded Corners the sixth, Effect the seventh
        sess.send("Y", 1.0)
        for _ in range(4):
            sess.send(b"\x1b[B", 0.2)
        check("< regular >" in sess.text(), "video overlay: the picture size starts on regular")
        sess.send(b"\x1b[C", 0.5)
        check("< cropped >" in sess.text() and "two columns cut off" in sess.text().replace("\n", " ") or "< cropped >" in sess.text(), "video overlay: RIGHT makes the size cropped")
        sess.send(b"\x1b[C", 0.5)
        check("< full >" in sess.text(), "video overlay: RIGHT again makes the size full")
        sess.send(b"\x1b[B", 0.2)   # Rounded Corners (below Picture Size)
        sess.send(b"\x1b[B", 0.2)   # Effect
        sess.send(b"\x1b[C", 0.5)
        check("< Scanlines >" in sess.text(), "video overlay: the effect row cycles to scanlines")
        sess.send(b"\x1b[D", 0.5)
        sess.send(b"\x1b[D", 0.5)
        check("< Interlace >" in sess.text(), "video overlay: LEFT from off goes round to interlace")
        sess.send(b"\x1b[C", 0.5)                              # back to off
        sess.send(b"\x1b", 0.8)
        conf = open(cfg, encoding="utf-8").read()
        check("VideoSize=full" in conf and "VideoFx=off" in conf, "video overlay: the size and the effect are saved in config.txt")
        # a track with a video: the full screen video
        sess.send("/", 0.5)
        sess.send("clip song", 1.0)
        sess.send(b"\r", 0.5)
        sess.send(b"\r", 3.0)
        sess.send("v", 2.0)
        t = sess.text()
        check("[UP/DOWN] Effect: Off" in t and "Lyrics on" in t and "Close" in t, "full screen video: the legend names effect, lyrics and close")
        lines = t.split("\n")
        leg = [i for i, l in enumerate(lines) if "[S] Options" in l or "[SHIFT+g] Grid/Scale" in l]
        check(len(leg) == 2 and leg[1] == leg[0] + 1 and "Close" in lines[leg[1]] and "Effect" in lines[leg[0]] and not lines[leg[0]].startswith("\u2502") and lines[leg[0]].count("\u2500") > 4 and not lines[leg[1]].lstrip().startswith("\u2502"), "full screen video: commands row 1 on the bottom border line, row 2 right below it")
        vid = [l for l in lines if l.startswith("\u2502") and any(0x2800 <= ord(ch) <= 0x28ff for ch in l)]
        check(bool(vid) and all(l[1:3] == "  " and l.rstrip()[-3:-1] != "" for l in vid) and all(not (0x2800 <= ord(l[1]) <= 0x28ff or 0x2800 <= ord(l[2]) <= 0x28ff) for l in vid), "full screen video: two empty columns between the picture and the border on the left")
        sess.send(b"\x1b[B", 0.8)
        check("Effect: Scanlines" in sess.text(), "full screen video: DOWN changes the effect")
        sess.send(b"\t", 0.8)
        check("Lyrics + transl." in sess.text(), "full screen video: TAB goes on to the lyrics with their translation (the status line says if none is saved)")
        sess.send(b"\t", 0.8)
        check("Lyrics off" in sess.text(), "full screen video: TAB again switches the lyric line off")
        check(any(0x2800 <= ord(ch) <= 0x28FF for ch in sess.text()), "full screen video: the picture is drawn (braille dots in this terminal)")
        # the layout: SHIFT+DOWN -> video + oscilloscope (and its options over it), another -> + spectrogram, SHIFT+UP back
        sess.send(b"\x1b[1;2B", 1.0)
        check("Layout: Video+Osci" in sess.text() or "Layout" in sess.text(), "full screen video: SHIFT+DOWN changes the layout")
        sess.send("O", 1.0)
        t = sess.text()
        check("video layout" in t and "default" in t, "full screen video layout: O opens the oscilloscope options with the layout's own style / mode")
        sess.send(b"\x1b", 1.0)
        sess.send(b"\x1b[1;2B", 1.0)
        sess.send("I", 1.0)
        check("Spectrogram (video layout)" in sess.text(), "full screen video layout: I opens the spectrogram options")
        sess.send(b"\x1b", 1.0)
        sess.send("G", 0.8)
        sess.send(b"\x1b[1;2B", 1.0)                        # video + both (the oscilloscope above the spectrogram, right of the video)
        check(sess.alive() and "VideoLayout=3" in open(cfg, encoding="utf-8").read(), "full screen video layout: SHIFT+DOWN reaches video + both")
        sess.send(b"\x1b[1;2A", 0.8)
        sess.send(b"\x1b[1;2A", 0.8)
        sess.send(b"\x1b[1;2A", 0.8)
        conf = open(cfg, encoding="utf-8").read()
        check("VideoLayout=0" in conf and "VideoLayoutScales=false" in conf, "full screen video layout: layout and scales are saved")
        sess.send("S", 1.0)
        t = sess.text()
        check("Chromatic Aberration" in t and "Contrast" in t and "Colors" in t and "Video Resolution" in t, "full screen video: S opens the options panel")
        sess.send(b"\x1b", 1.0)
        check("Chromatic Aberration" not in sess.text() and "Effect:" in sess.text(), "full screen video: ESC closes only the options panel")
        sess.send(b"\x1b", 1.0)
        check("Effect:" not in sess.text() and sess.alive(), "full screen video: ESC closes it")
        conf = open(cfg, encoding="utf-8").read()
        check("VideoFx=scanlines" not in conf, "full screen video: the effect is the track's own (not the general setting)")
        # Style Option (SHIFT+s) of the playing track: the rows of its video
        sess.send("S", 1.5)
        for _ in range(10):                                  # (a busy machine draws the overlay a little later)
            tt = sess.text()
            if "Video Chromatic Aberration" in tt and "Cover Chromatic Aberration" in tt and "Video Contrast" in tt and "Video Effect" in tt:
                break
            sess.pump(0.5)
        t = sess.text()
        check("Video of this track" in t and "Video Size" in t and "Video Effect" in t, "style option: video on / off, size and effect rows for this track")
        check("Video Contrast" in t and "Video Colors" in t and "Video Chromatic Aberration" in t and "Cover Chromatic Aberration" in t, "style option: contrast, colors and chromatic aberration rows (video and cover)")
        check(any("Use Lyrics on Video" in ln and "none" in ln for ln in t.splitlines()), "style option: TAB in the big video overlay is this track's Use Lyrics on Video (none after lyrics -> + translation -> none)")
        check(any("Video Chromatic Aberration" in ln and "as in Settings (off)" in ln for ln in t.splitlines()), "style option: the chromatic aberration row shows its value untruncated")
        check("Video Resolution" in t, "style option: a Video Resolution row")
        check(any("Use mp4 audio for this song" in ln and "false" in ln for ln in t.splitlines()), "style option: a song paired with a video has the row Use mp4 audio for this song (false)")
        for _ in range(7):                                   # down to the first video row: the preview shows the video instead of the disk
            sess.send(b"\x1b[B", 0.2)
        for _ in range(10):
            if "the video of this track" in sess.text():
                break
            sess.pump(0.5)
        check("the video of this track" in sess.text(), "style option: on a video row the preview shows the video of the track")
        for _ in range(7):
            sess.send(b"\x1b[A", 0.2)
        for _ in range(6):
            sess.send(b"\x1b[B", 0.2)
        sess.send(b"\x1b[C", 0.8)
        for _ in range(10):                                  # (a busy machine draws it a little later)
            if "Video of this track" in sess.text() and "off" in sess.text().split("Video of this track", 1)[1][:40]:
                break
            sess.pump(0.5)
        check("Video of this track" in sess.text() and "off" in sess.text().split("Video of this track", 1)[1][:40], "style option: the video of this track can be switched off")
        sess.send(b"\x1b[D", 0.5)
        sess.send(b"\x1b", 1.0)
        # a track without a video: a note instead of the overlay
        sess.send("/", 0.5)
        sess.send("plain", 1.0)
        sess.send(b"\r", 0.5)
        sess.send(b"\r", 2.0)
        sess.send("v", 1.0)
        check("Effect:" in sess.text() and sess.alive(), "full screen video: a track without a video opens it too (the clapperboard)")
        check("No video for this track." in sess.text() and "Fetch video via [" in sess.text() and "pair local video via [" in sess.text(), "full screen video: the note for a track without a video")
        sess.send("v", 1.0)
        check("Effect:" not in sess.text() and sess.alive(), "full screen video: v closes it again")
        # the overlay does not depend on the Video mode: with it off a track with a video still shows its picture
        sess.send("Y", 1.0)
        for _ in range(15):
            if "> Use Video in Lyrics Field" in sess.text():
                break
            sess.send(b"\x1b[B", 0.2)
        sess.send(b"\x1b[C", 0.8)
        check("Use Video in Lyrics Field" in sess.text() and "[ ] false" in sess.text().split("Use Video in Lyrics Field", 1)[1][:80], "video overlay: the Video mode can be switched off")
        sess.send(b"\x1b", 1.0)
        sess.send("/", 0.5)
        sess.send("clip", 1.0)
        sess.send(b"\r", 0.5)
        sess.send(b"\r", 2.0)
        sess.send("v", 2.0)
        t = sess.text()
        check("Effect:" in t and "No video for this track." not in t and any(0x2800 <= ord(ch) <= 0x28FF for ch in t),
              "full screen video: shows the picture with the Video mode off")
        sess.send("v", 1.0)
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


def organigram_menu(binary, root, scripts):
    """The "<" menu index is an organigram: Main with its six categories, DOWN goes into a category, LEFT / RIGHT move between the boxes,
    UP goes back, ENTER opens the menu of a box (a group box only goes in), ESC closes it."""
    home = os.path.join(root, "org_home")
    lib = os.path.join(root, "org_music")
    os.makedirs(os.path.join(home, ".config", "mousiki"))
    make_wav(os.path.join(lib, "Alpha Song.wav"), 30)
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
        sess.send("<", 1.5)
        t = sess.text()
        check("Main" in t and "System" in t and "Sound" in t and "Library" in t and "Other Lists" in t and "Visualizations" in t and "Lyrics" in t,
              "organigram: Main and its categories are drawn")
        check("Settings" in t and "Cheat Sheet" in t and "Equalizer" not in t, "organigram: the first category is open")
        sess.send(b"\x1b[C", 0.8)
        t = sess.text()
        check("Equalizer" in t and "Cheat Sheet" not in t, "organigram: RIGHT goes to the next category")
        for _ in range(3):
            sess.send(b"\x1b[C", 0.5)
        sess.send(b"\x1b[B", 0.6)
        sess.send(b"\x1b[B", 0.6)
        t = sess.text()
        check("Meta Data Menu" in t and "Fetch and Convert" in t and "Cover and Video Look" in t, "organigram: the meta data branches open one level at a time")
        sess.send(b"\x1b[B", 0.6)
        t = sess.text()
        check("Fetch Cover" in t and "File Conversion" in t, "organigram: the lower menus of a branch are shown")
        sess.send(b"\x1b[A", 0.5)
        sess.send(b"\x1b[A", 0.5)
        sess.send(b"\x1b[A", 0.5)
        for _ in range(4):
            sess.send(b"\x1b[D", 0.5)
        sess.send(b"\x1b[B", 0.5)
        sess.send(b"\r", 1.2)   # System / Settings: ENTER opens the settings
        t = sess.text()
        check("PATHS" in t or "GENERAL" in t, "organigram: ENTER opens the menu of the box (Settings)")
        sess.send(b"\x1b", 1.0)
        sess.send("<", 1.0)
        sess.send(b"\x1b", 1.0)
        sess.send("<", 1.0)
        for _ in range(5):
            sess.send(b"\x1b[C", 0.4)
        sess.send(b"\x1b[B", 0.6)
        sess.send(b"\x1b[C", 0.6)
        sess.send(b"\x1b[B", 0.8)
        t = sess.text()
        check("Spectrum Bars" in t and "Spectrogram" in t and "Oscilloscope" in t and "Creator" in t and "Extra Window" in t and "Options" in t,
              "organigram: the visualizations are grouped (Spectrogram: options, overlay, extra window) on several rows")
        sess.send("T", 1.0)   # SHIFT+t: the classic view, every category as a box
        t = sess.text()
        check("Equalizer" in t and "Karaoke Mode" in t and "Playlists" in t and "Fetch Cover" in t, "organigram: SHIFT+t shows the classic view with every category's options")
        sess.send(b"\x1b", 1.0)
        sess.send("<", 1.0)
        t = sess.text()
        check("Equalizer" in t and "Karaoke Mode" in t, "organigram: the classic view is remembered")
        sess.send("T", 1.0)
        t = sess.text()
        check("Karaoke Mode" not in t and "Visualizations" in t, "organigram: SHIFT+t goes back to the tree")
        sess.send(b"\x1b", 1.0)
        check("Alpha Song" in sess.text() and sess.alive(), "organigram: ESC closes it and the main screen is back")
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
        for _ in range(20):   # (a slow machine needs longer to load the track)
            if "File      :" in t: break
            sess.pump(0.5); t = sess.text()
        # (the field right of the meta data shows "No sync" when no lyrics are found, but nothing while an online lookup is still running:
        # on a machine with a network the text is not there yet, so only the meta data is checked)
        check("File      :" in t, "shrinking: the full window shows the meta data")
        if "File      :" not in t: print("  screen at the failure:\n" + "\n".join("    | " + l.rstrip() for l in t.split("\n") if l.strip()), flush=True)
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
    elif name == "translation_paste_undo":
        translation_paste_undo(binary, root, scripts)
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
             "mode_keys", "kitty_cover_options", "radio_latin_toggle", "radio_organigram", "selftest_command", "opened_lyrics_is_current", "disk_field_setting", "queue_shuffle", "translation_editor", "translation_create", "play_mode_back", "disk_border_setting", "translation_paste_undo", "translation_lines", "waterfall_view", "colors_scroll", "shrinking_window", "search_key", "video_overlays", "video_pair_tags", "video_fetch_flow", "video_fetch_restore", "settings_video_preview", "video_files_tab", "video_library_modes", "video_fullscreen_sizes", "video_translation_tab", "organigram_menu", "scope_spectro_overlays", "radio_waterfall", "radio_station_edit"]


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
