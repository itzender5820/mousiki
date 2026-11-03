#!/usr/bin/env python3
"""
Mousiki release toolkit
=======================

Builds installers + portable .zip packages for Windows, Linux (Ubuntu/Debian)
and macOS from nothing but the Mousiki source .zip.  Pure standard library.

    python release.py doctor                         what is installed / missing on this PC
    python release.py all   --zip Mousiki-src.zip    everything, driven from Windows
    python release.py build --target windows ...     one native build on this machine
    python release.py linux-wsl --zip ...            Linux .deb + zip via WSL (from Windows)
    python release.py cloud --repo you/Mousiki... --zip ...        macOS (etc.) via GitHub Actions
    python release.py cloud --sign-windows                         signed Windows build via SignPath (GitHub Actions)
    (the workflow also signs every package with GPG and publishes build provenance attestations, see README)
    python release.py update-repo --zip ...                        copy a new source zip into the git repo + push
    python release.py checksums --gpg-key KEYID                    SHA256SUMS + detached GPG signatures
    python release.py pin                                          fix yt-dlp / FFmpeg / Python / PyPI to exact versions + hashes (pins.json)
    python release.py pins-check [--out report.md]                 download every pinned file again, compare its hash (CI: pins-watch)

Outputs land in ./dist :
    Mousiki-<ver>-windows-x64-setup.exe      + Mousiki-<ver>-windows-x64-portable.zip
    Mousiki-<ver>-linux-amd64.deb           + Mousiki-<ver>-linux-amd64-portable.zip
    Mousiki-<ver>-macos-universal.dmg        + Mousiki-<ver>-macos-universal-portable.zip
    Mousiki-<ver>-macos-arm64.dmg            + Mousiki-<ver>-macos-arm64-portable.zip      (Apple silicon only; --mac-arch both, the default)
    SHA256SUMS.txt
Files of other versions found in ./dist are moved to ./dist/older/<version>/ when a build starts, so
only the current version is left to upload.

Re-running is the "update" workflow: the source zip is synced into a persistent
work tree (only changed files are rewritten) so CMake recompiles only what
changed.  --clean wipes the build trees for a from-scratch rebuild,
--refresh-tools re-downloads ffmpeg / yt-dlp / Python.
"""
import argparse
import calendar
import hashlib
import json
import os
import platform
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
APP_NAME = "Mousiki"
DEFAULT_PY_EMBED = "3.13.14"
SRC_EXCLUDE = {".git", ".github", "packaging", "build", "dist", "work", "__MACOSX"}   # never part of the product source
SRC_SKIP_EXT = {".gif"}                                                                # README previews etc.: never needed to build or package
REPO_KEEP = {".git", ".github", "packaging"}                                          # update-repo never touches these
WORKFLOW = "build-installers.yml"
CLOUD_TAG = "mousiki-source"
CLOUD_ASSET = "mousiki-source.zip"
RELEASE_ARTIFACT = "mousiki-release"       # the workflow's final artifact: packages + signatures
APPID_GUID = "{8F6B2C1E-3A4D-4B7E-9C15-6D2E0A7F5B38}"   # keep fixed: upgrades install over the old version
# SDL2 for the scope and spectrogram windows: Mousiki loads SDL2.dll at run time from the folder of mousiki.exe.
# Pinned and checksummed (SDL publishes no checksum file). To update, change both lines together:
# the SHA-256 of SDL2-<version>-win32-x64.zip from https://github.com/libsdl-org/SDL/releases
SDL2_VERSION = "2.32.10"
SDL2_WIN64_SHA256 = "6cf9706eefd0a4a06dc764007934d428afaf029fabdd408a9e646048c91e18fb"
THIRD_PARTY_LICENSES = "THIRD_PARTY_LICENSES.txt"   # licence texts of the code compiled into mousiki / fpcalc


# --------------------------------------------------------------------------
# small helpers
# --------------------------------------------------------------------------
def log(msg):
    print("==> " + msg, flush=True)


def warn(msg):
    print("WARNING: " + msg, file=sys.stderr, flush=True)


def die(msg, code=1):
    print("ERROR: " + msg, file=sys.stderr, flush=True)
    sys.exit(code)


class DownloadFailed(SystemExit):
    """A download that could not be done (server not reachable ...). A SystemExit like die(), so it still ends the build
    where nobody catches it; a caller with a fallback catches only this -- never a SHA-256 mismatch, which stays a die()."""


def die_download(msg):
    print("ERROR: " + msg, file=sys.stderr, flush=True)
    raise DownloadFailed(1)


def host_target():
    if sys.platform == "win32":
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    if sys.platform.startswith("linux"):
        return "linux"
    die("unsupported host OS: " + sys.platform)


def run(cmd, cwd=None, env=None, check=True):
    cmd = [str(c) for c in cmd]
    print("   $ " + " ".join(shlex.quote(c) for c in cmd), flush=True)
    try:
        rc = subprocess.call(cmd, cwd=str(cwd) if cwd else None, env=env)
    except FileNotFoundError:
        if check:
            die("command not found: " + cmd[0])
        return 127
    if check and rc != 0:
        die("command failed (exit %d): %s" % (rc, cmd[0]))
    return rc


def capture(cmd):
    try:
        return subprocess.check_output([str(c) for c in cmd], stderr=subprocess.DEVNULL).decode("utf-8", "replace").strip()
    except Exception:
        return ""


def which(name):
    return shutil.which(name)


def rmtree(p):
    p = Path(p)
    if not p.exists():
        return

    def onerr(func, path, exc):          # read-only files on Windows
        try:
            os.chmod(path, stat.S_IWRITE)
            func(path)
        except Exception:
            pass
    shutil.rmtree(str(p), onerror=onerr)


