<#
.SYNOPSIS
    Mousiki Setup and Build script for Windows.
.DESCRIPTION
    Checks for prerequisites (FFmpeg, Python, yt-dlp, CMake, Visual Studio / MSVC),
    installs Python requirements, creates default directories, and builds mousiki.exe.
#>

$ErrorActionPreference = "Stop"

Write-Host ""
Write-Host "=========================================" -ForegroundColor Cyan
Write-Host "       Mousiki Setup for Windows         " -ForegroundColor Cyan
Write-Host "=========================================" -ForegroundColor Cyan
Write-Host ""

function Test-CommandAvailable {
    param([string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    return ($null -ne $cmd)
}

function Print-Dep {
    param([string]$Name, [string]$Command)
    if (Test-CommandAvailable $Command) {
        Write-Host ("{0,-14} [OK]" -f $Name) -ForegroundColor Green
        return $true
    } else {
        Write-Host ("{0,-14} [MISSING]" -f $Name) -ForegroundColor Red
        return $false
    }
}

Write-Host "======== Checking Dependencies ==========" -ForegroundColor Yellow

$missing = @()

if (-not (Print-Dep "ffmpeg" "ffmpeg")) {
    $missing += "ffmpeg (install via: winget install Gyan.FFmpeg)"
}

if (-not (Print-Dep "ffprobe" "ffprobe")) {
    $missing += "ffprobe (comes with Gyan.FFmpeg)"
}

$hasPython = $false
$pythonCmd = "python"
if (Test-CommandAvailable "python") {
    $hasPython = $true
    $pythonCmd = "python"
    Write-Host ("{0,-14} [OK] ($(& python --version))" -f "python") -ForegroundColor Green
} elseif (Test-CommandAvailable "python3") {
    $hasPython = $true
    $pythonCmd = "python3"
    Write-Host ("{0,-14} [OK] ($(& python3 --version))" -f "python3") -ForegroundColor Green
} else {
    Write-Host ("{0,-14} [MISSING]" -f "python") -ForegroundColor Red
    $missing += "python (install via: winget install Python.Python.3.11)"
}

if (-not (Print-Dep "yt-dlp" "yt-dlp")) {
    Write-Host "  Note: yt-dlp can be installed via: winget install yt-dlp.yt-dlp" -ForegroundColor DarkGray
}

Write-Host ""

if ($hasPython) {
    Write-Host "Verifying Python dependencies (requests)..." -ForegroundColor Yellow
    & $pythonCmd -c "import requests" 2>$null
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Installing Python 'requests' package..." -ForegroundColor Cyan
        & $pythonCmd -m pip install requests
    } else {
        Write-Host "Python 'requests' package is installed." -ForegroundColor Green
    }
}

# Create config & cache folders
$configDir = Join-Path $env:USERPROFILE ".config\mousiki"
$cacheDir = Join-Path $env:USERPROFILE ".cache\mousiki"
New-Item -ItemType Directory -Force -Path $configDir | Out-Null
New-Item -ItemType Directory -Force -Path $cacheDir | Out-Null

$defaultConfig = Join-Path $PSScriptRoot "config.txt"
$targetConfig = Join-Path $configDir "config.txt"
if ((Test-Path $defaultConfig) -and -not (Test-Path $targetConfig)) {
    Copy-Item $defaultConfig $targetConfig
    Write-Host "Initialized default configuration at $targetConfig" -ForegroundColor Green
}

if ($missing.Count -gt 0) {
    Write-Host ""
    Write-Host "Some recommended dependencies are missing:" -ForegroundColor Yellow
    foreach ($m in $missing) {
        Write-Host "  - $m" -ForegroundColor Red
    }
    Write-Host ""
}

# Find Visual Studio / MSVC environment if cmake not on PATH
Write-Host "Locating build tools..." -ForegroundColor Yellow
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsPath -and (Test-Path "$vsPath\VC\Auxiliary\Build\vcvars64.bat")) {
        $vcvars = "$vsPath\VC\Auxiliary\Build\vcvars64.bat"
    }
}

Write-Host "Building Mousiki..." -ForegroundColor Cyan
if ($vcvars) {
    Write-Host "Using Visual Studio environment: $vcvars" -ForegroundColor DarkGray
    cmd.exe /c "call `"$vcvars`" && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release"
} elseif (Test-CommandAvailable "cmake") {
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release
} else {
    Write-Error "Neither CMake nor Visual Studio C++ build tools could be found on PATH."
    exit 1
}

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "=========================================" -ForegroundColor Green
    Write-Host "     Mousiki build successful!           " -ForegroundColor Green
    Write-Host "=========================================" -ForegroundColor Green
    Write-Host ""
    Write-Host "To run Mousiki:" -ForegroundColor Cyan
    if (Test-Path "build\Release\mousiki.exe") {
        Write-Host "  .\build\Release\mousiki.exe" -ForegroundColor White
    } elseif (Test-Path "build\mousiki.exe") {
        Write-Host "  .\build\mousiki.exe" -ForegroundColor White
    }
} else {
    Write-Error "Build failed. Check compiler output above for errors."
}
