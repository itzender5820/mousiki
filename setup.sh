#!/usr/bin/env bash

set -e

OS="$(uname -s)"

# -------------------------------
# Helpers
# -------------------------------

check_command() {
    command -v "$1" >/dev/null 2>&1
}

print_dep() {
    local name="$1"
    local command="$2"

    if check_command "$command"; then
        printf "%-14s ✓\n" "$name"
        return 0
    else
        printf "%-14s ✗\n" "$name"
        return 1
    fi
}

echo ""
echo "========================================="
echo "          Mousiki Setup"
echo "========================================="
echo ""

echo "OS detecting..."
echo "  $OS"
echo ""

# -------------------------------
# Detect platform
# -------------------------------

IS_TERMUX=false

if [ -n "$TERMUX_VERSION" ] || [ -d "/data/data/com.termux" ]; then
    IS_TERMUX=true
    echo "Platform: Termux"
elif [ "$OS" = "Darwin" ]; then
    echo "Platform: macOS"
elif [ "$OS" = "Linux" ]; then
    echo "Platform: Linux"
else
    echo "Error: Unsupported operating system: $OS"
    exit 1
fi

echo ""

# -------------------------------
# Dependency checking
# -------------------------------

echo "======== Checking deps =========="

MISSING=()

if ! print_dep "ffmpeg" "ffmpeg"; then
    MISSING+=("ffmpeg")
fi

if [ "$IS_TERMUX" = true ]; then
    if ! print_dep "clang" "clang"; then
        MISSING+=("clang")
    fi

    if ! print_dep "make" "make"; then
        MISSING+=("make")
    fi
else
    if ! print_dep "make" "make"; then
        MISSING+=("make")
    fi
fi

if [ "$IS_TERMUX" = true ]; then
    # Termux has its own yt-dlp package (built against Bionic libc) --
    # the standalone PyInstaller binaries below are glibc-linked Linux
    # builds that won't run under Termux at all, so this stays on the
    # existing "detect via PATH, tell the user to `pkg install` it
    # themselves" path rather than attempting a binary download. Termux's
    # package manager pulls in whatever yt-dlp itself needs (including
    # Python) as a transitive dependency automatically -- nothing to
    # check for separately here.
    if ! print_dep "yt-dlp" "yt-dlp"; then
        MISSING+=("yt-dlp")
    fi
fi

if ! print_dep "cmake" "cmake"; then
    MISSING+=("cmake")
fi

# libcurl development headers -- needed at build time now that lyrics
# fetching (src/http_client.cpp) links against libcurl directly instead
# of shelling out to a Python script. The *runtime* library is normally
# already present on any system that's ever used curl/wget/a browser;
# it's specifically the -dev/-devel headers package that's often missing.
if ! print_dep "libcurl" "curl-config"; then
    MISSING+=("libcurl-dev")
fi

# zlib development headers -- needed at build time for the KuGou lyrics
# provider (src/lyrics_fetcher.cpp inflates the KRC blob it downloads).
# No zlib-config-style utility exists to check for like curl-config, but
# pkg-config's zlib.pc is shipped by essentially every zlib dev package;
# falling back to a direct header check covers the rare system without
# pkg-config at all.
zlib_dev_installed() {
    if check_command pkg-config && pkg-config --exists zlib 2>/dev/null; then
        return 0
    fi
    [ -f /usr/include/zlib.h ] || [ -f /usr/local/include/zlib.h ]
}

if zlib_dev_installed; then
    printf "%-14s ✓\n" "zlib"
else
    printf "%-14s ✗\n" "zlib"
    MISSING+=("zlib-dev")
fi


# -------------------------------
# yt-dlp (standalone binary)
# -------------------------------
# yt-dlp is a hard dependency (search/streaming don't work without it),
# but it does NOT need Python at all to run it: yt-dlp publishes fully
# self-contained standalone binaries (PyInstaller builds with their own
# bundled interpreter) on GitHub releases. Downloading that directly,
# instead of going through a distro package or pip/pip3/pipx, sidesteps
# both of the problems that were actually showing up for people: distro
# packages lagging behind (yt-dlp needs frequent updates to keep working
# against YouTube's changes) and pip/pip3/pipx environment errors
# (PEP 668 "externally managed environment", missing pip, etc.) -- none
# of that applies to a plain binary download.
#
# Termux is the one exception -- handled above via `pkg install yt-dlp`,
# Termux's own Bionic-libc build, since these standalone binaries are
# glibc-linked Linux builds that won't run there at all.

