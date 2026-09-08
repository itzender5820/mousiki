#!/usr/bin/env python3
"""
Thin CLI wrapper called as a subprocess from the C++ player (see
src/lyrics_fetcher.cpp). Talks JSON on stdout.

Priority chain (the C++ side already checked a local sidecar .lrc before
even calling this): word-level/enhanced syncedlyrics first (best quality,
worth the extra round-trip), then plain (line-synced) syncedlyrics as a
fallback.

Paxsenix's Musixmatch-backed endpoint used to sit in this chain as a
faster, broader-coverage fallback, but it's unreliable (frequent misses,
occasional garbage output) — removed. syncedlyrics is the only source now.

"Better Lyrics" was requested for this chain too but isn't included here:
its API needs a YouTube video ID plus a Google API key behind a
self-hosted Cloudflare Worker (see better-lyrics/api on GitHub), which
isn't a simple public endpoint this script can just call.

Usage: fetch_lyrics.py "<title>" "<artist>"

Output (always exactly one JSON object on stdout):
  {"ok": true, "enhanced": bool, "lrc": "...", "source": "syncedlyrics"}
  {"ok": false, "error": "MODULE_MISSING" | "NOT_FOUND" | "EXCEPTION", "detail": "..."}
"""
import json
import sys


def emit(obj):
    print(json.dumps(obj))
    sys.exit(0)


def main():
    if len(sys.argv) < 2:
        emit({"ok": False, "error": "EXCEPTION", "detail": "usage: fetch_lyrics.py <title> [artist]"})

    title = sys.argv[1]
    artist = sys.argv[2] if len(sys.argv) > 2 else ""
    query = f"{title} {artist}".strip()

    try:
        import syncedlyrics
    except ImportError:
        emit({"ok": False, "error": "MODULE_MISSING", "detail": "run: pip install syncedlyrics"})
        return

    # 1) Word-level ("enhanced"/A2) synced lyrics are the best quality
    #    available, so this is tried first even though it costs an extra
    #    network round-trip that sometimes turns up nothing.
    try:
        enhanced_lrc = syncedlyrics.search(query, enhanced=True)
    except Exception:
        enhanced_lrc = None
    if enhanced_lrc:
        emit({"ok": True, "enhanced": True, "lrc": enhanced_lrc, "source": "syncedlyrics"})
        return

    # 2) Plain (line-synced) syncedlyrics search as a fallback.
    try:
        lrc = syncedlyrics.search(query)
    except Exception as e:
        emit({"ok": False, "error": "EXCEPTION", "detail": str(e)})
        return
    if lrc:
        emit({"ok": True, "enhanced": False, "lrc": lrc, "source": "syncedlyrics"})
        return

    emit({"ok": False, "error": "NOT_FOUND", "detail": f"no lyrics found for: {query}"})


if __name__ == "__main__":
    main()
