import json
import sys
import os
import subprocess


def emit(obj):
    print(json.dumps(obj))
    sys.exit(0)


def load_syncedlyrics():
    # First: normal Python environment.
    try:
        import syncedlyrics
        return syncedlyrics
    except ImportError:
        pass

    # Second: try pipx environment.
    try:
        pipx_path = subprocess.check_output(
            ["pipx", "environment", "--value", "PIPX_HOME"],
            stderr=subprocess.DEVNULL,
            text=True,
        ).strip()

        venv_dir = os.path.join(
            pipx_path,
            "venvs",
            "syncedlyrics",
        )

        # Ask pipx's venv Python where site-packages is.
        python_candidates = [
            os.path.join(venv_dir, "bin", "python"),
        ]

        for python in python_candidates:
            if not os.path.isfile(python):
                continue

            site_packages = subprocess.check_output(
                [
                    python,
                    "-c",
                    "import site; print(site.getsitepackages()[0])",
                ],
                stderr=subprocess.DEVNULL,
                text=True,
            ).strip()

            if site_packages:
                sys.path.insert(0, site_packages)
                break

        import syncedlyrics
        return syncedlyrics

    except (ImportError, OSError, subprocess.SubprocessError):
        return None


def main():
    if len(sys.argv) < 2:
        emit({
            "ok": False,
            "error": "EXCEPTION",
            "detail": "usage: fetch_lyrics.py <title> [artist]"
        })

    title = sys.argv[1]
    artist = sys.argv[2] if len(sys.argv) > 2 else ""
    query = f"{title} {artist}".strip()

    syncedlyrics = load_syncedlyrics()

    if syncedlyrics is None:
        emit({
            "ok": False,
            "error": "MODULE_MISSING",
            "detail": "syncedlyrics is not available"
        })

    # 1) Enhanced lyrics
    try:
        enhanced_lrc = syncedlyrics.search(query, enhanced=True)
    except Exception:
        enhanced_lrc = None

    if enhanced_lrc:
        emit({
            "ok": True,
            "enhanced": True,
            "lrc": enhanced_lrc,
            "source": "syncedlyrics"
        })

    # 2) Plain lyrics
    try:
        lrc = syncedlyrics.search(query)
    except Exception as e:
        emit({
            "ok": False,
            "error": "EXCEPTION",
            "detail": str(e)
        })

    if lrc:
        emit({
            "ok": True,
            "enhanced": False,
            "lrc": lrc,
            "source": "syncedlyrics"
        })

    emit({
        "ok": False,
        "error": "NOT_FOUND",
        "detail": f"no lyrics found for: {query}"
    })


if __name__ == "__main__":
    main()
