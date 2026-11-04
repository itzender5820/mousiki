"""AcoustID metadata lookup for mousiki's meta editor (SHIFT+B, fetch list).

Instead of guessing from the text in a file name (which is what the old
MusicBrainz search did, and why "TEST.mp3" could come back as a Japanese
recording), the audio itself is identified:

    1. fpcalc (Chromaprint) decodes the first ~120s of the file and prints
       its duration plus the audio fingerprint.
    2. Fingerprint + duration go to the AcoustID web service together with
       the application key hard-coded as API_KEY below.
    3. AcoustID answers with the MusicBrainz recordings the audio matches;
       the best one supplies artist + title.

Reads a batch of lookup requests from a JSON file passed as argv[1]:

    {"requests": [{"path": "...", "title": "...", "artist": "..."}, ...]}

and prints ONE JSON object per line (JSONL) on stdout as each lookup
completes, e.g.

    {"ok": true,  "path": "...", "artist": "...", "title": "..."}
    {"ok": false, "path": "...", "error": "NOT_FOUND", "detail": "..."}
    {"done": true, "count": 7}

so the caller can show progress and apply results incrementally while the
batch is still running. stdout is line-flushed for exactly that reason.

Each request may carry a "want" list (any of "artist", "title", "album",
"year"): what the editor's FETCH row asked for. Artist and title come straight
from the AcoustID answer. Album and year are specific to a pressing, so they
take a second step: the recording's MusicBrainz id (part of the AcoustID
answer) is looked up at MusicBrainz, whose releases are grouped into album
candidates (an album, its remaster, ...) and year candidates, best first. The
result then carries the first candidate as the value plus the whole list as
"album_choices" / "year_choices" ([{"value": ..., "aux": ...}]; for an album
"aux" is its year), and the editor lets the user pick. MusicBrainz is only
asked when album or year is wanted, and at most once per second.

Deliberately stdlib-only (urllib, no `requests`): the lyrics helper is the
only script that needs the third-party package, and metadata lookup should
keep working on a fresh Python install.

AcoustID's terms require a descriptive User-Agent and rate-limit the web
service to 3 requests/second, so requests are spaced ~0.35s apart and backed
off once when a 429/503 comes back.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

# Same UTF-8-on-a-pipe fix as scripts/fetch_lyrics.py: Windows picks the ANSI
# code page for redirected stdout, which would otherwise crash the script the
# moment it prints a non-Latin track title (and a crash here is indistinguish-
# able, from the UI, from the script not existing at all). line_buffering is
# what makes each result visible to mousiki while the batch is still running.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace", line_buffering=True)
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

# The two endpoints can be pointed at a test server (MOUSIKI_ACOUSTID_URL / MOUSIKI_MB_URL); nothing else uses them.
API = os.environ.get("MOUSIKI_ACOUSTID_URL") or "https://api.acoustid.org/v2/lookup"
MB_API = os.environ.get("MOUSIKI_MB_URL") or "https://musicbrainz.org/ws/2/recording/"
USER_AGENT = "Mousiki/1.0 (https://github.com/itzender5820/mousiki; metadata editor)"
MB_USER_AGENT = "MousikiPlus/3.1 (https://github.com/StSchwerdtfeger/mousiki-plus; metadata editor)"
MB_GAP = 1.1         # seconds between MusicBrainz requests -- it allows one per second

# The AcoustID *application* key every lookup in this script is signed with.
# Hard-coded on purpose: a key belongs to one registered application, not to a
# user, and there is nothing for an end user to pick -- feeding the service a
# user-account API key instead (or any key from the wrong kind of form) is
# exactly what comes back as "invalid API key".
#
# A modified/rebranded build should register its own application at
# https://acoustid.org/new-application and put that key here.
API_KEY = "JOGLiLJqg1"
REQUEST_GAP = 0.35   # seconds between lookups -- AcoustID allows 3/second
TIMEOUT = 20         # per-request network timeout, seconds
DECODE_SECONDS = 120 # audio fpcalc fingerprints (its default)
MIN_SCORE = 0.5      # AcoustID match confidence (0.0-1.0); below this the
                     # answer is a coin flip, and a wrong tag is worse than
                     # no tag at all (this is the "Japanese artist / TEST
                     # TEST TEST" failure mode of the old text search)


class LookupError(Exception):
    """A failure with an mousiki-facing error code and a short explanation."""

    def __init__(self, code, detail):
        super().__init__(detail)
        self.code = code
        self.detail = detail


def emit(obj):
    print(json.dumps(obj, ensure_ascii=False), flush=True)


# ---------------------------------------------------------------------
# fpcalc (Chromaprint)
# ---------------------------------------------------------------------

def resolve_fpcalc():
    """Locate the fingerprinting helper.

    Tried in order: MOUSIKI_FPCLC (explicit override -- mousiki itself
    exports this with the fpcalc.exe next to its own exe, because this
    script may have been resolved from a checkout's scripts/ that never
    gets a copy of the helper), the install directory of this script's
    parent (fpcalc.exe ships next to mousiki.exe, and scripts/ sits next
    to that), the script's own directory, and finally PATH -- so a
    packaged install works without configuration and a manual Chromaprint
    install is still picked up.
    """
    name = "fpcalc.exe" if os.name == "nt" else "fpcalc"
    override = os.environ.get("MOUSIKI_FPCLC")
    if override:
        return override if os.path.isfile(override) else None
    here = os.path.dirname(os.path.abspath(__file__))
    for candidate in (os.path.join(os.path.dirname(here), name),
                      os.path.join(here, name)):
        if os.path.isfile(candidate):
            return candidate
    return shutil.which("fpcalc") or shutil.which("fpcalc.exe")


VIDEO_EXTS = (".mp4", ".m4v", ".mkv", ".mov", ".avi", ".webm", ".wmv", ".flv", ".mpg", ".mpeg", ".ogv", ".3gp")


def resolve_ffmpeg():
    override = os.environ.get("MOUSIKI_FFMPEG")
    if override and os.path.isfile(override):
        return override
    here = os.path.dirname(os.path.abspath(__file__))
    name = "ffmpeg.exe" if os.name == "nt" else "ffmpeg"
    for candidate in (os.path.join(os.path.dirname(here), name), os.path.join(here, name)):
        if os.path.isfile(candidate):
            return candidate
    return shutil.which("ffmpeg") or shutil.which("ffmpeg.exe")


def fingerprint(path):
    """(duration, fingerprint) for `path`, or raise a LookupError.

    A video file is fingerprinted by its sound: the audio is cut out first
    (ffmpeg -vn, the first DECODE_SECONDS as a temporary wav) because fpcalc
    builds differ in how well they open video containers. The duration is
    then the whole file's, which AcoustID wants, not the cut's.
    """
    if os.path.splitext(path)[1].lower() in VIDEO_EXTS:
        ff = resolve_ffmpeg()
        if ff:
            tmp = None
            try:
                fd, tmp = tempfile.mkstemp(suffix=".wav", prefix="mousiki_fp_")
                os.close(fd)
                r = subprocess.run([ff, "-y", "-v", "error", "-i", path, "-vn", "-sn", "-t", str(DECODE_SECONDS),
                                    "-ac", "2", "-ar", "44100", tmp],
                                   capture_output=True, timeout=180)
                if r.returncode == 0 and os.path.getsize(tmp) > 1000:
                    _, fp = _fingerprint_file(tmp)
                    return _video_duration(ff, path) or _, fp
            except (OSError, subprocess.TimeoutExpired):
                pass   # fall through: let fpcalc try the video itself
            finally:
                if tmp:
                    try:
                        os.remove(tmp)
                    except OSError:
                        pass
    return _fingerprint_file(path)


def _video_duration(ff, path):
    """Whole-file duration in seconds via ffmpeg's banner, or 0.0 when it cannot be read."""
    try:
        r = subprocess.run([ff, "-hide_banner", "-i", path], capture_output=True, timeout=60,
                           encoding="utf-8", errors="replace")
        for line in (r.stderr or "").splitlines():
            line = line.strip()
            if line.startswith("Duration:"):
                h, m, sec = line.split(",")[0].split()[1].split(":")
                return int(h) * 3600 + int(m) * 60 + float(sec)
    except (OSError, ValueError, IndexError, subprocess.TimeoutExpired):
        pass
    return 0.0


