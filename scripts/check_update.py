"""Asks GitHub which release of Mousiki Plus is the newest one and prints {"ok": true, "tag": "v4.9.0", "url": "..."}.

Called by Mousiki at start-up (at most once a day; switch it off with UpdateCheck=false in config.txt). Only the public
releases page of the project is read; nothing about you or your music is sent. Uses only Python's standard library."""
import json
import os
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

API = "https://api.github.com/repos/StSchwerdtfeger/mousiki-plus/releases/latest"


def emit(obj):
    print(json.dumps(obj))
    sys.exit(0)


def main():
    try:
        import lrc   # its http_get_json: system certificates, User-Agent, timeout
    except Exception:
        emit({"ok": False, "error": "MODULE_MISSING"})
    try:
        data = lrc.http_get_json(API, {}, timeout=8)
    except Exception as e:
        emit({"ok": False, "error": "NETWORK", "detail": str(e)[:200]})
    tag = str(data.get("tag_name", "")).strip()
    if not tag:
        emit({"ok": False, "error": "NO_RELEASE"})
    emit({"ok": True, "tag": tag, "url": str(data.get("html_url", ""))})


main()
