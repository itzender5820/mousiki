#!/usr/bin/env bash

set -e

OS="$(uname -s)"

echo "==> Detecting operating system: $OS"

if [ "$OS" = "Darwin" ]; then
    echo "==> macOS detected. Checking prerequisites..."
    if ! command -v brew >/dev/null 2>&1; then
        echo "Error: Homebrew is required on macOS. Install it from https://brew.sh and re-run setup.sh"
        exit 1
    fi

    echo "==> Installing macOS dependencies via Homebrew..."
    BREW_DEPS=()
    for dep in cmake ffmpeg yt-dlp python3; do
        if ! command -v "$dep" >/dev/null 2>&1; then
            BREW_DEPS+=("$dep")
        fi
    done

    if [ ${#BREW_DEPS[@]} -gt 0 ]; then
        echo "==> Installing missing packages: ${BREW_DEPS[*]}"
        HOMEBREW_NO_AUTO_UPDATE=1 brew install "${BREW_DEPS[@]}"
    else
        echo "==> All Homebrew packages already installed."
    fi

elif [ -n "$TERMUX_VERSION" ] || [ -d "/data/data/com.termux" ]; then
    echo "==> Termux detected. Installing dependencies via pkg..."
    pkg update -y
    pkg install -y cmake clang ffmpeg yt-dlp python make

elif [ "$OS" = "Linux" ]; then
    echo "==> Linux detected."
    if command -v apt-get >/dev/null 2>&1; then
        echo "==> Installing dependencies via apt..."
        sudo apt-get update
        sudo apt-get install -y cmake build-essential ffmpeg yt-dlp python3 python3-pip
    elif command -v pacman >/dev/null 2>&1; then
        echo "==> Installing dependencies via pacman..."
        sudo pacman -Sy --noconfirm cmake base-devel ffmpeg yt-dlp python python-pip
    elif command -v dnf >/dev/null 2>&1; then
        echo "==> Installing dependencies via dnf..."
        sudo dnf install -y cmake gcc-c++ make ffmpeg yt-dlp python3 python3-pip
    else
        echo "Warning: Unsupported package manager. Please ensure cmake, ffmpeg, yt-dlp, and python3 are installed."
    fi
else
    echo "Warning: Unrecognized OS ($OS). Assuming dependencies are installed manually."
fi

echo "==> Installing Python dependencies..."
if ! python3 -c "import syncedlyrics" >/dev/null 2>&1; then
    echo "==> Installing syncedlyrics..."
    python3 -m pip install --break-system-packages syncedlyrics 2>/dev/null || \
    python3 -m pip install --user syncedlyrics 2>/dev/null || \
    python3 -m pip install syncedlyrics
else
    echo "==> syncedlyrics is already installed."
fi

echo "==> Installing Mousiki configuration..."
CONFIG_DIR="$HOME/.config/mousiki"
mkdir -p "$CONFIG_DIR"
if [ ! -f "$CONFIG_DIR/config.txt" ]; then
    cp config.txt "$CONFIG_DIR/config.txt"
    echo "==> Created $CONFIG_DIR/config.txt"
else
    echo "==> Existing config found at $CONFIG_DIR/config.txt (keeping current file)"
fi

echo "==> Building Mousiki..."
CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
cmake -B build
cmake --build build -j"$CORES"

echo ""
echo "========================================="
echo "  Setup complete! Run Mousiki with:     "
echo "    ./build/mousiki                      "
echo "========================================="
