#Requires -Version 7.0
<#
.SYNOPSIS
    Windows equivalent of setup.sh: installs mousiki's runtime dependencies,
    then configures and builds it.

.DESCRIPTION
    mousiki itself is a C++ binary, but it shells out to three external tools
    at runtime:

      ffmpeg  - decodes Opus (miniaudio's built-in decoders don't cover it,
                and Opus is exactly what the yt-dlp cache stores), supplies
                ffprobe track metadata, and decodes the audio that fpcalc
                fingerprints for the AcoustID fetch.
      yt-dlp  - online search, playlist listing and streaming.
      python  - runs scripts/fetch_lyrics.py for synced lyrics and
                scripts/fetch_meta.py for the AcoustID metadata fetch
                (which drives fpcalc, the fingerprint helper CMake builds
                from third_party/chromaprint/).

    None of those are optional if you want the corresponding feature, but all
    three are independent: mousiki runs local files fine with none installed.

.PARAMETER SkipDeps
    Configure and build only; don't touch winget/pip.

.PARAMETER BuildType
    Release (default) or Debug.

.PARAMETER UpgradeFfmpeg
    Also run `winget upgrade` on an FFmpeg that is already installed (slow: winget refreshes its sources first). Off by
    default; an FFmpeg that lacks libopenmpt / libgme (tracker modules, console music) is replaced by the full
    Gyan.FFmpeg build anyway.

.PARAMETER NoUpgrade
    Never touch an FFmpeg that is already installed, not even when it lacks libopenmpt / libgme.