def _fingerprint_file(path):
    exe = resolve_fpcalc()
    if not exe:
        # Name the directory that was searched: "in scripts/" alone hides
        # WHICH scripts/ it means, and the app may have been started with a
        # working directory that resolves to a checkout's folder -- which
        # never contains the helper (it is a build product).
        here = os.path.dirname(os.path.abspath(__file__))
        raise LookupError("NO_FPCALC",
                          "fpcalc not found - looked in %s and on PATH (MOUSIKI_FPCLC overrides)" % here)
    try:
        proc = subprocess.run([exe, "-json", "-length", str(DECODE_SECONDS), path],
                              capture_output=True, timeout=180,
                              encoding="utf-8", errors="replace")
    except subprocess.TimeoutExpired:
        raise LookupError("FPCALC", "fingerprinting timed out")
    except OSError as e:
        raise LookupError("FPCALC", "could not run fpcalc: %s" % (str(e) or e.__class__.__name__))
    if proc.returncode != 0 or not proc.stdout.strip():
        detail = (proc.stderr or "").strip().splitlines()
        raise LookupError("FPCALC", detail[-1][:80] if detail else "fpcalc failed")
    try:
        data = json.loads(proc.stdout)
        duration = float(data["duration"])
        # NOT -raw: AcoustID wants Chromaprint's compressed/base64 fingerprint,
        # which is exactly what fpcalc prints without -raw. The comma-separated
        # integer form is rejected with "invalid fingerprint".
        fp = data["fingerprint"]
        if isinstance(fp, list):
            fp = ",".join(str(x) for x in fp)
    except (ValueError, KeyError, TypeError) as e:
        raise LookupError("FPCALC", "unreadable fpcalc output (%s)" % e)
    if not fp:
        raise LookupError("FPCALC", "fpcalc produced no fingerprint")
    return duration, fp


