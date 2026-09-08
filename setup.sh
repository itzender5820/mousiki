#!/usr/bin/env bash

set -e

echo "Installing dependencies..."
sudo apt update
sudo apt install -y ffmpeg yt-dlp mpv python3 python3-pip

echo "Installing Python dependencies..."
python3 -m pip install syncedlyrics

echo "Installing Mousiki configuration..."
mkdir -p "$HOME/.config/mousiki"
cp config.txt "$HOME/.config/mousiki/config.txt"

echo "Building Mousiki..."
cmake -B build
cd build
make -j6

echo "Setup complete!"
