# OpenChess - Smart Chess Board [![Build & Release](https://github.com/joojoooo/OpenChess/actions/workflows/release.yml/badge.svg)](https://github.com/joojoooo/OpenChess/actions/workflows/release.yml)
OpenChess is a smart chessboard, it can show legal moves, plays against you using Stockfish or let you play online games on your physical board.

<p align="center"><img src="docs/BuildGuide/OpenChess - Plastic PCB (Bot config).webp" width="50%"></p>

## ☕ Support the project
Love this project? You can [support it on Ko-fi](https://ko-fi.com/joojooo) Every contribution makes a difference!

[![ko-fi](https://www.ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/joojooo)

## 🛠️ Build Guide
- **👣 [Step-by-step build guide](https://joojoooo.github.io/OpenChess)** - covers materials, schematics, assembly, and [software setup](https://joojoooo.github.io/OpenChess/index.html#software)
- **⚡ [Web Flasher](https://joojoooo.github.io/OpenChess/flash.html)** - easily flash your ESP32 directly from the browser

## ♟️ ChessConnect
This fork can emulate BLE (Bluetooth Low Energy) chess boards and is compatible with [ChessConnect](https://chessconnect.de/) for playing on Chess.com, Lichess and many other platforms.

### How to use
1. Download the [ChessConnect](https://chessconnect.de) app or extension
2. In the OpenChess WebUI, choose a board to emulate and start the BLE server
3. In ChessConnect, select the emulated board and connect
4. Start a game right from ChessConnect
5. Watch the [video guide](https://youtu.be/bYm5_dK6EnQ)

> [!NOTE]
> This is a premium feature with a free trial. It's **closed-source** and only included in precompiled [GitHub releases](https://github.com/joojoooo/OpenChess/releases)

## ✨ Features
Highlights of this fork compared with the original Concept-Bytes project:

### 🕹️ Gameplay
- **Lichess**: Play online over WiFi using the Lichess API, without ChessConnect
- **Game history**: Save and review local games, with power loss recovery
- **Legal moves**: Detects check, checkmate, draws, castling and en passant
- **Promotion**: Pick the promotion piece from the physical board or Web UI
- **Draw/Resign**: Use the Web UI buttons, or lift both kings to draw/end a game
- **Fixes**: Correct board rotation, king/queen placement and turn handling

### 🖥️ Hardware & Software
- **Calibration**: Maps GPIOs, shift-register outputs, and LEDs so the board rotation doesn't depend on wiring
- **Sensors**: Debounce and optimized scanning make piece detection more reliable
- **Responsive**: Async web server and LED animations keep the board and Web UI available at any time
- **WiFi**: Network scan, up to 3 saved networks, fast reconnect, captive portal, mDNS
- **Web UI**: Configure the ESP32, view logs, choose game modes, view and edit current position, review games, etc...
- **OTA updates**: Update firmware and web assets automatically from GitHub releases or manually from the Web UI.
- **Release builds**: GitHub Actions builds firmware, LittleFS images, and web assets for the [Web Flasher](https://joojoooo.github.io/OpenChess/flash.html) and OTA updates

## 🤝 Contributing
Contributions are welcome! If you have any new ideas to add or feedback to share, I'd love to hear it!
</br></br>
Please read the [Contributing Guidelines](/CONTRIBUTING.md) before submitting a PR.
</br></br>
[ChessConnect code](/src/chessconnect/) is [git-crypt](https://github.com/AGWA/git-crypt) encrypted.
You can still contribute to the open-source parts of the project: the ChessConnect feature is automatically disabled when building from encrypted sources.

## 📄 License
This project is based on [Open-Chess](https://github.com/Concept-Bytes/Open-Chess) by [Concept-Bytes](https://github.com/Concept-Bytes), which is licensed under the [MIT License](/LICENSE-MIT).

Original contributions made by [joojoooo](https://github.com/joojoooo) and all modifications in this repository are licensed under the [PolyForm Noncommercial License 1.0.0](/LICENSE.md).

By submitting a pull request to this repository, you agree that your contribution will be licensed under the PolyForm Noncommercial License 1.0.0.