# ---------------------------------------------------------------------
# AcoustID
# ---------------------------------------------------------------------

_last_call = [0.0]


def pace():
    """Keep the batch under AcoustID's 3 requests/second."""
    wait = REQUEST_GAP - (time.time() - _last_call[0])
    if wait > 0:
        time.sleep(wait)


def lookup(duration, fp):
    """The raw AcoustID answer for one fingerprint, or raise a LookupError.

    `duration` must be a whole number of seconds: the service parses it as an
    integer and reports a fractional value as a *missing* parameter, which
    would otherwise look like a broken request rather than a rounding issue.
    """
    body = urllib.parse.urlencode({
        "client": API_KEY,
        "fingerprint": fp,
        "duration": str(int(round(duration))),
        "meta": "recordings",  # the recording ids (also used for the MusicBrainz album/year step)
    }).encode("utf-8")
    req = urllib.request.Request(API, data=body, headers={"User-Agent": USER_AGENT})

    for attempt in range(3):
        pace()
        _last_call[0] = time.time()
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
                raw = resp.read().decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            detail = ""
            try:
                detail = (e.read().decode("utf-8", "replace") or "")
                detail = (json.loads(detail).get("error") or {}).get("message", "")
            except Exception:
                detail = ""
            if e.code in (429, 503) and attempt < 2:   # rate limited: back off
                time.sleep(3.0 * (attempt + 1))
                continue
            if e.code == 400 and "invalid API key" in detail:
                raise LookupError("NO_KEY",
                                  "invalid AcoustID key - check API_KEY in scripts/fetch_meta.py")
            raise LookupError("API", detail or ("HTTP %d" % e.code))
        except urllib.error.URLError as e:
            if attempt < 2:
                time.sleep(2.0)
                continue
            raise LookupError("NETWORK", str(getattr(e, "reason", e)) or "network error")
        except OSError:
            if attempt < 2:
                time.sleep(2.0)
                continue
            raise LookupError("NETWORK", "network error")
        try:
            data = json.loads(raw)
        except ValueError:
            raise LookupError("API", "unreadable answer from AcoustID")
        if data.get("status") != "ok":
            err = data.get("error") or {}
            message = err.get("message") or "lookup failed"
            if err.get("code") == 4:
                raise LookupError("NO_KEY",
                                  "invalid AcoustID key - check API_KEY in scripts/fetch_meta.py")
            raise LookupError("API", str(message)[:80])
        return data
    raise LookupError("NETWORK", "network error")


def join_artists(rec):
    parts = []
    for artist in rec.get("artists") or []:
        if isinstance(artist, dict):
            parts.append(artist.get("name") or "")
    return " ".join(p for p in parts if p).strip()


