import json
import sys
import os
import signal
import subprocess


def emit(obj):
    print(json.dumps(obj))
    sys.exit(0)


def load_syncedlyrics():
    # First: normal Python environment (pip install, or system package).
    try:
        import syncedlyrics
        return syncedlyrics
    except ImportError:
        pass

    # Second: try pipx environment.
    #
    # Use MOUSIKI_PIPX_PATH env var if set (setup.sh resolves the correct
    # pipx binary and can export it).  Bare "pipx" is a last resort —
    # under PRoot, PATH can expose the wrong pipx (e.g. /usr/sbin/pipx
    # instead of the Termux one), which is why setup.sh already does
    # careful selection.  Passing the resolved path via env avoids
    # re-doing that discovery here.
    pipx_bin = os.environ.get("MOUSIKI_PIPX_PATH", "")
    if not pipx_bin:
        # Fall back to bare PATH lookup (best-effort).
        import shutil
        pipx_bin = shutil.which("pipx") or "pipx"

    try:
        pipx_home = subprocess.check_output(
            [pipx_bin, "environment", "--value", "PIPX_HOME"],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()

        # Don't hardcode $PIPX_HOME/venvs/syncedlyrics — that's a pipx
        # layout assumption.  Instead, look for any directory whose name
        # starts with "syncedlyrics" under the venvs dir, and ask its
        # venv Python for the real site-packages path.
        import glob
        venvs_dir = os.path.join(pipx_home, "venvs")
        candidates = sorted(glob.glob(os.path.join(venvs_dir, "syncedlyrics*")))

        for venv_dir in candidates:
            venv_python = os.path.join(venv_dir, "bin", "python")
            if not os.path.isfile(venv_python):
                continue

            site_packages = subprocess.check_output(
                [
                    venv_python,
                    "-c",
                    "import site; print(site.getsitepackages()[0])",
                ],
                stderr=subprocess.DEVNULL,
                text=True,
            ).strip()

            if site_packages and os.path.isdir(site_packages):
                sys.path.insert(0, site_packages)
                break

        import syncedlyrics
        return syncedlyrics

    except (ImportError, OSError, subprocess.SubprocessError):
        return None


def has_word_timestamps(lrc_text):
    """Check if LRC text actually contains word-level <mm:ss.xx> timestamps."""
    if not lrc_text:
        return False
    for line in lrc_text.split("\n"):
        # A line-level timestamp looks like [mm:ss.xx], word-level looks
        # like <mm:ss.xx> *inside* the line content (after the [mm:ss.xx]).
        # We only need one line with a word timestamp to confirm enhanced.
        bracket_end = line.find("]")
        if bracket_end < 0:
            continue
        rest = line[bracket_end + 1:]
        if "<" in rest and ">" in rest:
            return True
    return False


import re

def clean_youtube_title(text):
    if '|' in text:
        text = text.split('|')[0]
    text = re.sub(r'\(.*?\)', '', text)
    text = re.sub(r'\[.*?\]', '', text)
    return text.strip()

def main():
    if len(sys.argv) < 2:
        emit({
            "ok": False,
            "error": "EXCEPTION",
            "detail": "usage: fetch_lyrics.py <title> [artist]"
        })

    raw_title = sys.argv[1]
    title = clean_youtube_title(raw_title)
    artist = sys.argv[2] if len(sys.argv) > 2 else ""
    query = f"{title} {artist}".strip()

    syncedlyrics = load_syncedlyrics()

    if syncedlyrics is None:
        emit({
            "ok": False,
            "error": "MODULE_MISSING",
            "detail": "syncedlyrics is not available"
        })

    # Hard 25-second alarm: if everything hangs, we still return *something*
    # to the C++ caller so it doesn't wait forever.
    try:
        signal.alarm(25)
    except (AttributeError, OSError):
        pass  # Windows or restricted env — no alarm, best-effort

    def do_search(q):
        return syncedlyrics.search(
            q,
            enhanced=True,
            synced_only=True,
            providers=["Musixmatch", "Lrclib", "NetEase", "Megalobiz"],
        )

    lrc = None
    last_error = None
    
    try:
        lrc = do_search(query)
    except Exception as e:
        last_error = str(e)

    # Retry with swapped Title/Artist if it failed and we have a "-" (common in YouTube)
    if not lrc and "-" in title and not artist:
        parts = [p.strip() for p in title.split("-", 1)]
        if len(parts) == 2 and parts[0] and parts[1]:
            swapped_query = f"{parts[1]} {parts[0]}"
            try:
                lrc = do_search(swapped_query)
            except Exception as e:
                last_error = str(e)

    if not lrc:
        if last_error:
            emit({
                "ok": False,
                "error": "EXCEPTION",
                "detail": last_error
            })
        else:
            emit({
                "ok": False,
                "error": "NOT_FOUND",
                "detail": f"no lyrics found for: {query}"
            })

    # Detect whether the returned LRC actually has word-level timestamps,
    # rather than trusting the `enhanced` flag we passed in.  The library
    # sets enhanced on the Musixmatch *request*, but if Musixmatch's
    # word-by-word endpoint fails it silently falls back to line-synced,
    # and other providers never return enhanced lyrics at all.
    actually_enhanced = has_word_timestamps(lrc)

    emit({
        "ok": True,
        "enhanced": actually_enhanced,
        "lrc": lrc,
        "source": "syncedlyrics"
    })


if __name__ == "__main__":
    main()
