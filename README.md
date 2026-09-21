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

[![License](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](https://github.com/itzender5820/mousiki/blob/main/LICENSE)
[![Language](https://img.shields.io/badge/Language-C++17-orange.svg)](https://github.com/itzender5820/mousiki)
[![Platform](https://img.shields.io/badge/Platform-Linux_%7C_Android_%7C_MacOS-brightgreen.svg)](https://github.com/itzender5820/mousiki)

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
- **External Player Control:** Drive music playing in your browser or the Spotify desktop app straight from the TUI.
- **Highly Configurable:** Tweak colors, visualizer fluidity, animations, and hotkeys to match your exact workflow.

## 🚀 Supported Platforms

- **Native Support:** **Linux**, **macOS**, and **Android (Termux)**.
- **Unverified Support:** *Windows*. (Support for Windows is currently not verified because I don't have the hardware access needed to test and debug on that operating system. If you try it out and get it working, feel free to contribute!)

## 🛠️ Getting Started
<div align="center">
  
## Default Keybindings

Configurable in `$HOME/.config/mousiki/config.txt`.

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
| **External Players** | `o` | Attach / detach an external player |

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

Mousiki relies on a few external tools for audio fetching, decoding, and lyrics. The easiest way to get started is by running the setup script on macOS (requires [Homebrew](https://brew.sh)), Debian-based Linux, or Termux:

```bash
# Clone the repository
git clone https://github.com/itzender5820/mousiki.git
cd mousiki

# Run the setup script (installs dependencies, sets up config, and builds the app)
bash setup.sh
```

If you're building manually, ensure you have `cmake`, a C++17 compiler, `ffmpeg`, `yt-dlp`, and the Python `requests` package installed.

Optionally, install [`playerctl`](https://github.com/altdesktop/playerctl) for external player control on Linux. It isn't needed for local or streaming playback — and if you'd rather not install anything, Mousiki falls back to `busctl` (ships with systemd) or `gdbus` (ships with glib), one of which you almost certainly already have.

### Running the App

After a successful build, you can start the player with:
```bash
./build/mousiki
```

## 🎛️ External Player Control

Mousiki can also drive music that isn't playing inside it — a track in your browser (YouTube, SoundCloud, or any page with media) or the Spotify desktop app.

Press `o` to open the **External Players** panel, pick a detected player with the arrow keys, and press `ENTER` to attach. While attached, the usual transport keys are forwarded to that player instead of Mousiki's own playback: `p` play/pause, `n` / `b` next / previous, `ARROW_LEFT` / `ARROW_RIGHT` seek, `1` / `2` volume, and `x` mute. The metadata panel and progress bar follow the external track, and synced lyrics are fetched for it just like a local one. Press `o` again to detach and return to controlling Mousiki (inside the panel, `ESC` just closes it without changing what you're attached to, and `d` detaches). Attaching pauses local playback by default, so the two never play over each other.

On **Linux** this rides on MPRIS, the standard D-Bus remote-control protocol that Firefox, Chromium/Chrome, Spotify, VLC, mpv, and most other players already speak. Mousiki links no D-Bus library; exactly as it shells out to `yt-dlp` and `ffmpeg`, it talks to MPRIS through whichever of `playerctl`, `busctl`, or `gdbus` it finds on your `PATH` — in that order of preference. `playerctl` is the nicest of the three and the one worth installing.

On **macOS** there is no MPRIS: only the **Spotify** and **Music** apps can be controlled, through `osascript`. Browser media is *not* controllable on macOS — there's no scriptable equivalent. On **Windows** and **Android (Termux)**, external control is not supported at all.

## ⚙️ Configuration

Your configuration file will be automatically generated at `$HOME/.config/mousiki/config.txt`. From there, you have complete freedom to customize Mousiki.

### Adding Custom Music Paths
You can easily tell Mousiki where to look for your music. Simply add multiple `LocalMusicPath` entries in your `config.txt`:

```ini
# Add as many custom paths as you need:
LocalMusicPath=/custom/path
LocalMusicPath=/home/user/Music
```

### Tuning External Player Control
External control is on by default. These four keys shape it:

```ini
ExternalControl=true         # Master on/off switch
ExternalPollIntervalMs=500   # How often the attached player is re-polled (200-5000; each poll is a subprocess round-trip)
ExternalAutoPauseLocal=true  # Pause local playback when attaching
ColorExternal=               # Accent color for the panel (empty = same as the list)
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