def pick_recording(data, duration):
    """The recording this audio most likely is: (recording, artist, title).

    Results arrive sorted by AcoustID's match confidence already, but one
    result usually carries several recordings of the same song (album cut,
    club remix, live take), so among equally confident candidates the one
    whose duration is closest to the file's wins. That is what turns the
    Erasure answer from "one of four Always recordings" into the 241s one.
    """
    results = [r for r in (data.get("results") or []) if isinstance(r, dict)]
    if not results:
        return None, 0.0
    results.sort(key=lambda r: r.get("score") or 0.0, reverse=True)
    top_score = float(results[0].get("score") or 0.0)
    if top_score < MIN_SCORE:
        return None, top_score

    best = None
    best_key = None
    for result in results:
        score = float(result.get("score") or 0.0)
        if score < MIN_SCORE:
            break
        for rec in result.get("recordings") or []:
            if not isinstance(rec, dict):
                continue
            title = (rec.get("title") or "").strip()
            if not title:
                continue
            artist = join_artists(rec)
            try:
                dist = abs(float(rec.get("duration")) - duration)
            except (TypeError, ValueError):
                dist = 1e9
            key = (score, bool(artist), -dist)   # descending sort below
            if best_key is None or key > best_key:
                best_key, best = key, (rec, artist, title)
    if best is None:
        return None, top_score   # confident results, but none of them named
    return best, top_score



# ---------------------------------------------------------------------
# MusicBrainz (album + year)
# ---------------------------------------------------------------------

_last_mb = [0.0]


def mb_releases(mbid):
    """(releases, first_release_year) of one MusicBrainz recording, or raise a LookupError.

    releases: [{"title", "year", "status", "primary", "secondary"}]
    """
    query = urllib.parse.urlencode({"inc": "releases+release-groups", "fmt": "json"})
    url = MB_API + urllib.parse.quote(mbid) + "?" + query
    req = urllib.request.Request(url, headers={"User-Agent": MB_USER_AGENT, "Accept": "application/json"})
    raw = None
    for attempt in range(3):
        wait = MB_GAP - (time.time() - _last_mb[0])
        if wait > 0:
            time.sleep(wait)
        _last_mb[0] = time.time()
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
                raw = resp.read().decode("utf-8", "replace")
            break
        except urllib.error.HTTPError as e:
            if e.code in (429, 503) and attempt < 2:   # MusicBrainz says "slow down"
                time.sleep(2.5 * (attempt + 1))
                continue
            if e.code == 404:
                return [], ""
            raise LookupError("MUSICBRAINZ", "MusicBrainz HTTP %d" % e.code)
        except (urllib.error.URLError, OSError):
            if attempt < 2:
                time.sleep(2.0)
                continue
            raise LookupError("MUSICBRAINZ", "MusicBrainz not reachable")
    try:
        data = json.loads(raw)
    except (ValueError, TypeError):
        raise LookupError("MUSICBRAINZ", "unreadable answer from MusicBrainz")
    releases = []
    for rel in data.get("releases") or []:
        if not isinstance(rel, dict):
            continue
        title = (rel.get("title") or "").strip()
        if not title:
            continue
        group = rel.get("release-group") or {}
        releases.append({
            "title": title,
            "year": (rel.get("date") or "")[:4] if (rel.get("date") or "")[:4].isdigit() else "",
            "status": rel.get("status") or "",
            "primary": group.get("primary-type") or "",
            "secondary": group.get("secondary-types") or [],
        })
    frd = (data.get("first-release-date") or "")[:4]
    return releases, (frd if frd.isdigit() else "")


# Release kinds that are rarely what a person means by "the album of this song".
_ODD_SECONDARY = {"Compilation", "Live", "Soundtrack", "Remix", "DJ-mix", "Demo", "Mixtape/Street",
                  "Interview", "Audiobook", "Spokenword", "Audio drama", "Field recording"}


def release_score(rel):
    score = 0
    if rel["primary"] == "Album":
        score += 3
    elif rel["primary"] == "EP":
        score += 1
    elif rel["primary"] == "Single":
        score -= 1
    if any(s in _ODD_SECONDARY for s in rel["secondary"]):
        score -= 4
    if rel["status"] == "Official":
        score += 2
    elif rel["status"] in ("Bootleg", "Pseudo-Release", "Withdrawn", "Cancelled"):
        score -= 3
    return score


def album_candidates(releases):
    """[{"title", "years"(ascending), "score", "count"}], best first, at most 8."""
    groups = {}
    for rel in releases:
        key = " ".join(rel["title"].casefold().split())
        g = groups.setdefault(key, {"title": rel["title"], "years": set(), "score": -99, "count": 0})
        g["count"] += 1
        g["score"] = max(g["score"], release_score(rel))
        if rel["year"]:
            g["years"].add(rel["year"])
    out = list(groups.values())
    if out:
        best = max(g["score"] for g in out)      # singles / compilations only stay when nothing better exists
        out = [g for g in out if g["score"] >= best - 3]
    for g in out:
        g["years"] = sorted(g["years"])
    out.sort(key=lambda g: (-g["score"], -g["count"], g["years"][0] if g["years"] else "9999", g["title"].casefold()))
    return out[:8]