MOUSIKI_BIN_DIR="$HOME/.local/share/mousiki/bin"
YTDLP_BIN="$MOUSIKI_BIN_DIR/yt-dlp"

ytdlp_asset_name() {
    if [ "$OS" = "Darwin" ]; then
        echo "yt-dlp_macos"
        return
    fi
    case "$(uname -m)" in
        x86_64|amd64) echo "yt-dlp_linux" ;;
        aarch64|arm64) echo "yt-dlp_linux_aarch64" ;;
        armv7l|armv6l) echo "yt-dlp_linux_armv7l" ;;
        *) echo "yt-dlp_linux" ;; # best-effort fallback for anything unrecognized
    esac
}

download_file() {
    # $1 = URL, $2 = destination path
    if check_command curl; then
        if curl -fL --retry 2 -o "$2" "$1"; then return 0; else return 1; fi
    elif check_command wget; then
        if wget -q -O "$2" "$1"; then return 0; else return 1; fi
    else
        return 1
    fi
}

install_ytdlp_standalone() {
    if ! check_command curl && ! check_command wget; then
        echo "Neither curl nor wget is available -- can't download yt-dlp."
        return 1
    fi

    mkdir -p "$MOUSIKI_BIN_DIR"
    local asset url tmp
    asset="$(ytdlp_asset_name)"
    url="https://github.com/yt-dlp/yt-dlp/releases/latest/download/$asset"
    tmp="$YTDLP_BIN.download"

    echo "==> Downloading yt-dlp ($asset) ..."
    if ! download_file "$url" "$tmp"; then
        rm -f "$tmp"
        echo "Download failed."
        return 1
    fi

    if ! chmod +x "$tmp"; then
        rm -f "$tmp"
        echo "Could not make the downloaded file executable."
        return 1
    fi

    if ! "$tmp" --version >/dev/null 2>&1; then
        rm -f "$tmp"
        echo "Downloaded yt-dlp binary did not run correctly."
        return 1
    fi

    if ! mv -f "$tmp" "$YTDLP_BIN"; then
        rm -f "$tmp"
        echo "Could not move the downloaded binary into place."
        return 1
    fi

    return 0
}

if [ "$IS_TERMUX" != true ]; then
    if install_ytdlp_standalone; then
        printf "%-14s ✓  (%s)\n" "yt-dlp" "$YTDLP_BIN"
    else
        printf "%-14s ✗\n" "yt-dlp"
        echo ""
        echo "Error: could not set up yt-dlp, and it's required (search and"
        echo "streaming don't work without it)."
        echo "You can install your own copy and mousiki will still find it"
        echo "on PATH -- or place a working binary at:"
        echo "  $YTDLP_BIN"
        echo "and re-run this script."
        exit 1
    fi
fi

echo ""

# -------------------------------
# Install missing dependencies
# -------------------------------

if [ ${#MISSING[@]} -gt 0 ]; then
    echo ""
    echo "Missing dependencies:"
    printf '  - %s\n' "${MISSING[@]}"
    echo ""

    if [ "$IS_TERMUX" = true ]; then
        echo "Termux dependencies are not installed automatically."
        echo "Please install the missing packages and run setup.sh again."
        exit 1
    fi

    if [ "$OS" = "Darwin" ]; then

        if ! check_command brew; then
            echo "Error: Homebrew is required."
            echo "Install Homebrew and run setup.sh again."
            exit 1
        fi

        echo "==> Installing missing macOS dependencies..."

        BREW_DEPS=()

        for dep in "${MISSING[@]}"; do
            case "$dep" in
                libcurl-dev)
                    # macOS ships its own libcurl + headers with the
                    # system/Xcode Command Line Tools already -- nothing
                    # to install via brew for this.
                    ;;
                zlib-dev)
                    # Same story as libcurl -- macOS ships zlib + headers
                    # with the system/Xcode Command Line Tools already.
                    ;;
                *)
                    BREW_DEPS+=("$dep")
                    ;;
            esac
        done

        HOMEBREW_NO_AUTO_UPDATE=1 brew install "${BREW_DEPS[@]}"

    elif [ "$OS" = "Linux" ]; then

        if check_command apt-get; then

            echo "==> Installing missing dependencies via apt..."

            sudo apt-get update
            sudo apt-get install -y \
                cmake \
                build-essential \
                ffmpeg \
                libcurl4-openssl-dev \
                zlib1g-dev

        elif check_command pacman; then

            echo "==> Installing missing dependencies via pacman..."

            sudo pacman -Sy --noconfirm \
                cmake \
                base-devel \
                ffmpeg \
                curl \
                zlib

        elif check_command dnf; then

            echo "==> Installing missing dependencies via dnf..."

            sudo dnf install -y \
                cmake \
                gcc-c++ \
                make \
                ffmpeg \
                libcurl-devel \
                zlib-devel

        else
            echo "Error: Unsupported Linux package manager."
            exit 1
        fi
    fi
