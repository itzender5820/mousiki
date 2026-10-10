<div align="center">

 # Mousiki 🎵

<p align="center">
  <a href="https://opensource.org/" target="_blank">
    <img src="https://i0.wp.com/opensource.org/wp-content/uploads/2023/03/cropped-OSI-horizontal-large.png?fit=640%2C229&quality=80&ssl=1" alt="OSI" height="52" /></a>
&nbsp;
  <a href="https://www.apache.org/" target="_blank">
    <img src="https://www.apache.org/images/oakleaf.svg" alt="Apache" height="52" /></a>
</p>


> [!NOTE]
> **Developer note:** Mousiki is released under the Apache License 2.0.
> You are free to use, modify, fork, re-distribute, and sell the software,
> subject to the terms of the license.

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://github.com/Basheer-io/mousiki/blob/main/LICENSE)
[![Language](https://img.shields.io/badge/Language-C++17-orange.svg)](https://github.com/Basheer-io/mousiki)
[![Platform](https://img.shields.io/badge/Platform-Windows_%7C_Linux_%7C_MacOS_%7C_Android-brightgreen.svg)](https://github.com/Basheer-io/mousiki)

Mousiki is a terminal music player built from the ground up for people who prefer control, simplicity, and a keyboard. It's a fast, focused TUI (Terminal User Interface) without unnecessary interface layers — fully keyboard-driven and configurable, with spectrum visualizers, synced lyrics, and online streaming, all without leaving your terminal.

``Personal preference is not a compromise—it's the design goal.``
 
## Preview

![Mousiki Preview](./preview.gif)

</div>
## ✨ Features

- **Local Music Playback:** Instantly browse and play your local music files.
- **Online Search & Streaming:** Search and stream tracks directly from online sources.
- **Synced Lyrics:** Real-time, word-by-word active lyrics highlighting as the song plays.
- **Visualizers:** Real-time FFT spectrum, waveform rendering, and spinning disk art.
- **Queue Management:** Effortless queueing, shuffling, and repeating.
- **Highly Configurable:** Tweak colors, visualizer fluidity, animations, and hotkeys to match your exact workflow.

## 🚀 Supported Platforms

- **Native Support:** **Windows 10/11** (Windows Terminal & PowerShell), **Linux**, **macOS**, and **Android (Termux)**.

## 🛠️ Getting Started
<div align="center">
  
## Default Keybindings

Configurable in `$HOME/.config/mousiki/config.txt` (or `%USERPROFILE%\.config\mousiki\config.txt` on Windows).

### Search & Playback
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Local Search** | `/` | Filter and search local library |
| **Online Stream Search** | `/s: <query>` | Search and stream music online |
| **Download Stream** | `y` | Download currently streaming track |
| **Play / Pause** | `p` (or `ENTER`) | Toggle playback |
| **Next / Previous Track** | `n` / `b` | Skip between songs |
| **Seek** | `ARROW_LEFT` / `ARROW_RIGHT` | Seek backward / forward |
| **Volume** | `1` / `2` | Decrease / Increase volume |
| **Shuffle / Repeat** | `m` / `r` | Toggle shuffle or repeat mode |

### Navigation & Queue
| Action | Keybinding | Description |
| :--- | :--- | :--- |
| **Navigate** | `ARROW_UP` / `ARROW_DOWN` | Move selection |
| **Switch Tabs/Cards** | `TAB` | Cycle between UI panels |
| **Add to Queue** | `a` | Enqueue selected track |
| **Remove from Queue** | `d` | Dequeue selected track |
| **Filter by Folder** | `f` | Apply folder filter |
| **Clear Filter** | `c` | Reset active search/filters |
| **Quit** | `q` | Exit application |

</div>

### Prerequisites & Installation

Mousiki relies on a few external tools for audio fetching, decoding, and lyrics: `ffmpeg`, `yt-dlp`, and Python with the `requests` package.

#### 🪟 Windows (PowerShell / Windows Terminal)

Make sure you have Visual Studio C++ Build Tools or CMake installed:

```powershell
# Clone the repository
git clone https://github.com/Basheer-io/mousiki.git
cd mousiki

# Run the setup script (checks dependencies, sets up config, and builds mousiki.exe)
powershell -ExecutionPolicy Bypass -File .\setup.ps1
```

Start the player:
```powershell
.\build\mousiki.exe
```

*(Recommended: Run inside **Windows Terminal** with **Cascadia Code** or a Nerd Font for crisp box-drawing and visualizer animations).*

#### 🐧 Linux / 🍎 macOS / 📱 Android (Termux)

The setup script handles dependencies on macOS (requires [Homebrew](https://brew.sh)), Debian-based Linux, and Termux:

```bash
# Clone the repository
git clone https://github.com/Basheer-io/mousiki.git
cd mousiki

# Run the setup script (installs dependencies, sets up config, and builds the app)
bash setup.sh
```

Start the player:
```bash
./build/mousiki
```

## ⚙️ Configuration

Your configuration file will be automatically generated at:
- **Windows:** `%USERPROFILE%\.config\mousiki\config.txt`
- **Linux / macOS:** `$HOME/.config/mousiki/config.txt`

From there, you have complete freedom to customize Mousiki.

### Adding Custom Music Paths
You can easily tell Mousiki where to look for your music. Simply add multiple `LocalMusicPath` entries in your `config.txt`:

```ini
# Add as many custom paths as you need:
LocalMusicPath=~/Music
LocalMusicPath=D:\MyMusic
```

## 🙏 Attribution & Dependencies

Mousiki stands on the shoulders of giants. A huge thank you to the developers behind these awesome open-source projects that make Mousiki tick:

- **[miniaudio](https://github.com/mackron/miniaudio):** An incredible single-file audio playback and capture library.
- **[kissfft](https://github.com/mborgerding/kissfft):** A wonderfully simple and lightweight real-input FFT library (powering the spectrum visualizer).
- **[yt-dlp](https://github.com/yt-dlp/yt-dlp):** The backend magic for our online search and streaming capabilities.
- **requests:** Python package used by `scripts/lrc.py` to fetch synced lyrics from Better Lyrics (primary) and [LRCLIB](https://lrclib.net) (fallback).
- **[FFmpeg](https://ffmpeg.org/):** The Swiss army knife of multimedia handling.

<div align="center">

## 📜 License

This project is open-sourced under the [Apache License 2.0](LICENSE). 

## Star History

<a href="https://www.star-history.com/?repos=itzender5820%2Fmousiki&type=date&legend=top-left">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/chart?repos=itzender5820/mousiki&type=date&theme=dark&legend=top-left" />
    <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/chart?repos=itzender5820/mousiki&type=date&legend=top-left" />
    <img alt="Star History Chart" src="https://api.star-history.com/chart?repos=itzender5820/mousiki&type=date&legend=top-left" />
  </picture>
</a>

---
*Crafted with ❤️ for the terminal by [itzender5820](https://github.com/itzender5820)*


<p align="center">
  <img
    src="https://raw.githubusercontent.com/mayankchaudhary26/Cool-Readme-ideas/refs/heads/master/data/trust%20me.gif"
    alt="Trust me"
  />
</p>

</div>