def order_years(years, first_year):
    years = sorted(set(y for y in years if y))
    if first_year and first_year in years:
        years.remove(first_year)
        years.insert(0, first_year)
    return years[:6]


def album_year_fields(mbid, want):
    """The album / year part of one answer: (fields dict, note)."""
    releases, frd = mb_releases(mbid)
    albums = album_candidates(releases)
    if not albums:
        return {}, "no album found at MusicBrainz"
    fields = {}
    top = albums[0]
    if "album" in want:
        fields["album"] = top["title"]
    if "year" in want:
        if "album" in want and top["years"]:
            years = order_years(top["years"], frd)       # the years of the album that is shown first
        else:
            years = order_years([y for g in albums for y in g["years"]], frd)
        if years:
            fields["year"] = years[0]
            fields["year_choices"] = [{"value": y, "aux": ""} for y in years]
    if "album" in want:
        choices = []
        for i, g in enumerate(albums):
            year = g["years"][0] if g["years"] else ""
            if i == 0 and fields.get("year"):
                year = fields["year"]
            choices.append({"value": g["title"], "aux": year})
        fields["album_choices"] = choices
    return fields, ""


def lookup_one(item):
    path = item.get("path") or ""
    if not path:
        emit({"ok": False, "path": path, "error": "NO_PATH",
              "detail": "no file to fingerprint"})
        return
    if not os.path.isfile(path):
        emit({"ok": False, "path": path, "error": "NO_FILE",
              "detail": "file not found"})
        return

    name = os.path.splitext(os.path.basename(path))[0]
    label = (item.get("title") or "").strip() or name
    want = set(w for w in (item.get("want") or []) if w in ("artist", "title", "album", "year"))
    if not want:
        want = {"artist", "title"}   # an older caller: exactly what this script always returned

    try:
        duration, fp = fingerprint(path)
        data = lookup(duration, fp)
        picked, score = pick_recording(data, duration)
    except LookupError as e:
        emit({"ok": False, "path": path, "error": e.code, "detail": e.detail})
        return

    if picked is None:
        detail = "no AcoustID match for: %s" % label
        if score:
            detail += " (best confidence %.2f)" % score
        emit({"ok": False, "path": path, "error": "NOT_FOUND", "detail": detail})
        return

    rec, artist, title = picked
    out = {"ok": True, "path": path}
    # Only emit the keys that were actually found: mousiki treats a missing
    # key as "leave that field alone", so a recording without an artist credit
    # can't blank one the file already had. FileName is never set -- AcoustID
    # has no opinion about file names.
    if title and "title" in want:
        out["title"] = title
    if artist and "artist" in want:
        out["artist"] = artist
    if ("album" in want or "year" in want) and rec.get("id"):
        try:
            fields, note = album_year_fields(rec["id"], want)
            out.update(fields)
            if note:
                out["note"] = note
        except LookupError as e:
            out["note"] = e.detail
    emit(out)


def main():
    if len(sys.argv) < 2:
        emit({"ok": False, "error": "USAGE",
              "detail": "usage: fetch_meta.py <requests.json>"})
        return 1
    try:
        # utf-8-sig, not utf-8: mousiki writes the file itself as plain
        # UTF-8, but "UTF8" in a Windows editor (PowerShell's Set-Content,
        # Notepad, ...) silently prepends a BOM, which json.load rejects
        # outright -- and a rejected request file looks, from the UI, like
        # "the lookup service is broken".
        with open(sys.argv[1], "r", encoding="utf-8-sig") as fh:
            payload = json.load(fh)
        requests = payload.get("requests") or []
    except Exception as e:
        emit({"ok": False, "error": "BAD_INPUT", "detail": str(e)})
        return 1

    if not API_KEY.strip():
        # The whole batch would fail one file at a time otherwise; say it once.
        emit({"ok": False, "error": "NO_KEY",
              "detail": "no AcoustID key - set API_KEY in scripts/fetch_meta.py"})
        return 1

    count = 0
    for item in requests:
        if not isinstance(item, dict):
            continue
        try:
            lookup_one(item)
        except LookupError as e:
            emit({"ok": False, "path": item.get("path") or "",
                  "error": e.code, "detail": e.detail})
        except Exception as e:
            # One broken request must not take the rest of the batch with it.
            emit({"ok": False, "path": item.get("path") or "",
                  "error": "EXCEPTION", "detail": str(e)})
        count += 1
    emit({"done": True, "count": count})
    return 0


if __name__ == "__main__":
    sys.exit(main())