fi

# -------------------------------
# Verify dependencies again
# -------------------------------

echo ""
echo "======== Verifying deps ========="

for cmd in cmake ffmpeg; do
    if ! check_command "$cmd"; then
        echo "Error: $cmd is still missing."
        exit 1
    fi
done

if [ "$IS_TERMUX" = true ]; then
    if ! check_command yt-dlp; then
        echo "Error: yt-dlp is still missing."
        exit 1
    fi
elif [ ! -x "$YTDLP_BIN" ]; then
    echo "Error: yt-dlp is still missing at $YTDLP_BIN."
    exit 1
fi

if ! check_command curl-config; then
    echo "Error: libcurl development headers are still missing."
    echo "(package name varies: libcurl4-openssl-dev on apt, curl on pacman, libcurl-devel on dnf)"
    exit 1
fi

if ! zlib_dev_installed; then
    echo "Error: zlib development headers are still missing."
    echo "(package name varies: zlib1g-dev on apt, zlib on pacman, zlib-devel on dnf)"
    exit 1
fi

if [ "$IS_TERMUX" = true ] && ! check_command clang; then
    echo "Error: clang is still missing."
    exit 1
fi

echo ""
echo "All required dependencies are ready."
echo ""

# -------------------------------
# Configuration
# -------------------------------

echo "======== Configuring Mousiki ========"

CONFIG_DIR="$HOME/.config/mousiki"
mkdir -p "$CONFIG_DIR"

if [ ! -f "$CONFIG_DIR/config.txt" ]; then
    cp config.txt "$CONFIG_DIR/config.txt"
    echo "Created $CONFIG_DIR/config.txt"
else
    echo "Existing config found. Keeping current file."
fi

YTDLP_CONFIG_DIR="$HOME/.config/yt-dlp"
mkdir -p "$YTDLP_CONFIG_DIR"

if [ ! -f "$YTDLP_CONFIG_DIR/config" ]; then
    echo '--extractor-args "youtube:player_client=android"' \
        > "$YTDLP_CONFIG_DIR/config"

    echo "Configured yt-dlp."
elif ! grep -q "player_client" "$YTDLP_CONFIG_DIR/config"; then
    echo '--extractor-args "youtube:player_client=android"' \
        >> "$YTDLP_CONFIG_DIR/config"

    echo "Updated yt-dlp configuration."
else
    echo "yt-dlp configuration already exists."
fi

# -------------------------------
# Build
# -------------------------------

echo ""
echo "======== Building ( cmake ) ========"

cmake -B build

echo ""
echo "======== Building ( make ) ========="

CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

cmake --build build -j"$CORES"

# -------------------------------
# Verify build
# -------------------------------

BINARY="./build/mousiki"

if [ ! -f "$BINARY" ]; then
    echo ""
    echo "Error: Build completed but binary was not found."
    exit 1
fi

echo ""
echo "======== Build completed ==========="
echo ""
echo "Binary: $BINARY"
echo ""

# -------------------------------
# Optional binary installation
# -------------------------------

if [ "$IS_TERMUX" = true ]; then
    BIN_DIR="$PREFIX/bin"
else
    BIN_DIR="/usr/local/bin"
fi

printf "WANT TO COPY BINARY TO %s? (Y/N) " "$BIN_DIR"
read -r INSTALL_BINARY

if [[ "$INSTALL_BINARY" =~ ^[Yy]$ ]]; then

    mkdir -p "$BIN_DIR"

    if [ -w "$BIN_DIR" ]; then
        cp "$BINARY" "$BIN_DIR/mousiki"
    else
        sudo cp "$BINARY" "$BIN_DIR/mousiki"
    fi

    echo ""
    echo "copied!!"
    echo ""
    echo "Run Mousiki with:"
    echo "  mousiki"
else
    echo ""
    echo "Binary left at:"
    echo "  $BINARY"
fi

echo ""
echo "========================================="
echo "        Mousiki setup complete!"
echo "========================================="