def sha256_of(path_or_bytes):
    h = hashlib.sha256()
    if isinstance(path_or_bytes, (bytes, bytearray)):
        h.update(path_or_bytes)
    else:
        with open(str(path_or_bytes), "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
    return h.hexdigest()


def make_exec(p):
    p = Path(p)
    p.chmod(p.stat().st_mode | 0o111)


def write_text(path, text, newline="\n", executable=False):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(str(path), "w", encoding="utf-8", newline="") as f:
        f.write(text.replace("\r\n", "\n").replace("\n", newline))
    if executable and os.name != "nt":
        make_exec(path)


def fetch(url, dest, refresh=False, max_age_days=None, tries=4):
    """Download url -> dest (cached).  Falls back to a stale cache when offline."""
    dest = Path(dest)
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() and not refresh:
        if max_age_days is None or (time.time() - dest.stat().st_mtime) < max_age_days * 86400:
            return dest
    log("Downloading " + url)
    tmp = dest.with_name(dest.name + ".part")
    try:
        last = None
        for attempt in range(1, tries + 1):   # servers like johnvansickle.com time out now and then
            try:
                req = urllib.request.Request(url, headers={"User-Agent": "mousiki-release-toolkit"})
                with urllib.request.urlopen(req, timeout=120) as r, open(str(tmp), "wb") as f:
                    shutil.copyfileobj(r, f, 1 << 20)
                tmp.replace(dest)
                last = None
                break
            except Exception as e:
                last = e
                if attempt < tries:
                    warn("download attempt %d failed (%s) - retrying in %d s" % (attempt, e, attempt * 10))
                    time.sleep(attempt * 10)
        if last is not None:
            raise last
    except Exception as e:
        if tmp.exists():
            tmp.unlink()
        if dest.exists():
            warn("download failed (%s) - using cached copy %s" % (e, dest.name))
            return dest
        die_download("download failed: %s\n    %s" % (url, e))
    return dest


def verify_sha256(dest, checksum_url, filename=None):
    """Compare a downloaded file with the checksum its publisher posts (a bare hash or a 'hash  name' list).
    Mismatch -> the file is deleted and the build stops; checksum not reachable -> warning only."""
    dest = Path(dest)
    try:
        req = urllib.request.Request(checksum_url, headers={"User-Agent": "mousiki-release-toolkit"})
        with urllib.request.urlopen(req, timeout=60) as r:
            text = r.read().decode("utf-8", "replace")
    except Exception as e:
        warn("could not fetch the checksum for %s (%s) - not verified" % (dest.name, e))
        return False
    want = None
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if filename is None and len(parts[0]) == 64:
            want = parts[0].lower()
            break
        if filename is not None and len(parts) >= 2 and parts[-1].lstrip("*") == filename:
            want = parts[0].lower()
            break
    if not want:
        warn("no checksum for %s in %s - not verified" % (filename or dest.name, checksum_url))
        return False
    got = sha256_of(dest)
    if got != want:
        dest.unlink()
        die("SHA-256 mismatch for %s (expected %s, got %s) - the download was deleted.\n"
            "  A damaged download fixes itself: run the build again. If it repeats, the file on the server is not the pinned one:\n"
            "  find out why it changed, then update the pin with 'release.py pin' (never skip the check)." % (dest.name, want, got))
    log("SHA-256 verified: " + dest.name)
    return True


def verify_pinned_sha256(dest, want):
    """Compare a download with a checksum pinned in this script; a mismatch deletes it and stops the build."""
    dest = Path(dest)
    got = sha256_of(dest)
    if got != want.lower():
        dest.unlink()
        die("SHA-256 mismatch for %s (expected %s, got %s) - the download was deleted.\n"
            "  A damaged download fixes itself: run the build again. If it repeats, the file on the server is not the pinned one:\n"
            "  find out why it changed, then update the pin with 'release.py pin' (never skip the check)." % (dest.name, want, got))
    log("SHA-256 verified: " + dest.name)


# ---- pinned downloads ---------------------------------------------------------
# pins.json (next to this script) fixes every bundled tool to ONE exact file: {name: {"url": <versioned url>, "sha256": ...}}.
# The checksum a publisher posts next to a download lives on the same server, so it only catches a damaged download, not
# a replaced one. A pin made once (and reviewed by you) catches both. Create / update it with:  release.py pin
# Tools without a pin are still downloaded the old way (latest + the publisher's checksum) with a warning.
PINS_FILE = HERE / "pins.json"


def load_pins():
    try:
        return json.loads(PINS_FILE.read_text(encoding="utf-8")).get("files", {})
    except Exception:
        return {}


def fetch_pinned(name, dest, a):
    """The file pinned under `name`, verified against its pinned SHA-256 (cache used when it matches);
    None when there is no pin (the caller then downloads the unpinned way)."""
    pin = load_pins().get(name)
    if not pin:
        warn("%s is not pinned - run 'release.py pin' to fix its version and checksum" % name)
        return None
    dest = Path(dest)
    if dest.exists() and not a.refresh_tools and sha256_of(dest) == pin["sha256"].lower():
        log("SHA-256 verified (pinned, cached): " + dest.name)
        return dest
    got = fetch(pin["url"], dest, True)
    verify_pinned_sha256(got, pin["sha256"])
    return got


def make_zip(src_dir, dest_zip, root_name):
    """Zip src_dir as <root_name>/... keeping Unix exec bits."""
    src_dir, dest_zip = Path(src_dir), Path(dest_zip)
    if dest_zip.exists():
        dest_zip.unlink()
    dest_zip.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(str(dest_zip), "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for p in sorted(src_dir.rglob("*")):
            arc = root_name + "/" + p.relative_to(src_dir).as_posix()
            if p.is_symlink():
                continue
            if p.is_dir():
                continue
            zi = zipfile.ZipInfo.from_file(str(p), arc)
            zi.compress_type = zipfile.ZIP_DEFLATED
            with open(str(p), "rb") as fsrc, zf.open(zi, "w") as fdst:
                shutil.copyfileobj(fsrc, fdst, 1 << 20)
    return dest_zip


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return "%.1f %s" % (n, unit) if unit != "B" else "%d B" % n
        n /= 1024.0


# --------------------------------------------------------------------------
# source handling
# --------------------------------------------------------------------------
def _write_if_changed(target, data, mode=None):
    if target.is_file() and target.stat().st_size == len(data) and sha256_of(target) == sha256_of(data):
        return False
    target.parent.mkdir(parents=True, exist_ok=True)
    with open(str(target), "wb") as f:
        f.write(data)
    if os.name != "nt":
        if mode:
            target.chmod((mode & 0o777) | 0o600)
        elif target.suffix == ".sh":
            make_exec(target)
    return True


def _prune(dest, wanted, keep=()):
    removed = 0
    for p in sorted(dest.rglob("*"), reverse=True):
        rel = p.relative_to(dest)
        if rel.parts and rel.parts[0] in keep:
            continue
        if p.is_file() or p.is_symlink():
            if rel.as_posix() not in wanted:
                p.unlink()
                removed += 1
        elif p.is_dir() and not any(p.iterdir()):
            p.rmdir()
    return removed


def sync_zip(zip_path, dest, keep=()):
    """Mirror the source tree inside the zip into dest; only touch changed files.
    Top-level names in `keep` are neither written nor deleted."""
    zip_path, dest = Path(zip_path), Path(dest)
    if not zip_path.is_file():
        die("source zip not found: %s" % zip_path)
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(str(zip_path)) as zf:
        infos = [i for i in zf.infolist() if not i.is_dir()]
        cmakes = [i.filename.replace("\\", "/") for i in infos
                  if i.filename.replace("\\", "/").split("/")[-1] == "CMakeLists.txt"]
        if not cmakes:
            die("no CMakeLists.txt in %s - is this the Mousiki source zip?" % zip_path.name)
        root = min(cmakes, key=lambda n: n.count("/"))
        root = root[:-len("CMakeLists.txt")]
        wanted, changed = set(), 0
        for info in infos:
            name = info.filename.replace("\\", "/")
            if not name.startswith(root):
                continue
            rel = name[len(root):]
            parts = Path(rel).parts
            if not rel or ".." in parts or ".git" in parts or "__MACOSX" in parts or (parts and parts[0] in keep):
                continue
            if Path(rel).suffix.lower() in SRC_SKIP_EXT:
                continue
            wanted.add(rel)
            if _write_if_changed(dest / rel, zf.read(info), (info.external_attr >> 16) & 0o777):
                changed += 1
    removed = _prune(dest, wanted, keep)
    log("Source synced: %d file(s) written, %d removed, %d total" % (changed, removed, len(wanted)))
    return dest


def sync_dir(src_dir, dest):
    """Same as sync_zip, but the source is a checked-out directory (CI builds from the repo)."""
    src_dir, dest = Path(src_dir), Path(dest)
    if not (src_dir / "CMakeLists.txt").is_file():
        die("%s has no CMakeLists.txt - is --src the Mousiki source root?" % src_dir)
    dest.mkdir(parents=True, exist_ok=True)
    wanted, changed = set(), 0
    for dp, dns, fns in os.walk(str(src_dir)):
        rel_dir = Path(dp).relative_to(src_dir)
        if not rel_dir.parts:
            dns[:] = [d for d in dns if d not in SRC_EXCLUDE]
        for fn in fns:
            p = Path(dp) / fn
            rel = (rel_dir / fn).as_posix()
            if p.is_symlink() or p.suffix.lower() in SRC_SKIP_EXT:
                continue
            wanted.add(rel)
            if _write_if_changed(dest / rel, p.read_bytes(), p.stat().st_mode):
                changed += 1
    removed = _prune(dest, wanted)
    log("Source synced from %s: %d file(s) written, %d removed, %d total" % (src_dir, changed, removed, len(wanted)))
    return dest


def detect_version(src, override):
    cmake_ver = None
    txt = (src / "CMakeLists.txt").read_text(encoding="utf-8", errors="replace")
    m = re.search(r"project\s*\(\s*\w+\s+VERSION\s+([\d.]+)", txt, re.I)
    if m:
        cmake_ver = m.group(1)
    readme_ver = None
    rp = src / "README.md"
    if rp.is_file():
        m = re.search(r"^#\s*\W*Mousiki(?:\s+Plus)?\s+v?(\d+\.\d+\.\d+)", rp.read_text(encoding="utf-8", errors="replace"), re.M)
        if m:
            readme_ver = m.group(1)
    version = (override or "").lstrip("v") or readme_ver or cmake_ver
    if not version:
        die("could not detect the version - pass --version X.Y.Z")
    if cmake_ver and cmake_ver != version:
        warn("CMakeLists.txt says %s but the release version is %s "
             "(use --sync-version to patch the build copy)" % (cmake_ver, version))
    return version, cmake_ver


def sync_version(src, version, cmake_ver):
    """Patch the *work copy* (never your zip) so CMake and the About screens show the release version.
    The About screens of the player and the radio get it from CMakeLists.txt (MOUSIKI_VERSION, src/version.h),
    so the project() line is the only thing to patch."""
    if not cmake_ver or cmake_ver == version:
        return
    cm = src / "CMakeLists.txt"
    t = cm.read_text(encoding="utf-8")
    cm.write_text(re.sub(r"(project\s*\(\s*\w+\s+VERSION\s+)[\d.]+", r"\g<1>" + version, t, count=1, flags=re.I), encoding="utf-8")
    log("Patched build copy: version %s -> %s" % (cmake_ver, version))


# --------------------------------------------------------------------------
# build
# --------------------------------------------------------------------------
def find_built(build, exe_name):
    for cand in (build / exe_name, build / "Release" / exe_name):
        if cand.is_file():
            return cand
    for p in build.rglob(exe_name):
        if p.is_file() and (p.parent / "scripts").is_dir():
            return p
    die("%s not found under %s after the build" % (exe_name, build))


def cmake_build(src, build, extra=None, config="Release"):
    run(["cmake", "-S", src, "-B", build, "-DCMAKE_BUILD_TYPE=" + config] + (extra or []))
    run(["cmake", "--build", build, "--config", config, "--parallel", str(os.cpu_count() or 2)])


def stage_common(stage, src, exe, target):
    """app/<exe>, scripts/ (+fpcalc), docs, license, config template."""
    exe_name = "mousiki.exe" if target == "windows" else "mousiki"
    (stage / "app").mkdir(parents=True, exist_ok=True)
    shutil.copy2(str(exe), str(stage / "app" / exe_name))
    scripts = exe.parent / "scripts"
    if not scripts.is_dir():
        scripts = src / "scripts"
        warn("no scripts/ next to the built executable - using the source copy (no fpcalc)")
    shutil.copytree(str(scripts), str(stage / "scripts"))
    fp = "fpcalc.exe" if target == "windows" else "fpcalc"
    if not (stage / "scripts" / fp).is_file():
        warn("fpcalc was not built - the AcoustID metadata fetch will not work")
    if not (src / THIRD_PARTY_LICENSES).is_file():
        warn("%s is missing from the source tree - the packages ship without the third-party licence texts" % THIRD_PARTY_LICENSES)
    for name in ("LICENSE", THIRD_PARTY_LICENSES, "README.md", "config.txt"):
        if (src / name).is_file():
            shutil.copy2(str(src / name), str(stage / name))
    # radio mode: the default station list and radio settings sit next to the executable (CMake copies them there)
    for name in ("stations.txt", "radio_config.txt"):
        for cand in (exe.parent / name, src / name, src / "radio" / name):
            if cand.is_file():
                shutil.copy2(str(cand), str(stage / "app" / name))
                break
        else:
            warn("%s not found - the radio starts with its built-in defaults" % name)
    manual = next(iter(sorted(src.glob("Mousiki_User_Manual*"))), None)
    if manual and manual.is_dir():
        shutil.copytree(str(manual), str(stage / "docs"), ignore=shutil.ignore_patterns("*.gif", "*.zip"))
    if target == "windows" and (src / "mousiki.ico").is_file():
        shutil.copy2(str(src / "mousiki.ico"), str(stage / "mousiki.ico"))
    if os.name != "nt":
        make_exec(stage / "app" / exe_name)        # not the .txt files next to it
        for p in (stage / "scripts").iterdir():
            if p.name == "fpcalc":
                make_exec(p)


def find_manual_pdf(stage):
    """The user-manual PDF that stage_common() copied into <stage>/docs (None if the source has none)."""
    docs = stage / "docs"
    pdfs = sorted(docs.rglob("*.pdf")) if docs.is_dir() else []
    if not pdfs:
        warn("no user-manual PDF found in the source tree - the manual shortcuts/entries are skipped")
        return None
    pdfs.sort(key=lambda p: (0 if re.search(r"guide|manual", p.name, re.I) else 1, len(p.parts)))
    return pdfs[0]


# ---- launchers ------------------------------------------------------------
WIN_LAUNCHER = r"""@echo off
rem Mousiki launcher: puts the bundled helper tools (ffmpeg, yt-dlp, Python) on PATH, then starts the app.
rem If a file called portable.txt sits next to this script, settings/cache/history are kept in .\data
rem instead of %USERPROFILE%\.config\mousiki  (that is what makes the .zip version portable).
setlocal
set "MOUSIKI_HOME=%~dp0"
set "MOUSIKI_SCRIPTS_DIR=%MOUSIKI_HOME%scripts"
set "PATH=%MOUSIKI_HOME%bin;%MOUSIKI_HOME%bin\python;%PATH%"
if exist "%MOUSIKI_HOME%portable.txt" if not exist "%MOUSIKI_HOME%data" mkdir "%MOUSIKI_HOME%data"
if exist "%MOUSIKI_HOME%portable.txt" set "HOME=%MOUSIKI_HOME%data"
"%MOUSIKI_HOME%app\mousiki.exe" %*
"""

WIN_WT_LAUNCHER = r"""@echo off
rem Opens Mousiki in Windows Terminal (the legacy console window cannot render the UI properly).
rem Mousiki keeps a Windows Terminal profile "Mousiki" with its own font (Cascadia Mono) and colour scheme;
rem once it exists, the window opens with that profile -- your own profiles are not changed.
where wt.exe >nul 2>&1
if errorlevel 1 goto plain
if exist "%LOCALAPPDATA%\Microsoft\Windows Terminal\Fragments\Mousiki\mousiki.json" (
    start "" wt.exe -p "Mousiki" --title Mousiki "%~dp0mousiki.cmd" %*
) else (
    start "" wt.exe --title Mousiki "%~dp0mousiki.cmd" %*
)
exit /b
:plain
call "%~dp0mousiki.cmd" %*
"""

UNIX_LAUNCHER = r"""#!/bin/sh
# Mousiki launcher: puts the bundled helper tools on PATH, then starts the app.
# If a file called portable.txt sits next to this script, settings/cache/history
# are kept in ./data instead of ~/.config/mousiki (that makes the .zip version portable).
SELF="$0"
while [ -h "$SELF" ]; do
    DIR="$(cd "$(dirname "$SELF")" && pwd)"
    SELF="$(readlink "$SELF")"
    case "$SELF" in /*) ;; *) SELF="$DIR/$SELF" ;; esac
done
APP_HOME="$(cd "$(dirname "$SELF")" && pwd)"
export MOUSIKI_SCRIPTS_DIR="$APP_HOME/scripts"
PATH="$APP_HOME/bin:$PATH"@@EXTRA_PATH@@
export PATH
if [ -f "$APP_HOME/portable.txt" ]; then
    mkdir -p "$APP_HOME/data"
    HOME="$APP_HOME/data"
    export HOME
fi
exec "$APP_HOME/app/mousiki" "$@"
"""


# "lala" is a second name for the same launcher: both commands start the app.
WIN_LALA_LAUNCHER = r"""@echo off
rem "lala" = a second name for Mousiki: the same as mousiki.cmd.
call "%~dp0mousiki.cmd" %*
"""


def write_launchers(stage, target):
    if target == "windows":
        write_text(stage / "mousiki.cmd", WIN_LAUNCHER, newline="\r\n")
        write_text(stage / "lala.cmd", WIN_LALA_LAUNCHER, newline="\r\n")
        write_text(stage / "mousiki-wt.cmd", WIN_WT_LAUNCHER, newline="\r\n")
    else:
        extra = ""
        if target == "macos":     # GUI-less shells on macOS often lack Homebrew on PATH
            extra = '\nPATH="$PATH:/opt/homebrew/bin:/usr/local/bin"'
        launcher = UNIX_LAUNCHER.replace("@@EXTRA_PATH@@", extra)
        # The launcher finds the app relative to its own (symlink-resolved) location, so a plain copy under a
        # second name works anywhere -- also in the portable .zip, which does not keep symlinks.
        write_text(stage / "mousiki", launcher, executable=True)
        write_text(stage / "lala", launcher, executable=True)


# ---- bundled third-party tools ----------------------------------------------
def python_arch_tag():
    return "arm64" if platform.machine().lower() in ("arm64", "aarch64") else "amd64"


def bundle_windows_tools(stage, dl, a):
    bin_dir = stage / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    notices = []

    yt = fetch_pinned("yt-dlp.exe", dl / "yt-dlp.exe", a)
    if yt is None:
        yt = fetch("https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe",
                   dl / "yt-dlp.exe", a.refresh_tools, max_age_days=1)
        verify_sha256(yt, "https://github.com/yt-dlp/yt-dlp/releases/latest/download/SHA2-256SUMS", "yt-dlp.exe")
    shutil.copy2(str(yt), str(bin_dir / "yt-dlp.exe"))
    notices.append("yt-dlp (Unlicense) - https://github.com/yt-dlp/yt-dlp")

    ff = fetch_pinned("ffmpeg-win-essentials.zip", dl / "ffmpeg-win-essentials.zip", a)
    if ff is None:
        ff = fetch("https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip",
                   dl / "ffmpeg-win-essentials.zip", a.refresh_tools)
        verify_sha256(ff, "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip.sha256")
    with zipfile.ZipFile(str(ff)) as zf:
        got = set()
        for n in zf.namelist():
            base = n.split("/")[-1]
            depth = n.count("/")
            if base in ("ffmpeg.exe", "ffprobe.exe") and "/bin/" in n:
                (bin_dir / base).write_bytes(zf.read(n)); got.add(base)
            elif depth == 1 and base in ("LICENSE", "README.txt"):
                (bin_dir / ("ffmpeg-" + base + ("" if base.endswith(".txt") else ".txt"))).write_bytes(zf.read(n))
        if got != {"ffmpeg.exe", "ffprobe.exe"}:
            die("ffmpeg.exe/ffprobe.exe not found in the downloaded FFmpeg archive")
    notices.append("FFmpeg essentials build (GPLv3; source: https://www.gyan.dev/ffmpeg/builds/ and https://ffmpeg.org) - see bin/ffmpeg-LICENSE.txt")

    # embeddable Python (for lyrics + metadata scripts) -----------------------
    arch = python_arch_tag()
    pyver = a.python_embed_version
    pz = fetch_pinned("python-%s-embed-%s.zip" % (pyver, arch), dl / ("python-%s-embed-%s.zip" % (pyver, arch)), a)
    if pz is None:
        pz = fetch("https://www.python.org/ftp/python/%s/python-%s-embed-%s.zip" % (pyver, pyver, arch),
                   dl / ("python-%s-embed-%s.zip" % (pyver, arch)), a.refresh_tools)
    py_dir = bin_dir / "python"
    with zipfile.ZipFile(str(pz)) as zf:
        zf.extractall(str(py_dir))
    shutil.copy2(str(py_dir / "python.exe"), str(py_dir / "python3.exe"))   # mousiki looks for python3 / python
    pth = next(py_dir.glob("python*._pth"), None)
    if pth:   # embeddable Python ignores PYTHONPATH when a ._pth exists, so list the dirs explicitly
        lines = pth.read_text(encoding="utf-8").splitlines()
        lines = [l for l in lines if l.strip() not in ("../../pylib", "../../scripts")]
        lines += ["../../scripts"]
        pth.write_text("\n".join(lines) + "\n", encoding="utf-8")
    notices.append("Python %s embeddable (PSF licence) - https://www.python.org" % pyver)

    # SDL2 for the scope / spectrogram windows: next to mousiki.exe, where LoadLibrary looks first ---------
    if python_arch_tag() == "amd64":
        sdl_name = "SDL2-%s-win32-x64.zip" % SDL2_VERSION
        sz = fetch("https://github.com/libsdl-org/SDL/releases/download/release-%s/%s" % (SDL2_VERSION, sdl_name),
                   dl / sdl_name, a.refresh_tools)
        verify_pinned_sha256(sz, SDL2_WIN64_SHA256)
        with zipfile.ZipFile(str(sz)) as zf:
            names = {n.split("/")[-1]: n for n in zf.namelist()}
            if "SDL2.dll" not in names:
                die("SDL2.dll not found in %s" % sdl_name)
            (stage / "app" / "SDL2.dll").write_bytes(zf.read(names["SDL2.dll"]))
            if "README-SDL.txt" in names:
                (stage / "app" / "README-SDL.txt").write_bytes(zf.read(names["README-SDL.txt"]))
        notices.append("SDL2 %s (zlib licence, see %s) - https://www.libsdl.org - app/SDL2.dll, "
                       "used by the scope and spectrogram windows" % (SDL2_VERSION, THIRD_PARTY_LICENSES))
    else:
        warn("no SDL2 build for this architecture is bundled - the scope / spectrogram windows need SDL2.dll next to mousiki.exe")

    write_notices(stage, notices)


def linux_arch():
    return "arm64" if platform.machine().lower() in ("aarch64", "arm64") else "amd64"


def bundle_ytdlp_unix(stage, dl, a, target):
    bin_dir = stage / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    if target == "macos":
        asset = "yt-dlp_macos"
    else:
        asset = "yt-dlp_linux_aarch64" if linux_arch() == "arm64" else "yt-dlp_linux"
    yt = fetch_pinned(asset, dl / asset, a)
    if yt is None:
        yt = fetch("https://github.com/yt-dlp/yt-dlp/releases/latest/download/" + asset, dl / asset,
                   a.refresh_tools, max_age_days=1)
        verify_sha256(yt, "https://github.com/yt-dlp/yt-dlp/releases/latest/download/SHA2-256SUMS", asset)
    shutil.copy2(str(yt), str(bin_dir / "yt-dlp"))
    make_exec(bin_dir / "yt-dlp")
    return "yt-dlp (Unlicense) - https://github.com/yt-dlp/yt-dlp"


def bundle_unix_tools(stage, dl, a, target):
    notices = [bundle_ytdlp_unix(stage, dl, a, target)]
    write_notices(stage, notices)


def add_static_ffmpeg_linux(pstage, dl, a):
    arch = linux_arch()
    url = "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-%s-static.tar.xz" % arch
    source = "John Van Sickle (GPLv3; source: https://johnvansickle.com/ffmpeg/ and https://ffmpeg.org)"
    try:
        tf = fetch_pinned("ffmpeg-linux-%s-static.tar.xz" % arch, dl / ("ffmpeg-linux-%s-static.tar.xz" % arch), a)
        if tf is None:
            tf = fetch(url, dl / ("ffmpeg-linux-%s-static.tar.xz" % arch), a.refresh_tools, tries=2)
    except DownloadFailed:
        # johnvansickle.com sometimes does not answer GitHub's runners: the GitHub-hosted BtbN builds (GPLv3) are the fallback --
        # only when the server was not reachable (a SHA-256 mismatch stops the build), and only a pinned, dated BtbN build
        warn("static ffmpeg from johnvansickle.com is not reachable - trying the pinned BtbN build on GitHub")
        tf = fetch_pinned("ffmpeg-linux-%s-btbn.tar.xz" % arch, dl / ("ffmpeg-linux-%s-btbn.tar.xz" % arch), a)
        if tf is None:
            die("no static ffmpeg for the Linux portable zip: johnvansickle.com is not reachable and the BtbN fallback is not\n"
                "  pinned (run 'release.py pin'). An unverified build is never bundled.")
        source = "BtbN FFmpeg-Builds (GPLv3; source: https://github.com/BtbN/FFmpeg-Builds and https://ffmpeg.org)"
    bin_dir = pstage / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    got = set()
    with tarfile.open(str(tf), "r:xz") as t:
        for m in t.getmembers():
            base = m.name.split("/")[-1]
            if m.isfile() and base in ("ffmpeg", "ffprobe"):
                (bin_dir / base).write_bytes(t.extractfile(m).read())
                make_exec(bin_dir / base)
                got.add(base)
            elif m.isfile() and base in ("GPLv3.txt", "readme.txt"):
                (bin_dir / ("ffmpeg-" + base)).write_bytes(t.extractfile(m).read())
    if got != {"ffmpeg", "ffprobe"}:
        warn("static ffmpeg/ffprobe not found in archive - portable zip will need a system ffmpeg")
        return
    with open(str(pstage / "THIRD-PARTY.txt"), "a", encoding="utf-8") as f:
        f.write("- FFmpeg static build by %s\n" % source)


def write_notices(stage, lines):
    body = ("Third-party components bundled with this Mousiki package\n"
            "=====================================================\n"
            "Mousiki itself is Apache-2.0 licensed (see LICENSE).\n"
            "The code compiled into mousiki and fpcalc (miniaudio, KISS FFT, miniz, Chromaprint) is listed\n"
            "with its licence texts in %s.\n"
            "The tools below are downloaded unmodified from their upstream projects at build time.\n\n" % THIRD_PARTY_LICENSES)
    write_text(stage / "THIRD-PARTY.txt", body + "".join("- %s\n" % l for l in lines))


PORTABLE_TXT = """Mousiki portable mode
=====================
This file's presence makes Mousiki keep its settings, cache, playlists and history in the
"data" folder next to the launcher instead of in your user profile (~/.config/mousiki).
Delete this file to use the regular user-profile locations instead.
"""


def portable_readme(target, version):
    head = "Mousiki %s - portable (%s)\n%s\n\n" % (version, target, "=" * 40)
    if target == "windows":
        body = ("Unzip anywhere and run  mousiki-wt.cmd  (opens Windows Terminal) - or  mousiki.cmd  (or  lala.cmd)  from a terminal.\n"
                "ffmpeg, yt-dlp and Python are included; nothing else has to be installed.\n"
                "Use Windows Terminal, not the legacy console window.\n")
    elif target == "linux":
        body = ("Unzip anywhere and run  ./mousiki  (or  ./lala)  in a UTF-8 terminal.\n"
                "ffmpeg and yt-dlp are included. python3 must be installed (Ubuntu: 'sudo apt install python3').\n"
                "Audio goes through PulseAudio/PipeWire-pulse or ALSA.\n"
                "The scope and spectrogram windows need SDL2 (Ubuntu: 'sudo apt install libsdl2-2.0-0').\n"
                "If the executable bit was lost on extraction:  chmod +x mousiki lala app/mousiki bin/* scripts/fpcalc\n")
    else:
        body = ("Unzip anywhere and run  ./mousiki  (or  ./lala)  in Terminal / iTerm.\n"
                "yt-dlp is included. ffmpeg is NOT:  brew install ffmpeg\n"
                "python3 comes with the Xcode Command Line Tools (xcode-select --install).\n"
                "The scope and spectrogram windows need SDL2:  brew install sdl2\n"
                "The binaries are not notarized. If macOS blocks them:  xattr -dr com.apple.quarantine <folder>\n")
    return head + body + "\n" + PORTABLE_TXT


def make_portable(stage, ws, dist, name, root_name, target, version, extra=None):
    pstage = ws / "portable-stage"
    rmtree(pstage)
    shutil.copytree(str(stage), str(pstage), symlinks=True)
    write_text(pstage / "portable.txt", PORTABLE_TXT, newline="\r\n" if target == "windows" else "\n")
    write_text(pstage / "README-PORTABLE.txt", portable_readme(target, version),
               newline="\r\n" if target == "windows" else "\n")
    if extra:
        extra(pstage)
    out = make_zip(pstage, dist / name, root_name)
    rmtree(pstage)
    return out


# --------------------------------------------------------------------------
# Windows: build + Inno Setup installer
# --------------------------------------------------------------------------
def find_iscc():
    p = which("iscc") or which("ISCC")
    if p:
        return p
    for env in ("ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"):
        base = os.environ.get(env)
        if not base:
            continue
        for sub in ("Inno Setup 6", os.path.join("Programs", "Inno Setup 6")):
            c = Path(base) / sub / "ISCC.exe"
            if c.is_file():
                return str(c)
    return None


def have_msvc():
    if which("cl"):
        return True
    pf = os.environ.get("ProgramFiles(x86)")
    vw = Path(pf) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe" if pf else None
    if vw and vw.is_file():
        out = capture([vw, "-latest", "-products", "*", "-requires",
                       "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-property", "installationPath"])
        return bool(out)
    return False


def refresh_path_windows():
    extra = [r"C:\Program Files\CMake\bin"]
    os.environ["PATH"] = os.pathsep.join(extra + [os.environ.get("PATH", "")])


def winget(pkg_id, override=None):
    cmd = ["winget", "install", "-e", "--id", pkg_id, "--accept-source-agreements", "--accept-package-agreements"]
    if override:
        cmd += ["--override", override]
    run(cmd, check=False)


def deps_windows(install):
    need = []
    if not which("cmake"):
        need.append(("cmake", "Kitware.CMake", None))
    if not have_msvc() and not which("g++"):
        need.append(("C++ build tools", "Microsoft.VisualStudio.2022.BuildTools",
                     "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"))
    if not find_iscc():
        need.append(("Inno Setup", "JRSoftware.InnoSetup", None))
    if need and install:
        if not which("winget"):
            die("winget not found - install: " + ", ".join(n[0] for n in need))
        for label, pid, ov in need:
            log("Installing " + label)
            winget(pid, ov)
        refresh_path_windows()
    elif need:
        warn("missing: %s  (re-run with --install-deps to install them via winget)" % ", ".join(n[0] for n in need))
    if not which("cmake"):
        die("cmake is not on PATH (open a NEW terminal after installing it, or use --install-deps)")
    if not have_msvc() and not which("g++"):
        die("no C++ compiler found. Install 'Visual Studio Build Tools' with the 'Desktop development with C++' workload "
            "(or run with --install-deps).")


def inno_script(stage, out_dir, base_name, version, license_file, manual_rel=None):
    s = str(stage)
    manual_icon = ""
    if manual_rel:
        manual_icon = 'Name: "{group}\\Mousiki User Manual (PDF)"; Filename: "{app}\\%s"\n' % manual_rel.replace("/", "\\")
    return r"""; generated by release.py - do not edit, re-run the toolkit instead
#define MyAppName "Mousiki"
#define MyAppVersion "%(ver)s"

[Setup]
AppId={%(guid)s
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
VersionInfoVersion=%(ver)s
DefaultDirName={autopf}\Mousiki
DefaultGroupName=Mousiki
DisableProgramGroupPage=yes
OutputDir=%(out)s
OutputBaseFilename=%(base)s
SetupIconFile=%(stage)s\mousiki.ico
UninstallDisplayIcon={app}\mousiki.ico
LicenseFile=%(lic)s
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ChangesEnvironment=yes
; upgrade over any running copy: Restart Manager closes mousiki.exe so its file is not locked
CloseApplications=force
CloseApplicationsFilter=*.exe,*.dll
RestartApplications=no
; the previous install is found through the fixed AppId and reuses its folder / per-user-or-all-users mode
UsePreviousAppDir=yes
UsePreviousPrivileges=yes
UsePreviousTasks=yes

[InstallDelete]
; clean the program folders first so files dropped in a newer version do not linger (settings live outside {app})
Type: filesandordirs; Name: "{app}\app"
Type: filesandordirs; Name: "{app}\scripts"
Type: filesandordirs; Name: "{app}\docs"
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\pylib"

[UninstallDelete]
; the optional pykakasi download is not tracked by the installer, so remove it explicitly
Type: filesandordirs; Name: "{app}\pylib"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked
Name: "addtopath"; Description: "Add Mousiki to &PATH (type 'mousiki' or 'lala' in any terminal)"
Name: "kanjireader"; Description: "Download &pykakasi so kanji are shown in Latin letters too (optional; needs internet; GPL-3.0 package from pypi.org, not part of Mousiki)"

[Files]
Source: "%(stage)s\*"; DestDir: "{app}"; Excludes: "portable.txt,data\*"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
; Start menu > Programs > Mousiki  (a folder: player, manual, install folder, uninstaller)
Name: "{group}\Mousiki"; Filename: "{app}\mousiki-wt.cmd"; IconFilename: "{app}\mousiki.ico"; WorkingDir: "{userdocs}"
%(manual_icon)sName: "{group}\Mousiki install folder"; Filename: "{app}"
Name: "{group}\Uninstall Mousiki"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Mousiki"; Filename: "{app}\mousiki-wt.cmd"; IconFilename: "{app}\mousiki.ico"; WorkingDir: "{userdocs}"; Tasks: desktopicon

[Run]
; optional: pykakasi (kanji readings) from PyPI into {app}\pylib with the bundled Python; if it fails, Mousiki just goes without
Filename: "{app}\bin\python\python.exe"; Parameters: %(q3)s{app}\scripts\install_kanji_reader.py%(q2)s %(q2)s{app}\pylib%(q3)s; StatusMsg: "Downloading pykakasi (kanji readings) ..."; Flags: runhidden waituntilterminated skipifdoesntexist; Tasks: kanjireader

[Registry]
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Tasks: addtopath; Check: UserPathMissing
Root: HKLM; Subkey: "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"; ValueType: expandsz; ValueName: "Path"; ValueData: "{olddata};{app}"; Tasks: addtopath; Check: SystemPathMissing

[Code]
const
  SysEnvKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';

function PathContains(Root: Integer; Key: String): Boolean;
var
  Paths: String;
begin
  if not RegQueryStringValue(Root, Key, 'Path', Paths) then
    Result := False
  else
    Result := Pos(';' + Uppercase(ExpandConstant('{app}')) + ';', ';' + Uppercase(Paths) + ';') > 0;
end;

function UserPathMissing: Boolean;
begin
  Result := (not IsAdminInstallMode) and (not PathContains(HKEY_CURRENT_USER, 'Environment'));
end;

function SystemPathMissing: Boolean;
begin
  Result := IsAdminInstallMode and (not PathContains(HKEY_LOCAL_MACHINE, SysEnvKey));
end;

procedure RemoveFromPath(Root: Integer; Key: String);
var
  Paths, S, P: String;
  Idx: Integer;
begin
  if not RegQueryStringValue(Root, Key, 'Path', Paths) then exit;
  P := ExpandConstant('{app}');
  S := ';' + Paths + ';';
  Idx := Pos(';' + Uppercase(P) + ';', Uppercase(S));
  if Idx = 0 then exit;
  Delete(S, Idx, Length(P) + 1);
  Paths := Copy(S, 2, Length(S) - 2);
  RegWriteExpandStringValue(Root, Key, 'Path', Paths);
end;

// Optional clean-up of the settings and cache FILES in USERPROFILE\.config\mousiki and \.cache\mousiki: only the files of
// the types the program writes there (txt, bak, json, log, pcm) directly in those two folders. Every subfolder (history,
// playlists, covers, ...) and every other file (e.g. tracks saved to the default download folder) is left alone.
procedure DeleteSettingsFiles(Dir: String);
var
  FR: TFindRec;
  E: String;
begin
  if not DirExists(Dir) then exit;
  if FindFirst(Dir + '\*', FR) then
  begin
    try
      repeat
        if (FR.Attributes and FILE_ATTRIBUTE_DIRECTORY) = 0 then
        begin
          E := Lowercase(ExtractFileExt(FR.Name));
          if ((E = '.txt') or (E = '.bak') or (E = '.json') or (E = '.log') or (E = '.pcm')) and (Lowercase(FR.Name) <> 'history.json') then
            DeleteFile(Dir + '\' + FR.Name);
        end;
      until not FindNext(FR);
    finally
      FindClose(FR);
    end;
  end;
  RemoveDir(Dir);   // only works when nothing is left in it
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Home: String;
begin
  if CurUninstallStep = usPostUninstall then
  begin
    RemoveFromPath(HKEY_CURRENT_USER, 'Environment');
    RemoveFromPath(HKEY_LOCAL_MACHINE, SysEnvKey);
    Home := GetEnv('USERPROFILE');
    if (Home <> '') and (not UninstallSilent) then
      if MsgBox('Also delete the settings and cache files of Mousiki (.config\mousiki and .cache\mousiki in your user folder)?' + #13#10 + #13#10 +
                'Your history, playlists and all other subfolders and music files stay where they are. Choose No to keep everything.',
                mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      begin
        DeleteSettingsFiles(Home + '\.config\mousiki');
        DeleteSettingsFiles(Home + '\.cache\mousiki');
      end;
  end;
end;
""" % {"ver": version, "guid": APPID_GUID, "out": str(out_dir), "base": base_name, "stage": s, "lic": str(license_file), "manual_icon": manual_icon, "q3": '"' * 3, "q2": '"' * 2}


def authenticode_status(path):
    ps = which("powershell") or which("pwsh")
    if not ps:
        return None
    return capture([ps, "-NoProfile", "-Command", "(Get-AuthenticodeSignature -LiteralPath '%s').Status" % path]) or None


def overlay_signed(stage, signed_dir):
    """Replace the unsigned exes in the stage with the ones SignPath returned."""
    signed_dir = Path(signed_dir)
    if not signed_dir.is_dir():
        die("--signed-binaries folder not found: %s" % signed_dir)
    for name, dest in (("mousiki.exe", stage / "app" / "mousiki.exe"), ("fpcalc.exe", stage / "scripts" / "fpcalc.exe")):
        found = next(iter(signed_dir.rglob(name)), None)
        if not found:
            die("%s is missing from the signed-binaries folder %s" % (name, signed_dir))
        shutil.copy2(str(found), str(dest))
        st = authenticode_status(dest)
        if st == "NotSigned":
            die("%s came back from signing but carries no signature" % name)
        if st and st != "Valid":
            warn("%s: Authenticode status is '%s' (expected 'Valid')" % (name, st))
        elif st:
            log("%s: signature valid" % name)


def build_windows(a, ctx):
    """phase full    : compile + package, everything unsigned  (local default)
       phase compile : compile only, leave mousiki.exe + fpcalc.exe in --to-sign-dir (CI, before SignPath)
       phase package : stage + portable zip + installer using the signed exes from --signed-binaries;
                       leaves the installer in --installer-to-sign-dir (CI, before the 2nd SignPath request)"""
    phase = getattr(a, "phase", "full") or "full"
    deps_windows(a.install_deps)
    ws, src, dist = ctx["ws"], ctx["src"], ctx["dist"]
    build = ws / "build"
    if phase == "package":
        exe = find_built(build, "mousiki.exe")        # compiled by the previous 'compile' phase
        if not a.signed_binaries:
            die("--phase package needs --signed-binaries <folder with the signed mousiki.exe and fpcalc.exe>")
    else:
        cmake_build(src, build)
        exe = find_built(build, "mousiki.exe")
    if phase == "compile":
        ts = Path(a.to_sign_dir) if a.to_sign_dir else ws / "to-sign"
        rmtree(ts)
        ts.mkdir(parents=True)
        out = [ts / "mousiki.exe"]
        shutil.copy2(str(exe), str(out[0]))
        fp = exe.parent / "scripts" / "fpcalc.exe"
        if fp.is_file():
            shutil.copy2(str(fp), str(ts / "fpcalc.exe"))
            out.append(ts / "fpcalc.exe")
        else:
            warn("fpcalc.exe was not built")
        log("Binaries to sign are in %s" % ts)
        return out
    stage = ws / "stage"
    rmtree(stage)
    stage_common(stage, src, exe, "windows")
    if phase == "package":
        overlay_signed(stage, a.signed_binaries)
    write_launchers(stage, "windows")
    if not a.no_bundle_tools:
        bundle_windows_tools(stage, ctx["dl"], a)
    arch = "arm64" if platform.machine().lower() == "arm64" else "x64"
    ver = ctx["version"]
    outputs = []
    outputs.append(make_portable(stage, ws, dist, "Mousiki-%s-windows-%s-portable.zip" % (ver, arch),
                                 "Mousiki-%s" % ver, "windows", ver))
    iscc = find_iscc()
    if not iscc:
        die("Inno Setup (ISCC.exe) not found - the portable .zip was built, the installer was not.\n"
            "    Install it with:  winget install -e --id JRSoftware.InnoSetup   (or use --install-deps)", 2)
    if not (stage / "mousiki.ico").is_file():
        die("mousiki.ico missing from the source tree - required for the installer")
    manual = find_manual_pdf(stage)
    manual_rel = manual.relative_to(stage).as_posix() if manual else None
    iss_dir = ws / "installer"
    iss_dir.mkdir(parents=True, exist_ok=True)
    base = "Mousiki-%s-windows-%s-setup" % (ver, arch)
    iss = iss_dir / "mousiki.iss"
    write_text(iss, inno_script(stage, dist, base, ver, stage / "LICENSE", manual_rel), newline="\r\n")
    run([iscc, "/Qp", iss])
    setup = dist / (base + ".exe")
    outputs.append(setup)
    if phase == "package":
        its = Path(a.installer_to_sign_dir) if a.installer_to_sign_dir else ws / "installer-to-sign"
        rmtree(its)
        its.mkdir(parents=True)
        shutil.copy2(str(setup), str(its / setup.name))
        log("Installer to sign is in %s" % its)
    rmtree(stage)
    return outputs


# --------------------------------------------------------------------------
# Linux: build + .deb
# --------------------------------------------------------------------------
def deps_linux(install):
    need = [t for t in ("cmake", "g++", "dpkg-deb") if not which(t)]
    if which("apt-get") is None and need:
        die("not a Debian/Ubuntu system (no apt-get) - the .deb target needs one. Missing: " + ", ".join(need))
    if install:
        sudo = [] if os.geteuid() == 0 else ["sudo"]
        run(sudo + ["apt-get", "update"])
        run(sudo + ["apt-get", "install", "-y", "build-essential", "cmake", "python3", "python3-pip", "dpkg-dev"])
    elif need:
        die("missing tools: %s\n    Install with:  sudo apt-get install -y build-essential cmake python3 python3-pip dpkg-dev\n"
            "    (or re-run with --install-deps)" % ", ".join(need))


DEB_DESKTOP = """[Desktop Entry]
Type=Application
Name=Mousiki
Comment=Terminal music player
Exec=mousiki
Terminal=true
Icon=audio-x-generic
Categories=X-Mousiki;
Keywords=music;player;tui;terminal;
"""

DEB_DESKTOP_MANUAL = """[Desktop Entry]
Type=Application
Name=Mousiki User Manual
Comment=Mousiki user guide (PDF)
Exec=xdg-open /usr/share/doc/mousiki/Mousiki_User_Guide.pdf
Terminal=false
Icon=application-pdf
Categories=X-Mousiki;
Keywords=mousiki;manual;help;guide;documentation;
"""

DEB_DIRECTORY = """[Desktop Entry]
Type=Directory
Name=Mousiki
Icon=audio-x-generic
"""

DEB_MENU = """<!DOCTYPE Menu PUBLIC "-//freedesktop//DTD Menu 1.0//EN"
 "http://www.freedesktop.org/standards/menu-spec/menu-1.0.dtd">
<Menu>
  <Name>Applications</Name>
  <Menu>
    <Name>Mousiki</Name>
    <Directory>mousiki.directory</Directory>
    <Include><Category>X-Mousiki</Category></Include>
  </Menu>
</Menu>
"""


def build_deb(stage, ws, dist, version):
    arch = linux_arch()
    root = ws / "debroot"
    rmtree(root)
    opt = root / "opt" / "mousiki"
    shutil.copytree(str(stage), str(opt), symlinks=True)
    (root / "usr" / "bin").mkdir(parents=True)
    os.symlink("/opt/mousiki/mousiki", str(root / "usr" / "bin" / "mousiki"))
    os.symlink("/opt/mousiki/lala", str(root / "usr" / "bin" / "lala"))          # second command name
    write_text(root / "usr" / "share" / "applications" / "mousiki.desktop", DEB_DESKTOP)
    doc = root / "usr" / "share" / "doc" / "mousiki"
    doc.mkdir(parents=True)
    manual = find_manual_pdf(stage)
    if manual:
        # one real copy under /opt/mousiki/docs, reachable from the standard doc folder and the app menu
        os.symlink("/opt/mousiki/docs/" + manual.relative_to(stage / "docs").as_posix(),
                   str(doc / "Mousiki_User_Guide.pdf"))
        write_text(root / "usr" / "share" / "applications" / "mousiki-manual.desktop", DEB_DESKTOP_MANUAL)
    # "Mousiki" folder in the application menu of KDE / XFCE / MATE / LXQt (GNOME has no menu folders:
    # there both entries simply appear in the app grid)
    write_text(root / "usr" / "share" / "desktop-directories" / "mousiki.directory", DEB_DIRECTORY)
    write_text(root / "etc" / "xdg" / "menus" / "applications-merged" / "mousiki.menu", DEB_MENU)
    if (stage / "LICENSE").is_file():
        shutil.copy2(str(stage / "LICENSE"), str(doc / "copyright"))
    if (stage / THIRD_PARTY_LICENSES).is_file():
        os.symlink("/opt/mousiki/" + THIRD_PARTY_LICENSES, str(doc / THIRD_PARTY_LICENSES))
    for dp, _dns, fns in os.walk(str(root)):
        os.chmod(dp, 0o755)
        for fn in fns:
            p = Path(dp) / fn
            if p.is_symlink():
                continue
            os.chmod(str(p), 0o755 if os.access(str(p), os.X_OK) else 0o644)
    size_kb = sum(p.stat().st_size for p in root.rglob("*") if p.is_file() and not p.is_symlink()) // 1024
    control = """Package: mousiki
Version: %s
Section: sound
Priority: optional
Architecture: %s
Installed-Size: %d
Maintainer: Mousiki release toolkit <noreply@localhost>
Depends: libc6, libstdc++6, python3, ffmpeg, curl, libasound2 | libasound2t64, libpulse0
Recommends: pulseaudio-utils | pipewire-pulse, xclip | wl-clipboard, xdg-utils, libsdl2-2.0-0, fonts-cascadia-code
Homepage: https://github.com/StSchwerdtfeger/mousiki-plus
Description: Terminal music player and online radio (TUI)
 Mousiki plays local files and online sources (via the bundled yt-dlp) in the
 terminal, with spectrum/oscilloscope visualizers, synced lyrics, an equalizer,
 playlist and metadata editors and AcoustID tagging. SHIFT and + switch to the
 built-in internet radio (presets, station lists, Radio Browser search, recording).
""" % (version, arch, size_kb)
    write_text(root / "DEBIAN" / "control", control)
    os.chmod(str(root / "DEBIAN"), 0o755)
    out = dist / ("Mousiki-%s-linux-%s.deb" % (version, arch))
    if out.exists():
        out.unlink()
    run(["dpkg-deb", "--root-owner-group", "--build", root, out])
    rmtree(root)
    return out


def build_linux(a, ctx):
    deps_linux(a.install_deps)
    ws, src, dist = ctx["ws"], ctx["src"], ctx["dist"]
    build = ws / "build"
    cmake_build(src, build)
    exe = find_built(build, "mousiki")
    if which("strip"):
        run(["strip", exe], check=False)
        fp = exe.parent / "scripts" / "fpcalc"
        if fp.is_file():
            run(["strip", fp], check=False)
    stage = ws / "stage"
    rmtree(stage)
    stage_common(stage, src, exe, "linux")
    write_launchers(stage, "linux")
    if not a.no_bundle_tools:
        bundle_unix_tools(stage, ctx["dl"], a, "linux")
    ver, arch = ctx["version"], linux_arch()
    outputs = [build_deb(stage, ws, dist, ver)]
    extra = None if a.no_bundle_tools else (lambda p: add_static_ffmpeg_linux(p, ctx["dl"], a))
    outputs.append(make_portable(stage, ws, dist, "Mousiki-%s-linux-%s-portable.zip" % (ver, arch),
                                 "Mousiki-%s" % ver, "linux", ver, extra))
    rmtree(stage)
    return outputs


# --------------------------------------------------------------------------
# macOS: build + Mousiki.app on a .dmg
# --------------------------------------------------------------------------
def deps_macos(install):
    if not capture(["xcode-select", "-p"]):
        die("Xcode Command Line Tools missing - run: xcode-select --install")
    if not which("cmake"):
        if install and which("brew"):
            run(["brew", "install", "cmake"])
        else:
            die("cmake missing - run: brew install cmake  (or use --install-deps)")
    for t in ("hdiutil", "codesign", "sips"):
        if not which(t):
            die("%s not found - this must run on macOS" % t)


CASCADIA_VERSION = "2407.24"   # github.com/microsoft/cascadia-code releases; 'release.py pin' fixes the file and its SHA-256
CASCADIA_ZIP = "CascadiaCode-%s.zip" % CASCADIA_VERSION
CASCADIA_URL = "https://github.com/microsoft/cascadia-code/releases/download/v%s/%s" % (CASCADIA_VERSION, CASCADIA_ZIP)
CASCADIA_LICENSE = HERE / "fonts" / "CascadiaCode-OFL.txt"   # SIL Open Font License 1.1, shipped next to the font


def fetch_cascadia_mono(dl, a):
    """Cascadia Mono (the font of the README pictures: full braille dots) as {file name: bytes}; {} when it cannot be had.
    The variable font when the release has it, else the static Regular and Bold."""
    try:
        z = fetch_pinned(CASCADIA_ZIP, dl / CASCADIA_ZIP, a)
        if z is None:
            z = fetch(CASCADIA_URL, dl / CASCADIA_ZIP, a.refresh_tools, tries=2)
    except DownloadFailed:   # (a SHA-256 mismatch stops the build)
        warn("Cascadia Mono could not be downloaded - the app is built without the font")
        return {}
    out = {}
    with zipfile.ZipFile(str(z)) as zf:
        names = zf.namelist()
        var = [n for n in names if n.lower().endswith("ttf/cascadiamono.ttf")]
        pick = var or [n for n in names if n.lower().split("/")[-1] in ("cascadiamono-regular.ttf", "cascadiamono-bold.ttf")]
        for n in pick:
            out[n.split("/")[-1]] = zf.read(n)
    if not out:
        warn("Cascadia Mono not found in %s - the app is built without the font" % z.name)
    return out


# Mousiki.app/Contents/MacOS/Mousiki: copies the bundled font into ~/Library/Fonts once (per user, no password), then opens
# Mousiki in a new Terminal window. Mousiki itself sets the font of that window (Cascadia Mono; terminal_font.cpp) and gives it back when it quits.
MAC_APP_LAUNCHER = r"""#!/bin/sh
CONTENTS="$(cd "$(dirname "$0")/.." && pwd)"
RES="$CONTENTS/Resources"
if [ -d "$RES/fonts" ]; then
    mkdir -p "$HOME/Library/Fonts"
    for f in "$RES"/fonts/*.ttf; do
        [ -e "$f" ] || continue
        [ -e "$HOME/Library/Fonts/$(basename "$f")" ] || cp "$f" "$HOME/Library/Fonts/" 2>/dev/null
    done
fi
exec open -a Terminal "$RES/Mousiki.command"
"""

MAC_APP_COMMAND = """#!/bin/sh
# Starts Mousiki in a Terminal window (used when the app may not control Terminal).
exec "$(cd "$(dirname "$0")" && pwd)/mousiki/mousiki"
"""

# On the disk image: the two terminal commands (optional), and the clean-up of an install by the old .pkg (up to v4.7.x).
MAC_INSTALL_COMMANDS = """#!/bin/sh
APP="/Applications/Mousiki.app/Contents/Resources/mousiki"
echo "This adds the terminal commands  mousiki  and  lala  (links in /usr/local/bin to Mousiki.app)."
echo "Drag Mousiki.app into Applications first. Your Mac password is asked for."
if [ ! -x "$APP/mousiki" ]; then echo "Mousiki.app is not in /Applications yet."; exit 1; fi
sudo mkdir -p /usr/local/bin
sudo ln -sf "$APP/mousiki" /usr/local/bin/mousiki
sudo ln -sf "$APP/lala" /usr/local/bin/lala
echo "Done. Open a new Terminal window and type  mousiki  (or  lala)."
echo "(When you move Mousiki.app to the Bin, these two links stay behind and do nothing; remove them with"
echo " sudo rm /usr/local/bin/mousiki /usr/local/bin/lala )"
"""

MAC_REMOVE_OLD_PKG = """#!/bin/sh
echo "This removes Mousiki installed by the older .pkg installer (up to v4.7.x):"
echo "/usr/local/lib/mousiki, the /Applications/Mousiki folder and its mousiki / lala commands."
echo "Mousiki.app and your settings in ~/.config/mousiki are kept."
printf "Continue? [y/N] "
read ans
case "$ans" in y|Y) ;; *) echo "Cancelled."; exit 0 ;; esac
for c in mousiki lala; do
    case "$(readlink /usr/local/bin/$c 2>/dev/null)" in *lib/mousiki/*) sudo rm -f /usr/local/bin/$c ;; esac
done
sudo rm -rf /usr/local/lib/mousiki /Applications/Mousiki
sudo pkgutil --forget io.github.mousiki.player >/dev/null 2>&1
echo "The old installation was removed."
"""

MAC_INFO_PLIST = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key><string>Mousiki</string>
    <key>CFBundleDisplayName</key><string>Mousiki</string>
    <key>CFBundleIdentifier</key><string>io.github.mousiki.player</string>
    <key>CFBundleVersion</key><string>@@VER@@</string>
    <key>CFBundleShortVersionString</key><string>@@VER@@</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleExecutable</key><string>Mousiki</string>
    @@ICON@@<key>LSMinimumSystemVersion</key><string>10.15</string>
    <key>NSAppleEventsUsageDescription</key><string>Mousiki sets the font of its own Terminal window (and gives the old one back when it quits).</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
"""


def mac_icon(src, res):
    """mousiki.ico -> Contents/Resources/mousiki.icns with the tools macOS has (sips); False when that does not work."""
    ico = src / "mousiki.ico"
    if not ico.is_file():
        return False
    png = res / "icon-tmp.png"
    try:
        ok = subprocess.call(["sips", "-s", "format", "png", str(ico), "--out", str(png)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0
        ok = ok and subprocess.call(["sips", "-s", "format", "icns", str(png), "--out", str(res / "mousiki.icns")],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0
    except OSError:
        ok = False
    if png.exists():
        png.unlink()
    return ok and (res / "mousiki.icns").is_file()


def build_mac_app(stage, src, app, ver, fonts):
    """Mousiki.app: everything inside the bundle (program, tools, scripts, font, manual), so moving the app to the Bin removes it."""
    rmtree(app)
    contents = app / "Contents"
    res = contents / "Resources"
    (contents / "MacOS").mkdir(parents=True)
    res.mkdir(parents=True)
    shutil.copytree(str(stage), str(res / "mousiki"), symlinks=True)
    write_text(contents / "MacOS" / "Mousiki", MAC_APP_LAUNCHER, executable=True)
    write_text(res / "Mousiki.command", MAC_APP_COMMAND, executable=True)
    if fonts:
        (res / "fonts").mkdir()
        for name, data in fonts.items():
            (res / "fonts" / name).write_bytes(data)
        shutil.copy2(str(CASCADIA_LICENSE), str(res / "fonts" / "CascadiaCode-OFL.txt"))
        with open(str(res / "mousiki" / "THIRD-PARTY.txt"), "a", encoding="utf-8") as f:
            f.write("- Cascadia Mono %s (SIL Open Font License 1.1, Microsoft) - https://github.com/microsoft/cascadia-code - "
                    "Mousiki.app/Contents/Resources/fonts\n" % CASCADIA_VERSION)
    manual = find_manual_pdf(stage)
    if manual:
        shutil.copy2(str(manual), str(res / "Mousiki User Manual.pdf"))
    icon = mac_icon(src, res)
    plist = MAC_INFO_PLIST.replace("@@VER@@", ver)
    plist = plist.replace("@@ICON@@", "<key>CFBundleIconFile</key><string>mousiki</string>\n    " if icon else "")
    write_text(contents / "Info.plist", plist)
    if not icon:
        warn("no app icon (sips could not convert mousiki.ico) - the app shows the generic icon")


def build_mac_dmg(app, ws, out, ver):
    """The disk image: Mousiki.app, a link to Applications (drag and drop), the manual and two optional helper commands."""
    root = ws / "dmgroot"
    rmtree(root)
    root.mkdir(parents=True)
    shutil.move(str(app), str(root / "Mousiki.app"))
    os.symlink("/Applications", str(root / "Applications"))
    manual = root / "Mousiki.app" / "Contents" / "Resources" / "Mousiki User Manual.pdf"
    if manual.is_file():
        shutil.copy2(str(manual), str(root / "Mousiki User Manual.pdf"))
    write_text(root / "Install Terminal Commands (optional).command", MAC_INSTALL_COMMANDS, executable=True)
    write_text(root / "Remove Old Mousiki (pkg install).command", MAC_REMOVE_OLD_PKG, executable=True)
    if out.exists():
        out.unlink()
    run(["hdiutil", "create", "-volname", "Mousiki %s" % ver, "-srcfolder", str(root), "-ov", "-format", "UDZO", str(out)])
    rmtree(root)


def build_macos_one(a, ctx, label, archs):
    """One macOS flavour: its own build tree, its own Mousiki.app on a .dmg, its own portable zip.
    label = the name in the file names (universal / arm64 / x86_64), archs = CMAKE_OSX_ARCHITECTURES ("" = this Mac's own)."""
    ws, src, dist = ctx["ws"], ctx["src"], ctx["dist"]
    build = ws / ("build" if label == "universal" else "build-" + label)   # (universal keeps the old tree name: incremental builds stay)
    log("macOS %s: %s" % (label, archs or "this Mac's own architecture"))
    cmake_build(src, build, ["-DCMAKE_OSX_ARCHITECTURES=" + archs] if archs else [])
    exe = find_built(build, "mousiki")
    stage = ws / "stage"
    rmtree(stage)
    stage_common(stage, src, exe, "macos")
    write_launchers(stage, "macos")
    if not a.no_bundle_tools:
        bundle_unix_tools(stage, ctx["dl"], a, "macos")   # (yt-dlp_macos is one universal binary: the same in every flavour)
    run(["xattr", "-cr", stage], check=False)
    sign_id = a.sign_identity or os.environ.get("MOUSIKI_CODESIGN_ID") or "-"
    sign_opts = ["--options", "runtime", "--timestamp"] if sign_id != "-" else []
    for f in (stage / "app" / "mousiki", stage / "scripts" / "fpcalc"):
        if f.is_file():
            run(["codesign", "--force", "--sign", sign_id] + sign_opts + [f], check=False)
    if sign_id == "-":
        warn("binaries are only ad-hoc signed (no Apple Developer ID) - Gatekeeper will warn on other Macs")
    ver = ctx["version"]
    outputs = []
    # the app (with the font) on a disk image: drag it to Applications; moving it to the Bin removes Mousiki again
    app = ws / "Mousiki.app"
    build_mac_app(stage, src, app, ver, fetch_cascadia_mono(ctx["dl"], a))
    run(["xattr", "-cr", app], check=False)
    run(["codesign", "--force", "--deep", "--sign", sign_id] + sign_opts + [app], check=False)
    out = dist / ("Mousiki-%s-macos-%s.dmg" % (ver, label))
    build_mac_dmg(app, ws, out, ver)
    outputs.append(out)
    outputs.append(make_portable(stage, ws, dist, "Mousiki-%s-macos-%s-portable.zip" % (ver, label),
                                 "Mousiki-%s" % ver, "macos", ver))
    rmtree(stage)
    return outputs


def build_macos(a, ctx):
    """--mac-arch both (default): the universal app (Intel + Apple silicon in one) and a separate arm64 one for M chips,
    each as .dmg + portable zip. universal / arm64 / x86_64 build only that one, native = this Mac's own architecture."""
    deps_macos(a.install_deps)
    host = "arm64" if platform.machine().lower() == "arm64" else "x86_64"
    flavours = {
        "both": [("universal", "arm64;x86_64"), ("arm64", "arm64")],
        "universal": [("universal", "arm64;x86_64")],
        "arm64": [("arm64", "arm64")],
        "x86_64": [("x86_64", "x86_64")],
        "native": [(host, "")],
    }[a.mac_arch]
    outputs = []
    for label, archs in flavours:
        outputs += build_macos_one(a, ctx, label, archs)
    return outputs


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------
def archive_old_dist(dist, version):
    """Move release files of other versions out of dist (into dist/older/<version>/), so only the current version is
    left to upload and SHA256SUMS.txt lists only it. Files without a version number (SHA256SUMS, the key) stay."""
    moved = 0
    for p in sorted(dist.iterdir()):
        if not p.is_file():
            continue
        m = re.search(r"(?<![\d.])(\d+\.\d+\.\d+)(?![\d])", p.name)
        if not m or m.group(1) == version:
            continue
        target = dist / "older" / m.group(1)
        target.mkdir(parents=True, exist_ok=True)
        dest = target / p.name
        if dest.exists():
            dest.unlink()
        shutil.move(str(p), str(dest))
        moved += 1
    if moved:
        log("Moved %d file(s) of other versions from %s to %s" % (moved, dist.name, (dist / "older").name + "/<version>"))
        for stale in ("SHA256SUMS.txt", "SHA256SUMS.txt.asc"):   # they described the old file list
            sp = dist / stale
            if sp.is_file():
                sp.unlink()


def write_checksums(dist):
    pats = ("*.exe", "*.zip", "*.deb", "*.dmg", "*.pkg", "*.cdx.json")
    files = sorted({p for pat in pats for p in dist.glob(pat)})
    if not files:
        return
    sums = dist / "SHA256SUMS.txt"
    text = "".join("%s  %s\n" % (sha256_of(p), p.name) for p in files)
    old = sums.read_text(encoding="utf-8") if sums.is_file() else None
    if old == text:
        return                                  # unchanged: a signature made for it stays valid
    write_text(sums, text)
    asc = dist / "SHA256SUMS.txt.asc"
    if asc.exists():                            # signed a different list: it would no longer verify
        asc.unlink()
        warn("SHA256SUMS.txt changed, its old signature was removed - sign again: release.py checksums --gpg-key KEYID")


def gpg_binary():
    return which("gpg") or which("gpg2")


def gpg_key_of(a):
    return getattr(a, "gpg_key", None) or os.environ.get("MOUSIKI_GPG_KEY")


def gpg_sign_release(dist, key):
    """Detached, ASCII-armoured signatures for SHA256SUMS.txt (covers every file) and for every package
    (.exe / .zip / .deb / .dmg) and the SBOM (.cdx.json), plus the public key.
    Verify with:  gpg --verify SHA256SUMS.txt.asc SHA256SUMS.txt   or   gpg --verify <package>.asc <package>"""
    gpg = gpg_binary()
    if not gpg:
        die("gpg not found. Windows: winget install GnuPG.Gpg4win | Ubuntu: sudo apt install gnupg")
    sums = dist / "SHA256SUMS.txt"
    if not sums.is_file():
        die("no SHA256SUMS.txt in %s - nothing to sign" % dist)
    packages = sorted({p for pat in ("*.exe", "*.zip", "*.deb", "*.dmg", "*.pkg", "*.cdx.json") for p in dist.glob(pat)})
    for t in [sums] + packages:
        asc = t.with_name(t.name + ".asc")
        if asc.exists():
            asc.unlink()
        run([gpg, "--yes", "--armor", "--detach-sign", "--local-user", key, "--output", asc, t])
        run([gpg, "--verify", asc, t])
    pub = dist / "mousiki-release-key.asc"
    run([gpg, "--yes", "--armor", "--export", "--output", pub, key])
    fpr = capture([gpg, "--fingerprint", key])
    log("Signed with GPG key %s" % key)
    if fpr:
        print(fpr)
    print("    Publish this fingerprint (README / release notes) so users can check the key.")


def cmd_checksums(a):
    dist = Path(a.dist).expanduser().resolve()
    write_checksums(dist)
    key = gpg_key_of(a)
    if key:
        gpg_sign_release(dist, key)
    else:
        log("No GPG key given (--gpg-key or MOUSIKI_GPG_KEY) - checksums are not signed")


def resolve_source(a):
    """('dir', path) for --src (CI builds straight from the git checkout) else ('zip', path)."""
    if getattr(a, "src", None):
        return "dir", Path(a.src).expanduser().resolve()
    return "zip", resolve_zip(a)


def resolve_zip(a):
    if a.zip:
        return Path(a.zip).expanduser().resolve()
    cands = sorted((HERE / "input").glob("*.zip"), key=lambda p: p.stat().st_mtime, reverse=True)
    if cands:
        log("Using source zip: %s" % cands[0].name)
        return cands[0].resolve()
    die("no source zip given. Pass --zip <file> or drop the zip into %s" % (HERE / "input"))


def cmd_build(a):
    target = a.target or host_target()
    if target != host_target():
        die("a %s build has to run on %s (this is %s). Use 'all', 'linux-wsl' or 'cloud' instead."
            % (target, target, host_target()))
    kind, source = resolve_source(a)
    work = Path(a.work).expanduser().resolve()
    dist = Path(a.dist).expanduser().resolve()
    dist.mkdir(parents=True, exist_ok=True)
    ws = work / target
    dl = work / "downloads"
    packaging_phase = target == "windows" and getattr(a, "phase", "full") == "package"
    if a.clean_all and not packaging_phase:
        rmtree(work)
    elif a.clean and not packaging_phase:       # the package phase must keep the compile phase's build tree
        rmtree(ws)
    ws.mkdir(parents=True, exist_ok=True)
    dl.mkdir(parents=True, exist_ok=True)
    src = sync_zip(source, ws / "src") if kind == "zip" else sync_dir(source, ws / "src")
    version, cmake_ver = detect_version(src, a.version)
    archive_old_dist(dist, version)
    if a.sync_version:
        sync_version(src, version, cmake_ver)
    log("Building Mousiki %s for %s%s" % (version, target, "  (clean rebuild)" if a.clean or a.clean_all else "  (incremental)"))
    ctx = {"ws": ws, "src": src, "dist": dist, "dl": dl, "version": version}
    t0 = time.time()
    outputs = {"windows": build_windows, "linux": build_linux, "macos": build_macos}[target](a, ctx)
    write_checksums(dist)
    if gpg_key_of(a) and any(dist.glob("*.deb")):
        gpg_sign_release(dist, gpg_key_of(a))
    log("Done in %.0fs. Output:" % (time.time() - t0))
    for p in outputs:
        print("    %-60s %s" % (p.name, human(p.stat().st_size)))
    return outputs


# ---- Linux via WSL ----------------------------------------------------------
def wsl_base(distro):
    return ["wsl.exe"] + (["-d", distro] if distro else [])


def wslpath(distro, p):
    out = capture(wsl_base(distro) + ["--", "wslpath", "-a", str(p).replace("\\", "/")])
    if not out:
        die("wslpath failed for %s - is the WSL distribution running?" % p)
    return out


def cmd_linux_wsl(a):
    if host_target() != "windows":
        die("linux-wsl is for Windows hosts; on Linux use: build --target linux")
    if not which("wsl.exe"):
        die("WSL is not installed. In an admin PowerShell run:  wsl --install -d Ubuntu   (then reboot, create the user, re-run)")
    kind, source = resolve_source(a)
    dist = Path(a.dist).expanduser().resolve()
    dist.mkdir(parents=True, exist_ok=True)
    w_script = wslpath(a.distro, HERE / "release.py")
    w_zip = wslpath(a.distro, source)
    w_dist = wslpath(a.distro, dist)
    flags = []
    for name, val in (("--clean", a.clean), ("--clean-all", a.clean_all), ("--refresh-tools", a.refresh_tools),
                      ("--no-bundle-tools", a.no_bundle_tools), ("--install-deps", a.install_deps),
                      ("--sync-version", a.sync_version)):
        if val:
            flags.append(name)
    if a.version:
        flags += ["--version", a.version]
    inner = ("python3 %s build --target linux " + ("--zip" if kind == "zip" else "--src") + " %s --dist %s --work \"$HOME/.cache/mousiki-release\" %s") % (
        shlex.quote(w_script), shlex.quote(w_zip), shlex.quote(w_dist), " ".join(shlex.quote(f) for f in flags))
    log("Building the Linux packages inside WSL (build tree lives in the WSL home for speed)")
    rc = run(wsl_base(a.distro) + ["--", "bash", "-lc", inner], check=False)
    if rc != 0:
        die("the WSL build failed (see output above). If tools are missing, retry with --install-deps.")
    return []


# ---- cloud (GitHub Actions) ---------------------------------------------------
def check_repo_pushed():
    """Builds from the repo only see what has been pushed - warn if the local tree is ahead."""
    if not capture(["git", "-C", str(HERE), "rev-parse", "--show-toplevel"]):
        warn("packaging/ is not inside a git repository - cannot check what GitHub will build")
        return
    if capture(["git", "-C", str(HERE), "status", "--porcelain"]):
        warn("uncommitted local changes exist - GitHub builds only what you have pushed (see: update-repo)")
    ahead = capture(["git", "-C", str(HERE), "rev-list", "--count", "@{u}..HEAD"])
    if ahead not in ("", "0"):
        warn("%s local commit(s) are not pushed yet - GitHub will not see them" % ahead)


def cmd_cloud(a):
    gh = which("gh")
    if not gh:
        die("GitHub CLI not found. Install it (winget install GitHub.cli), then run: gh auth login")
    repo = a.repo or os.environ.get("MOUSIKI_RELEASE_REPO")
    if not repo:
        die("pass --repo <owner/name> (the GitHub repo that holds the source + packaging/) or set MOUSIKI_RELEASE_REPO")
    targets = [t.strip() for t in a.targets.split(",") if t.strip()]
    for t in targets:
        if t not in ("windows", "linux", "macos"):
            die("unknown target '%s'" % t)
    sign = bool(a.sign_windows) and "windows" in targets
    # Signed builds must come from the repository itself: SignPath verifies the origin (repo + workflow + commit).
    from_repo = sign or a.from_repo
    dist = Path(a.dist).expanduser().resolve()
    dist.mkdir(parents=True, exist_ok=True)
    work = Path(a.work).expanduser().resolve()
    work.mkdir(parents=True, exist_ok=True)
    if from_repo:
        log("Building from the repository checkout%s" % (" (signed Windows build)" if sign else ""))
        check_repo_pushed()
    else:
        zip_path = resolve_zip(a)
        asset = work / CLOUD_ASSET
        shutil.copy2(str(zip_path), str(asset))
        log("Uploading the source zip to %s (release '%s')" % (repo, CLOUD_TAG))
        if run([gh, "release", "view", CLOUD_TAG, "--repo", repo], check=False) != 0:
            run([gh, "release", "create", CLOUD_TAG, "--repo", repo, "--title", "Mousiki build source",
                 "--notes", "Source zip consumed by the build-installers workflow (replaced on every run)."])
        run([gh, "release", "upload", CLOUD_TAG, asset, "--repo", repo, "--clobber"])

    log("Starting workflow for: " + ", ".join(targets))
    started = time.time() - 5
    cmd = [gh, "workflow", "run", WORKFLOW, "--repo", repo, "-f", "targets=" + ",".join(targets),
           "-f", "version=" + (a.version or ""), "-f", "clean=" + ("true" if (a.clean or a.clean_all) else "false"),
           "-f", "sign=" + ("true" if sign else "false"), "-f", "source=" + ("checkout" if from_repo else "zip")]
    if a.ref:
        cmd += ["--ref", a.ref]
    run(cmd)
    run_id = None
    for _ in range(40):
        time.sleep(3)
        out = capture([gh, "run", "list", "--repo", repo, "--workflow", WORKFLOW, "--limit", "5",
                       "--json", "databaseId,createdAt,event"])
        try:
            rows = json.loads(out or "[]")
        except ValueError:
            rows = []
        for r in rows:
            ts = calendar.timegm(time.strptime(r["createdAt"], "%Y-%m-%dT%H:%M:%SZ"))
            if ts >= started and r.get("event") == "workflow_dispatch":
                run_id = str(r["databaseId"])
                break
        if run_id:
            break
    if not run_id:
        die("could not find the started workflow run - check the Actions tab of " + repo)
    if sign:
        log("Signing requests may need your approval in the SignPath web portal - the run waits for it (up to 30 min per request)")
    log("Waiting for run %s (this takes a few minutes)" % run_id)
    rc = run([gh, "run", "watch", run_id, "--repo", repo, "--exit-status"], check=False)
    tmp = work / "cloud-artifacts"
    rmtree(tmp)
    # only the final artifact (the intermediate signing artifacts hold loose mousiki.exe / fpcalc.exe)
    run([gh, "run", "download", run_id, "--repo", repo, "--name", RELEASE_ARTIFACT, "--dir", tmp], check=False)
    got = []
    if tmp.is_dir():
        for p in tmp.rglob("*"):
            ext = p.suffix.lower()
            if p.is_file() and (ext in (".exe", ".zip", ".deb", ".dmg", ".pkg") or p.name.endswith(".cdx.json")):   # packages + the SBOM
                shutil.copy2(str(p), str(dist / p.name))
                got.append(dist / p.name)
            elif p.is_file() and (p.name in ("mousiki-release-key.asc", "SHA256SUMS.txt", "SHA256SUMS.txt.asc")
                                  or (ext == ".asc" and p.name[:-4].lower().endswith((".exe", ".zip", ".deb", ".dmg", ".pkg", ".cdx.json")))):
                shutil.copy2(str(p), str(dist / p.name))      # checksums + GPG signatures made in the workflow
            elif p.is_file() and p.name.endswith(".intoto.jsonl"):
                shutil.copy2(str(p), str(dist / p.name))      # provenance bundle
    write_checksums(dist)
    if rc != 0:
        die("the cloud build reported failures - open https://github.com/%s/actions/runs/%s" % (repo, run_id))
    log("Downloaded %d file(s) from the cloud build:" % len(got))
    for p in got:
        print("    %-60s %s" % (p.name, human(p.stat().st_size)))
    return got


def cmd_update_repo(a):
    """Copy a new source zip over the git working tree (keeping .git, .github, packaging), commit and push."""
    top = capture(["git", "-C", str(HERE), "rev-parse", "--show-toplevel"])
    if not top:
        die("packaging/ is not inside a git repository")
    root = Path(top).resolve()
    if HERE.resolve().parent != root:
        die("expected release.py in <repo>/packaging (found it in %s, repo root is %s)" % (HERE, root))
    zip_path = resolve_zip(a)
    sync_zip(zip_path, root, keep=REPO_KEEP)
    run(["git", "-C", root, "add", "-A"])
    if not capture(["git", "-C", root, "status", "--porcelain"]):
        log("The repository already matches this zip - nothing to commit")
        return
    version = detect_version(root, a.version)[0]
    run(["git", "-C", root, "commit", "-m", a.message or ("Update source to v%s" % version)])
    if a.no_push:
        log("Committed locally (not pushed because of --no-push)")
    else:
        run(["git", "-C", root, "push"])
        log("Pushed v%s" % version)


def cmd_all(a):
    host = host_target()
    wanted = [t.strip() for t in a.targets.split(",") if t.strip()]
    results, failed, cloud = [], [], []
    if a.update_repo:
        cmd_update_repo(a)
    for t in wanted:
        try:
            if t == host and not (t == "windows" and a.sign_windows):
                a.target = t
                results += cmd_build(a)
            elif host == "windows" and t == "linux" and not a.no_wsl and which("wsl.exe"):
                cmd_linux_wsl(a)
                results.append(t + " (via WSL)")
            else:
                cloud.append(t)
        except SystemExit as e:
            failed.append(t)
            warn("%s build failed (exit %s) - continuing" % (t, e.code))
    if cloud:
        if a.repo or os.environ.get("MOUSIKI_RELEASE_REPO"):
            a.targets = ",".join(cloud)
            try:
                cmd_cloud(a)
            except SystemExit:
                failed.extend(cloud)
        else:
            warn("%s not built: there is no local way to build it on %s. Pass --repo owner/name to build it on "
                 "GitHub Actions (see README)." % (", ".join(cloud), host))
            failed.extend(cloud)
    dist = Path(a.dist).expanduser().resolve()
    write_checksums(dist)
    key = gpg_key_of(a)
    if key:
        try:
            gpg_sign_release(dist, key)
        except SystemExit:
            failed.append("gpg-signing")
    print()
    log("Summary - dist folder: %s" % dist)
    for p in sorted(dist.glob("*")):
        if p.is_file():
            print("    %-60s %s" % (p.name, human(p.stat().st_size)))
    if failed:
        sys.exit("Not completed: " + ", ".join(failed))


KANJI_PINS_FILE = HERE.parent / "scripts" / "kanji_pins.json"
KANJI_PACKAGES = ("pykakasi", "jaconv", "deprecated", "wrapt", "janome")   # pykakasi and what it needs, and janome (word spacing); all pure-Python wheels (one file for every platform)


def pin_kanji_packages(work):
    """The optional pykakasi download of the Windows installer (scripts/install_kanji_reader.py): the exact wheels and their
    SHA-256, written to scripts/kanji_pins.json (shipped with the program). Only py3-none-any wheels, so there is one file per
    package for every platform and Python version."""
    out = {}
    for name in KANJI_PACKAGES:
        try:
            req = urllib.request.Request("https://pypi.org/pypi/%s/json" % name, headers={"User-Agent": "mousiki-release-toolkit"})
            with urllib.request.urlopen(req, timeout=60) as r:
                info = json.loads(r.read().decode("utf-8"))
            wheels = [f for f in info["urls"] if f["packagetype"] == "bdist_wheel" and f["filename"].endswith("none-any.whl") and not f.get("yanked")]
            if not wheels:
                raise RuntimeError("no pure-Python wheel")
            f = wheels[0]
            got = fetch(f["url"], work / f["filename"], True)
            h = sha256_of(got)
            if h != f["digests"]["sha256"].lower():
                die("SHA-256 mismatch for %s (PyPI says %s, got %s)" % (f["filename"], f["digests"]["sha256"], h))
            out[name] = {"file": f["filename"], "url": f["url"], "sha256": h}
            log("pinned %-40s %s" % (f["filename"], h[:16] + "..."))
        except (Exception, SystemExit) as e:
            warn("%s: could not be pinned (%s) - keeping the old kanji pins" % (name, e))
            return
    write_text(KANJI_PINS_FILE, json.dumps({"_note": "written by release.py pin - review the diff before committing", "packages": out},
                                           indent=2, sort_keys=True) + "\n")
    log("Wrote %s - review the diff, then commit it." % KANJI_PINS_FILE)


def cmd_pin(a):
    """Fix every bundled tool to one exact, versioned file and write its SHA-256 to pins.json. Run it when you want newer
    tools, read the diff, then commit the file. (The Python scripts use only the standard library: no PyPI packages.)"""
    work = Path(a.work).expanduser().resolve() / "pin"
    work.mkdir(parents=True, exist_ok=True)
    old = load_pins()
    files = {}

    def put(name, url, path):
        h = sha256_of(path)
        files[name] = {"url": url, "sha256": h}
        log("pinned %-40s %s" % (name, h[:16] + "..."))

    def get(url, dest):
        return fetch(url, dest, True)

    # yt-dlp: the release tag of 'latest', and the publisher's own checksum list as a cross-check
    try:
        req = urllib.request.Request("https://github.com/yt-dlp/yt-dlp/releases/latest", headers={"User-Agent": "mousiki-release-toolkit"})
        with urllib.request.urlopen(req, timeout=60) as r:
            tag = r.geturl().rstrip("/").split("/")[-1]
    except Exception as e:
        warn("yt-dlp: could not find the latest release (%s) - keeping its old pins" % e)
        tag = None
    if tag:
        base = "https://github.com/yt-dlp/yt-dlp/releases/download/%s/" % tag
        for asset in ("yt-dlp.exe", "yt-dlp_linux", "yt-dlp_linux_aarch64", "yt-dlp_macos"):
            f = get(base + asset, work / asset)
            verify_sha256(f, base + "SHA2-256SUMS", asset)
            put(asset, base + asset, f)
    # FFmpeg for Windows (gyan.dev): the versioned file name is the folder inside the archive
    try:
        f = get("https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip", work / "ffmpeg-win.zip")
        verify_sha256(f, "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip.sha256")
        with zipfile.ZipFile(str(f)) as zf:
            root = zf.namelist()[0].split("/")[0]
        url = "https://www.gyan.dev/ffmpeg/builds/packages/%s.zip" % root
        try:
            f2 = get(url, work / "ffmpeg-win-versioned.zip")
        except SystemExit:
            warn("gyan.dev: %s is not available - pinning the current 'release' file (its URL changes with the next release)" % url)
            url, f2 = "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip", f
        put("ffmpeg-win-essentials.zip", url, f2)
    except SystemExit:
        warn("FFmpeg for Windows: download failed - keeping the old pin")
    # static FFmpeg for the Linux portable zip (John Van Sickle): old-releases keeps the versioned files
    for arch in ("amd64", "arm64"):
        try:
            f = get("https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-%s-static.tar.xz" % arch, work / ("ffmpeg-linux-%s.tar.xz" % arch))
            with tarfile.open(str(f), "r:xz") as tf:
                root = tf.getnames()[0].split("/")[0]
            url = "https://johnvansickle.com/ffmpeg/old-releases/%s.tar.xz" % root
            try:
                f2 = get(url, work / ("ffmpeg-linux-%s-versioned.tar.xz" % arch))
            except SystemExit:
                warn("johnvansickle.com: %s is not available - pinning the current 'release' file" % url)
                url = "https://johnvansickle.com/ffmpeg/releases/ffmpeg-release-%s-static.tar.xz" % arch
                f2 = f
            put("ffmpeg-linux-%s-static.tar.xz" % arch, url, f2)
        except SystemExit:
            warn("static FFmpeg (%s): download failed - keeping the old pin" % arch)
    # BtbN FFmpeg-Builds (the fallback when johnvansickle.com does not answer): a dated release, never the moving "latest"
    try:
        req = urllib.request.Request("https://github.com/BtbN/FFmpeg-Builds/releases/latest", headers={"User-Agent": "mousiki-release-toolkit"})
        with urllib.request.urlopen(req, timeout=60) as r:
            btbn_tag = r.geturl().rstrip("/").split("/")[-1]
    except Exception as e:
        warn("BtbN: could not find the latest dated release (%s) - keeping its old pins" % e)
        btbn_tag = None
    if btbn_tag and btbn_tag != "latest":
        base = "https://github.com/BtbN/FFmpeg-Builds/releases/download/%s/" % btbn_tag
        for arch, flavor in (("amd64", "linux64"), ("arm64", "linuxarm64")):
            asset = "ffmpeg-master-latest-%s-gpl.tar.xz" % flavor
            try:
                f = get(base + asset, work / ("ffmpeg-linux-%s-btbn.tar.xz" % arch))
                verify_sha256(f, base + "checksums.sha256", asset)
                put("ffmpeg-linux-%s-btbn.tar.xz" % arch, base + asset, f)
            except SystemExit:
                warn("BtbN (%s): download failed - keeping the old pin" % arch)
    # Python embeddable: python.org versions never change; no checksum file is published, so this is the first pin you review
    for arch in ("amd64", "arm64"):
        name = "python-%s-embed-%s.zip" % (a.python_embed_version, arch)
        url = "https://www.python.org/ftp/python/%s/%s" % (a.python_embed_version, name)
        try:
            put(name, url, get(url, work / name))
        except SystemExit:
            warn("%s: download failed - keeping the old pin" % name)
    # Cascadia Mono for the macOS app (fixed version, see CASCADIA_VERSION)
    try:
        put(CASCADIA_ZIP, CASCADIA_URL, get(CASCADIA_URL, work / CASCADIA_ZIP))
    except SystemExit:
        warn("%s: download failed - keeping the old pin" % CASCADIA_ZIP)
    for k, v in old.items():           # whatever could not be refreshed this time stays as it was
        files.setdefault(k, v)
    pin_kanji_packages(work)
    write_text(PINS_FILE, json.dumps({"_note": "written by release.py pin - review the diff before committing", "files": files},
                                     indent=2, sort_keys=True) + "\n")
    log("Wrote %s - review the diff, then commit it." % PINS_FILE.name)


def cmd_doctor(a):
    host = host_target()
    print("Host: %s (%s, Python %s)\n" % (host, platform.machine(), platform.python_version()))

    def row(name, ok, hint=""):
        print("  [%s] %-22s %s" % ("ok" if ok else "--", name, "" if ok else hint))
    row("cmake", bool(which("cmake")), "winget install Kitware.CMake | apt install cmake | brew install cmake")
    if host == "windows":
        row("C++ build tools", have_msvc() or bool(which("g++")), "winget install Microsoft.VisualStudio.2022.BuildTools (C++ workload)")
        row("Inno Setup", bool(find_iscc()), "winget install JRSoftware.InnoSetup")
        row("WSL (Linux builds)", bool(which("wsl.exe")), "wsl --install -d Ubuntu")
        row("GitHub CLI (macOS)", bool(which("gh")), "winget install GitHub.cli ; gh auth login")
        row("git", bool(which("git")), "winget install Git.Git   (needed for update-repo / signed builds)")
        row("gpg (Linux signing)", bool(gpg_binary()), "winget install GnuPG.Gpg4win")
        print("\n  'python release.py build --target windows --install-deps' installs the first three for you.")
    elif host == "linux":
        row("g++", bool(which("g++")), "sudo apt install build-essential")
        row("dpkg-deb", bool(which("dpkg-deb")), "sudo apt install dpkg-dev")
    else:
        row("Xcode CLT", bool(capture(["xcode-select", "-p"])), "xcode-select --install")
        row("hdiutil", bool(which("hdiutil")), "comes with macOS")


def cmd_pins_check(a):
    """Downloads every pinned file again (pins.json and scripts/kanji_pins.json) and compares it with its pinned SHA-256, so a
    pin that went stale is noticed before a release build stops on it. Mismatch = the file on the server is not the pinned one
    any more (exit code 1); unreachable = only a warning. With --out the report is also written as Markdown."""
    work = Path(a.work).expanduser().resolve() / "pins-check"
    work.mkdir(parents=True, exist_ok=True)
    items = [(name, pin["url"], pin["sha256"]) for name, pin in sorted(load_pins().items())]
    try:
        kp = json.loads(KANJI_PINS_FILE.read_text(encoding="utf-8")).get("packages", {})
        items += [(v["file"], v["url"], v["sha256"]) for _, v in sorted(kp.items())]
    except Exception as e:
        warn("scripts/kanji_pins.json could not be read (%s)" % e)
    rows, bad, gone = [], 0, 0
    for name, url, want in items:
        dest = work / name
        try:
            got = sha256_of(fetch(url, dest, True, tries=2))
            state = "ok" if got == want.lower() else "MISMATCH"
        except DownloadFailed:
            state = "unreachable"
        if dest.exists():
            dest.unlink()   # (nothing is kept: the next check downloads again)
        if state == "MISMATCH":
            bad += 1
        elif state == "unreachable":
            gone += 1
        rows.append((name, state, url))
        log("%-40s %s" % (name, state))
    lines = ["# Pinned downloads", "",
             "Every file in `packaging/pins.json` and `scripts/kanji_pins.json`, downloaded again and compared with its pinned SHA-256.", ""]
    if bad:
        lines += ["**%d file(s) changed on the server.** A release build stops on them: find out why, then run "
                  "`python packaging/release.py pin` and review the diff." % bad, ""]
    if gone:
        lines += ["%d file(s) could not be downloaded (server down, or the file was removed: if it stays like that, run `pin`)." % gone, ""]
    if not bad and not gone:
        lines += ["All pins are current.", ""]
    lines += ["| file | state | url |", "|---|---|---|"] + ["| %s | %s | %s |" % r for r in rows]
    report = "\n".join(lines) + "\n"
    if getattr(a, "out", None):
        write_text(a.out, report)
    print(report)
    out = os.environ.get("GITHUB_OUTPUT")
    if out:
        with open(out, "a", encoding="utf-8") as f:
            f.write("stale=%d\nunreachable=%d\n" % (bad, gone))
    if bad:
        sys.exit(1)


def main():
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--zip", help="Mousiki source .zip (default: newest *.zip in ./input)")
    common.add_argument("--src", help="build from a source DIRECTORY instead of a zip (used by CI)")
    common.add_argument("--work", default=str(HERE / "work"), help="persistent build/cache folder (default ./work)")
    common.add_argument("--dist", default=str(HERE / "dist"), help="output folder (default ./dist)")
    common.add_argument("--version", help="override the detected version (default: README title, else CMakeLists)")
    common.add_argument("--clean", action="store_true", help="wipe this platform's build tree first (from-scratch rebuild)")
    common.add_argument("--clean-all", action="store_true", help="--clean plus the download cache")
    common.add_argument("--refresh-tools", action="store_true", help="re-download ffmpeg / yt-dlp / Python")
    common.add_argument("--no-bundle-tools", action="store_true", help="do not bundle ffmpeg/yt-dlp/Python (much smaller, relies on PATH)")
    common.add_argument("--install-deps", action="store_true", help="install missing build tools (winget / apt / brew)")
    common.add_argument("--sync-version", action="store_true", help="patch the CMakeLists.txt version (and so the About screens) in the build copy")
    common.add_argument("--python-embed-version", default=DEFAULT_PY_EMBED, help="Windows embedded Python version")
    common.add_argument("--mac-arch", choices=["both", "universal", "arm64", "x86_64", "native"], default="both",
                        help="macOS: both = the universal app plus a separate arm64 app for M chips (default)")
    common.add_argument("--sign-identity", help="macOS codesign identity (default: ad-hoc)")
    common.add_argument("--distro", help="WSL distribution name (default: your default distro)")
    common.add_argument("--repo", help="GitHub repo (owner/name) used for cloud builds")
    common.add_argument("--ref", help="branch/tag the cloud workflow runs on (default: the repo's default branch)")
    common.add_argument("--from-repo", action="store_true", help="cloud builds use the pushed repo content, not an uploaded zip")
    common.add_argument("--sign-windows", action="store_true",
                        help="build + sign the Windows installer on GitHub Actions via SignPath (implies --from-repo)")
    common.add_argument("--update-repo", action="store_true", help="'all': first copy the zip into the git repo and push it")
    common.add_argument("--message", help="commit message for update-repo")
    common.add_argument("--no-push", action="store_true", help="update-repo: commit but do not push")
    common.add_argument("--gpg-key", help="GPG key id/fingerprint: sign SHA256SUMS.txt and the .deb (env: MOUSIKI_GPG_KEY)")
    common.add_argument("--phase", choices=["full", "compile", "package"], default="full",
                        help="Windows only (CI signing flow): compile | package; default full")
    common.add_argument("--to-sign-dir", help="Windows compile phase: where mousiki.exe/fpcalc.exe are left for signing")
    common.add_argument("--signed-binaries", help="Windows package phase: folder with the signed mousiki.exe/fpcalc.exe")
    common.add_argument("--installer-to-sign-dir", help="Windows package phase: where the unsigned installer is left for signing")

    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sb = sub.add_parser("build", parents=[common], help="native build on this machine")
    sb.add_argument("--target", choices=["windows", "linux", "macos"], help="default: this machine's OS")
    sub.add_parser("linux-wsl", parents=[common], help="Linux .deb + zip built inside WSL")
    sc = sub.add_parser("cloud", parents=[common], help="build on GitHub Actions and download the result")
    sc.add_argument("--targets", default="macos", help="comma list: windows,linux,macos (default macos)")
    sa = sub.add_parser("all", parents=[common], help="windows (local) + linux (WSL) + macos (cloud)")
    sa.add_argument("--targets", default="windows,linux,macos")
    sa.add_argument("--no-wsl", action="store_true", help="build Linux in the cloud instead of WSL")
    sub.add_parser("doctor", parents=[common], help="check prerequisites")
    sub.add_parser("pin", parents=[common], help="fix yt-dlp / FFmpeg / Python / PyPI packages to exact versions + SHA-256 (pins.json)")
    sub.add_parser("checksums", parents=[common], help="(re)write SHA256SUMS.txt; with --gpg-key also sign it")
    spc = sub.add_parser("pins-check", parents=[common], help="download every pinned file again and compare it with its pinned SHA-256")
    spc.add_argument("--out", help="also write the report (Markdown) to this file")
    sub.add_parser("update-repo", parents=[common], help="copy the source zip into the git repo, commit and push")
    a = p.parse_args()
    {"build": cmd_build, "linux-wsl": cmd_linux_wsl, "cloud": cmd_cloud, "all": cmd_all, "doctor": cmd_doctor,
     "checksums": cmd_checksums, "update-repo": cmd_update_repo, "pin": cmd_pin, "pins-check": cmd_pins_check}[a.cmd](a)


if __name__ == "__main__":
    main()