.PARAMETER WithKanji
    (Every download of this script -- CMake, FFmpeg, yt-dlp, Python, pykakasi -- needs an internet connection.)
    Also install pykakasi (pip install --user pykakasi), so kanji are shown in Latin letters too (SHIFT+'). Optional:
    pykakasi is GPL-3.0 and not part of Mousiki; it is downloaded from pypi.org and run as a separate program.

.PARAMETER NoCcache
    Don't use ccache. By default it is installed with winget (when winget exists and it is missing) and CMake uses it, so a
    rebuild after a small change, a branch switch or a clean build takes a fraction of the time.

.PARAMETER NoInstall
    Don't create the launchers. By default the commands "mousiki" and "lala"
    are created in %LOCALAPPDATA%\Mousiki\bin, which is added to your user
    PATH, so both start the app from any (new) terminal.
#>
[CmdletBinding()]
param(
    [switch]$SkipDeps,
    [ValidateSet('Release','Debug')]
    [string]$BuildType = 'Release',
    [switch]$NoInstall,
    [switch]$NoUpgrade,
    [switch]$UpgradeFfmpeg,
    [switch]$WithKanji,
    [switch]$NoCcache
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

function Test-Command([string]$Name) {
    return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

function Write-Step([string]$Message) {
    Write-Host "==> $Message" -ForegroundColor Cyan
}

# Tracker modules and console music are read through FFmpeg's libopenmpt / libgme demuxers.
$script:demuxerList = $null   # ffmpeg is asked once (and again after it was installed or upgraded)
function Test-FfmpegDemuxer([string]$Name) {
    if (-not (Test-Command ffmpeg)) { return $false }
    if ($null -eq $script:demuxerList) { $script:demuxerList = (& ffmpeg -hide_banner -demuxers 2>$null) -join "`n" }
    return $script:demuxerList -match "\s$Name\s"
}

function Update-SessionPath {
    $env:PATH = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
                [Environment]::GetEnvironmentVariable('Path','User')
}

function Add-ToUserPath([string]$Dir) {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $parts = @($userPath -split ';' | Where-Object { $_ -ne '' })
    if ($parts -notcontains $Dir) {
        [Environment]::SetEnvironmentVariable('Path', (($parts + $Dir) -join ';'), 'User')
    }
    if (($env:PATH -split ';') -notcontains $Dir) { $env:PATH = "$env:PATH;$Dir" }
}

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
if (-not $SkipDeps) {
    if (-not (Test-Command winget)) {
        Write-Warning "winget not found. Install the dependencies manually, then re-run with -SkipDeps."
    } else {
        # cmake and a compiler come first; without them there is nothing to build.
        if (-not (Test-Command cmake)) {
            Write-Step "Installing CMake"
            winget install --id Kitware.CMake --accept-source-agreements --accept-package-agreements --silent
        }
        if (-not $NoCcache -and -not (Test-Command ccache)) {
            Write-Step "Installing ccache (faster rebuilds)"
            winget install --id ccache.ccache --accept-source-agreements --accept-package-agreements --silent
            if ($LASTEXITCODE -ne 0) { Write-Warning "ccache could not be installed (the build works without it; -NoCcache silences this)." }
        }
        if (-not (Test-Command ffmpeg)) {
            Write-Step "Installing FFmpeg (the full build: it includes libopenmpt and libgme)"
            winget install --id Gyan.FFmpeg --accept-source-agreements --accept-package-agreements --silent
        } elseif ($UpgradeFfmpeg -and -not $NoUpgrade) {
            # Already installed: upgrade it on request (quietly does nothing when it is current or was not installed by winget).
            Write-Step "Upgrading FFmpeg (already installed)"
            winget upgrade --id Gyan.FFmpeg --accept-source-agreements --accept-package-agreements --silent
        }
        if (-not (Test-Command yt-dlp)) {
            Write-Step "Installing yt-dlp"
            winget install --id yt-dlp.yt-dlp --accept-source-agreements --accept-package-agreements --silent
        }
        if (-not (Test-Command py) -and -not (Test-Command python3)) {
            Write-Step "Installing Python 3"
            winget install --id Python.Python.3.13 --accept-source-agreements --accept-package-agreements --silent
        }
    }

    # winget puts new tools on the machine PATH, but this already-running
    # session inherited the old one. Refresh it in-process so the checks below
    # and the build itself can see them without a new shell.
    Update-SessionPath

    if ($WithKanji) {
        Write-Step "Installing pykakasi (optional: kanji readings for the Latin letters display)"
        $pyCmd = if (Test-Command py) { @('py','-3') } elseif (Test-Command python3) { @('python3') } elseif (Test-Command python) { @('python') } else { $null }
        if ($pyCmd) {
            $pyRest = if ($pyCmd.Count -gt 1) { $pyCmd[1..($pyCmd.Count-1)] } else { @() }
            & $pyCmd[0] @pyRest -m pip install --user pykakasi janome
            if ($LASTEXITCODE -ne 0) { Write-Warning "pykakasi could not be installed -- Mousiki works without it (kanji just stay as they are)." }
        } else {
            Write-Warning "No Python found -- pykakasi was skipped."
        }
    }

    # --- retro formats: FFmpeg needs libopenmpt (tracker modules) and libgme (console music) ---
    if ((Test-Command ffmpeg) -and -not $NoUpgrade -and (Test-Command winget) -and
        -not ((Test-FfmpegDemuxer 'libopenmpt') -and (Test-FfmpegDemuxer 'libgme'))) {
        Write-Step "Your FFmpeg lacks libopenmpt / libgme -- installing the full build (Gyan.FFmpeg)"
        winget install --id Gyan.FFmpeg --accept-source-agreements --accept-package-agreements --silent
        Update-SessionPath
        $script:demuxerList = $null
    }
    if (Test-Command ffmpeg) {
        foreach ($d in @(@('libopenmpt','tracker modules (.mod .xm .it .s3m ...)'), @('libgme','console music (.nsf .spc .vgm ...)'))) {
            if (-not (Test-FfmpegDemuxer $d[0])) {
                Write-Warning "The ffmpeg that is first on your PATH ($((Get-Command ffmpeg).Source)) has no $($d[0]) -- $($d[1]) will not play. Remove that ffmpeg from the PATH (or put the Gyan.FFmpeg full build before it) and open a new terminal."
            }
        }
    }

    # --- sidplayfp (Commodore 64 SID tunes) must be on the PATH ---
    if (-not (Test-Command sidplayfp)) {
        $roots = @("$env:LOCALAPPDATA\Mousiki\bin", "$env:USERPROFILE\scoop\apps",
                   "$env:USERPROFILE\Downloads", "$env:USERPROFILE\Desktop", "$env:USERPROFILE\sidplayfp", 'C:\tools', 'C:\sidplayfp') |
                 Where-Object { $_ -and (Test-Path $_) }
        $found = $null
        foreach ($r in $roots) {
            $found = Get-ChildItem -Path $r -Filter 'sidplayfp.exe' -Recurse -Depth 2 -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($found) { break }
        }
        if ($found) {
            Add-ToUserPath $found.DirectoryName
            Write-Step "Added $($found.DirectoryName) (sidplayfp.exe) to your user PATH (new terminals pick it up)"
        } else {
            Write-Warning @"
sidplayfp.exe was not found, so Commodore 64 .sid tunes will not play (everything else works).
The project publishes no Windows build; get one (any sidplayfp 2.x/3.x win64 build), unzip it anywhere
(e.g. C:\tools\sidplayfp or your Downloads folder) and run this script again: it finds sidplayfp.exe and
puts its folder on your PATH. Dropping sidplayfp.exe into $env:LOCALAPPDATA\Mousiki\bin works as well.
"@
        }
    }

    # The Python scripts (lyrics, AcoustID fetch, fast search) use only the standard library: no pip package is needed.
    if (-not ((Test-Command py) -or (Test-Command python3))) {
        Write-Warning "No Python 3 found. Lyrics will be unavailable; everything else still works."
    }
}

# ---------------------------------------------------------------------------
# Toolchain check
# ---------------------------------------------------------------------------
if (-not (Test-Command cmake)) {
    throw "cmake is not on PATH. Open a new PowerShell window (winget updates PATH only for new sessions) or install CMake manually."
}

# CMake needs a C++17 compiler. On a clean Windows box there isn't one: Visual
# Studio Build Tools has to be installed interactively, because the C++
# workload is a separate component that winget's silent install won't select.
$hasCompiler = (Test-Command cl) -or (Test-Command g++) -or
               (Test-Path 'C:\Program Files\Microsoft Visual Studio') -or
               (Test-Path 'C:\Program Files (x86)\Microsoft Visual Studio')
if (-not $hasCompiler) {
    Write-Warning @"
No C++ compiler detected.

Install Visual Studio Build Tools and tick the workload
"Desktop development with C++":

    winget install --id Microsoft.VisualStudio.2022.BuildTools

Then re-run this script. (MSYS2 / MinGW-w64 also works if you prefer it --
pass -G "MinGW Makefiles" to cmake below.)
"@
    return
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
$build = Join-Path $root 'build'

Write-Step "Configuring ($BuildType)"
# miniaudio and kissfft are vendored in third_party/, so configuring
# needs no network access.
$ccacheFlag = if ($NoCcache) { '-DMOUSIKI_USE_CCACHE=OFF' } else { '-DMOUSIKI_USE_CCACHE=ON' }
cmake -S $root -B $build -DCMAKE_BUILD_TYPE=$BuildType $ccacheFlag
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Step "Building"
cmake --build $build --config $BuildType --parallel $env:NUMBER_OF_PROCESSORS
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }
if (-not $NoCcache -and (Test-Command ccache)) { Write-Step "ccache"; ccache --show-stats 2>$null | Select-Object -First 8 }

# Single-config generators put the exe in build\, multi-config (Visual Studio)
# in build\<Config>\.
$exe = Get-ChildItem -Path $build -Filter 'mousiki.exe' -Recurse |
       Select-Object -First 1

if (-not $exe) { throw "Build reported success but mousiki.exe was not found under $build" }

# ---------------------------------------------------------------------------
# SDL2.dll (optional): the scope and spectrogram windows
# ---------------------------------------------------------------------------
# Mousiki loads SDL2.dll at run time from the folder of mousiki.exe; nothing
# else needs it, so a failed download only costs the two windows. Pinned and
# checksummed like in packaging/release.py (keep the two in step).
$sdlVersion = '2.32.10'
$sdlSha256  = '6cf9706eefd0a4a06dc764007934d428afaf029fabdd408a9e646048c91e18fb'
$sdlDll = Join-Path $exe.DirectoryName 'SDL2.dll'
if ($SkipDeps) {
    if (-not (Test-Path $sdlDll)) {
        Write-Host "SDL2.dll not fetched (-SkipDeps): the scope / spectrogram windows need it next to mousiki.exe."
    }
} elseif (-not (Test-Path $sdlDll)) {
    $sdlZipName = "SDL2-$sdlVersion-win32-x64.zip"
    $sdlZip = Join-Path ([IO.Path]::GetTempPath()) $sdlZipName
    try {
        Write-Step "Fetching SDL2 $sdlVersion (scope and spectrogram windows)"
        Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/libsdl-org/SDL/releases/download/release-$sdlVersion/$sdlZipName" -OutFile $sdlZip
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sdlZip).Hash.ToLowerInvariant()
        if ($hash -ne $sdlSha256) {
            Write-Warning "A damaged download is possible: trying once more."
            Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/libsdl-org/SDL/releases/download/release-$sdlVersion/$sdlZipName" -OutFile $sdlZip
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $sdlZip).Hash.ToLowerInvariant()
        }
        if ($hash -ne $sdlSha256) {
            throw "SHA-256 mismatch for $sdlZipName - expected $sdlSha256, received $hash. NOT installed: the file at that address is not the pinned one (run setup again later; if it stays, update Mousiki or report it at github.com/StSchwerdtfeger/mousiki-plus/issues)"
        }
        $sdlDir = Join-Path ([IO.Path]::GetTempPath()) "mousiki-sdl2-$sdlVersion"
        Expand-Archive -LiteralPath $sdlZip -DestinationPath $sdlDir -Force
        Copy-Item -LiteralPath (Join-Path $sdlDir 'SDL2.dll') -Destination $exe.DirectoryName -Force
        $sdlReadme = Join-Path $sdlDir 'README-SDL.txt'
        if (Test-Path $sdlReadme) { Copy-Item -LiteralPath $sdlReadme -Destination $exe.DirectoryName -Force }
        Remove-Item -LiteralPath $sdlDir -Recurse -Force -ErrorAction SilentlyContinue
        Write-Step "SDL2.dll placed next to mousiki.exe"
    } catch {
        Write-Warning "Could not fetch SDL2 ($($_.Exception.Message)). Everything else works; for the scope / spectrogram windows copy SDL2.dll from $sdlZipName (github.com/libsdl-org/SDL/releases) next to mousiki.exe."
    } finally {
        Remove-Item -LiteralPath $sdlZip -Force -ErrorAction SilentlyContinue
    }
}

# ---------------------------------------------------------------------------
# Launchers: "mousiki" and "lala" (skip with -NoInstall)
# ---------------------------------------------------------------------------
# Two small .cmd files in %LOCALAPPDATA%\Mousiki\bin that start the built exe
# (which finds scripts\ next to itself); that folder goes on the user PATH.
if (-not $NoInstall) {
    $binDir = Join-Path $env:LOCALAPPDATA 'Mousiki\bin'
    New-Item -ItemType Directory -Force -Path $binDir | Out-Null
    foreach ($name in 'mousiki', 'lala') {
        $launcher = Join-Path $binDir "$name.cmd"
        Set-Content -Path $launcher -Encoding ascii -Value "@echo off`r`n`"$($exe.FullName)`" %*"
        Write-Step "Launcher created: $launcher"
    }
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $parts = @($userPath -split ';' | Where-Object { $_ -ne '' })
    if ($parts -notcontains $binDir) {
        [Environment]::SetEnvironmentVariable('Path', (($parts + $binDir) -join ';'), 'User')
        Write-Step "Added $binDir to your user PATH (new terminals pick it up)"
    }
    if (($env:PATH -split ';') -notcontains $binDir) { $env:PATH = "$env:PATH;$binDir" }
}

Write-Host ""
Write-Host "Built: $($exe.FullName)" -ForegroundColor Green
Write-Host "Run it with:  & '$($exe.FullName)'"
if (-not $NoInstall) { Write-Host "or simply:    mousiki   (or: lala)   -- in a new terminal" }
Write-Host ""
Write-Host "Config will be generated at: $env:USERPROFILE\.config\mousiki\config.txt"
Write-Host "Use Windows Terminal, not the legacy console window."
