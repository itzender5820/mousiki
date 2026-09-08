# Mousiki 🎵

![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)
![Language](https://img.shields.io/badge/Language-C++17-orange.svg)
![Platform](https://img.shields.io/badge/Platform-Linux_%7C_Android-brightgreen.svg)

Hey there! Welcome to **Mousiki**, a terminal music player built from the ground up for people who prefer control, simplicity, and a keyboard.

I created Mousiki because I wanted a fast, focused TUI (Terminal User Interface) without any unnecessary interface layers. It’s designed to be completely keyboard-driven and fully configurable while giving you rich features like spectrum visualizers, synced lyrics, and online streaming—all without ever leaving your terminal.

## Preview

![Mousiki Preview](./rawlook.png)

## ✨ Features

- **Local Music Playback:** Instantly browse and play your local music files.
- **Online Search & Streaming:** Search and stream tracks directly from online sources.
- **Synced Lyrics:** Real-time, word-by-word active lyrics highlighting as the song plays.
- **Visualizers:** Real-time FFT spectrum, waveform rendering, and spinning disk art.
- **Queue Management:** Effortless queueing, shuffling, and repeating.
- **Highly Configurable:** Tweak colors, visualizer fluidity, animations, and hotkeys to match your exact workflow.

## 🚀 Supported Platforms

- **Native Support:** **Linux** and **Android (Termux)**.
- **Unverified Support:** *Windows* and *macOS*. (Support for these platforms is currently not verified because I don't have the hardware access needed to test and debug on those specific operating systems. If you try it out and get it working, feel free to contribute!)

## 🛠️ Getting Started

### Prerequisites & Installation

Mousiki relies on a few external tools for audio fetching, decoding, and lyrics. The easiest way to get started is by running the setup script on a Debian-based Linux system or Termux:

```bash
# Clone the repository
git clone https://github.com/itzender5820/mousiki.git
cd mousiki

# Run the setup script (installs dependencies, sets up config, and builds the app)
bash setup.sh
```

If you're building manually, ensure you have `cmake`, a C++17 compiler, `ffmpeg`, `yt-dlp`, and the Python `syncedlyrics` package installed.

### Running the App

After a successful build, you can start the player with:
```bash
./build/mousiki
```

## ⚙️ Configuration

Your configuration file will be automatically generated at `$HOME/.config/mousiki/config.txt`. From there, you have complete freedom to customize Mousiki.

### Adding Custom Music Paths
You can easily tell Mousiki where to look for your music. Simply add multiple `LocalMusicPath` entries in your `config.txt`:

```ini
# Add as many custom paths as you need:
LocalMusicPath=/custom/path
LocalMusicPath=/home/user/Music
```

## 🙏 Attribution & Dependencies

Mousiki stands on the shoulders of giants. A huge thank you to the developers behind these awesome open-source projects that make Mousiki tick:

- **[miniaudio](https://github.com/mackron/miniaudio):** An incredible single-file audio playback and capture library.
- **[kissfft](https://github.com/mborgerding/kissfft):** A wonderfully simple and lightweight real-input FFT library (powering the spectrum visualizer).
- **[yt-dlp](https://github.com/yt-dlp/yt-dlp):** The backend magic for our online search and streaming capabilities.
- **[syncedlyrics](https://github.com/rtcq/syncedlyrics):** Python package fetching the synced lyrics data.
- **[FFmpeg](https://ffmpeg.org/):** The Swiss army knife of multimedia handling.

## 📜 License

This project is open-sourced under the [Apache License 2.0](LICENSE). 

---
*Crafted with ❤️ for the terminal by [itzender5820](https://github.com/itzender5820)*
