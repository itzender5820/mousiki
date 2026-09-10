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

    if ! print_dep "python" "python"; then
        MISSING+=("python")
    fi

    PYTHON_CMD="python"

else
    if ! print_dep "make" "make"; then
        MISSING+=("make")
    fi

    if ! print_dep "python" "python3"; then
        MISSING+=("python3")
    fi

    PYTHON_CMD="python3"
fi

if ! print_dep "yt-dlp" "yt-dlp"; then
    MISSING+=("yt-dlp")
fi

if ! print_dep "cmake" "cmake"; then
    MISSING+=("cmake")
fi

# -------------------------------
# Python package manager
# -------------------------------

PIP_PATH="$(command -v pip 2>/dev/null || true)"
PIPX_PATH="$(command -v pipx 2>/dev/null || true)"

if [[ "$PIPX_PATH" == /usr/* ]]; then
    PIP_MANAGER="pipx"
    PIP_MANAGER_PATH="$PIPX_PATH"
elif [[ "$PIP_PATH" == /usr/* ]]; then
    PIP_MANAGER="pip"
    PIP_MANAGER_PATH="$PIP_PATH"
elif [ -n "$PIPX_PATH" ]; then
    PIP_MANAGER="pipx"
    PIP_MANAGER_PATH="$PIPX_PATH"
elif [ -n "$PIP_PATH" ]; then
    PIP_MANAGER="pip"
    PIP_MANAGER_PATH="$PIP_PATH"
else
    echo "Error: neither pip nor pipx is installed."
    exit 1
fi

echo "Python package manager: $PIP_MANAGER"
echo "  $PIP_MANAGER_PATH"

# -------------------------------
# Python dependency
# -------------------------------

if [ "$PIP_MANAGER" = "pip" ]; then

    if "$PYTHON_CMD" -c "import syncedlyrics" >/dev/null 2>&1; then
        printf "%-14s ✓\n" "syncedlyrics"
    else
        printf "%-14s ✗\n" "syncedlyrics"
        MISSING+=("syncedlyrics")
    fi

elif [ "$PIP_MANAGER" = "pipx" ]; then

    if "$PIP_MANAGER_PATH" list 2>/dev/null | grep -q '^syncedlyrics '; then
        printf "%-14s ✓\n" "syncedlyrics"
    else
        printf "%-14s ✗\n" "syncedlyrics"
        MISSING+=("syncedlyrics")
    fi

fi

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
                python3)
                    BREW_DEPS+=("python3")
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
                yt-dlp \
                python3 \
                python3-pip

        elif check_command pacman; then

            echo "==> Installing missing dependencies via pacman..."

            sudo pacman -Sy --noconfirm \
                cmake \
                base-devel \
                ffmpeg \
                yt-dlp \
                python \
                python-pip

        elif check_command dnf; then

            echo "==> Installing missing dependencies via dnf..."

            sudo dnf install -y \
                cmake \
                gcc-c++ \
                make \
                ffmpeg \
                yt-dlp \
                python3 \
                python3-pip

        else
            echo "Error: Unsupported Linux package manager."
            exit 1
        fi
    fi
fi

# -------------------------------
# Install Python dependencies
# -------------------------------

if [[ " ${MISSING[*]} " == *" syncedlyrics "* ]]; then

    echo ""
    echo "==> Installing syncedlyrics..."

    if [ "$PIP_MANAGER" = "pip" ]; then
        "$PYTHON_CMD" -m pip install syncedlyrics

    elif [ "$PIP_MANAGER" = "pipx" ]; then
        "$PIP_MANAGER_PATH" install syncedlyrics
    fi

fi

# -------------------------------
# Verify dependencies again
# -------------------------------

echo ""
echo "======== Verifying deps ========="

for cmd in cmake ffmpeg yt-dlp "$PYTHON_CMD"; do
    if ! check_command "$cmd"; then
        echo "Error: $cmd is still missing."
        exit 1
    fi
done

if [ "$IS_TERMUX" = true ] && ! check_command clang; then
    echo "Error: clang is still missing."
    exit 1
fi

if [ "$PIP_MANAGER" = "pip" ]; then

    if ! "$PYTHON_CMD" -c "import syncedlyrics" >/dev/null 2>&1; then
        echo "Error: syncedlyrics is not installed correctly."
        exit 1
    fi

elif [ "$PIP_MANAGER" = "pipx" ]; then

    if ! "$PIP_MANAGER_PATH" list 2>/dev/null | grep -q '^syncedlyrics '; then
        echo "Error: syncedlyrics is not installed correctly."
        exit 1
    fi

fi

echo "All dependencies are ready."
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

printf "Want to copy binary to %s? (Y/N) " "$BIN_DIR"
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
