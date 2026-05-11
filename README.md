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
OpenChess can emulate BLE (Bluetooth Low Energy) chess boards and is compatible with [ChessConnect](https://chessconnect.de/) for playing on Chess.com, Lichess and many other platforms.

### How to use
1. Download the [ChessConnect](https://chessconnect.de) app or extension
2. In the OpenChess WebUI, choose a board to emulate and start the BLE server
3. In ChessConnect, select the emulated board and connect
4. Start a game right from ChessConnect
5. Watch the [video guide](https://youtu.be/bYm5_dK6EnQ)

> [!NOTE]
> This feature is **closed-source** and only included in precompiled [GitHub releases](https://github.com/joojoooo/OpenChess/releases). Works fully offline, no Internet required.

## ✨ Features
Features that differentiate this fork from the original Concept-Bytes project:

### 🕹️ Gameplay
- **Lichess**: Play online Lichess games using the Lichess API directly over WiFi (no ChessConnect)
- **Game history**: Saves local (not-online) games so they can be reviewed later. If power is lost during gameplay, the game is automatically recovered on reboot.
- **Check**: Shows a blinking animation on the checked king square and doesn't display or allow illegal moves that would put or leave the king in check
- **GameOver**: Detects when the game is over and shows an animation with the winner color. Enforces 50-move, 3-fold repetition and insufficient material rules
- **Draw/Resign**: Buttons in the WebUI can be used to Draw/Resign or Lift both kings off the board and hold them lifted for 2 seconds to end the game in a Draw.
- **Castling**: Castling is possible by moving the king 2 squares towards the side you want to castle and it will show you where to move the rook.
- **En passant**: Lift the pawn to show the destination square in red and the captured pawn square in purple
- **Promotion**: Can be picked from the WebUI or ChessConnect app. If the WebUI or app are not open, a Queen is automatically picked.
- **Fixes**: Glaring bugs from the Concept-Bytes code that were fixed: Board is rotated correctly (white on the right), Queen and King are on the correct squares and playing infinite moves in a row is no longer possible.

### 🖥️ Hardware & Software
- **Calibration**: Automatically orders GPIOs, shift-register outputs and LED index mapping. No need to care about pin order or LED strip layout. In simple terms: it can rotate/flip the board. Makes it easier to throubleshoot magnet detection issues by printing verbose info in the serial monitor console.
- **Sensors**: Added debounce logic. Allows sliding pieces without getting them immediately detected on the first square they briefly touch. Prevents accidental gamemode selection. Optimized sensor column scan (shift-register) for more reliable sensor reads. Added a way to invert shift-register outputs to be used with PNP transistors
- **Async**: Web server is now Async, website doesn't become unresponsive by moving or not moving a piece. LED animations are now Async and also look cooler
- **WiFi**: On-demand scan for available networks. Supports up to 3 saved networks, fast auto-reconnect via cached BSSID/channel and AP fallback with captive portal
- **Web UI**: Allows for easy ESP32 configuration (WiFi credentials, Lichess token, GPIOs, LED brigthness, OTA, etc.), gamemode selection (with AI/Bot config), displays current board state with evaluation and move history, board edits mid-game. Customizable piece theme and square colors, board flip and zoom, move sounds. Easily editable HTML/CSS/JS files which are automatically minified and compressed into LittleFS.
- **Brightness**: Dark squares now have 70% brightness by default (adjustable in WebUI) this makes them look as bright as light squares (due to contrast)
- **OTA Updates**: Over-the-air firmware and web assets updates. Checks GitHub releases at boot and automatically updates (configurable). Manual Firmware (.BIN) or Web assets (.TAR) updates via drag & drop in Web UI.
- **CI/CD**: GitHub Actions workflow automatically builds firmware, LittleFS image, and web assets TAR on tagged releases. Those file are then used by the [Web Flasher](https://joojoooo.github.io/OpenChess/flash.html) and OTA updates.

## 🤝 Contributing
Contributions are welcome! If you have any new ideas to add or feedback to share, I'd love to hear it!
</br></br>
Please read the [Contributing Guidelines](/CONTRIBUTING.md) before submitting a PR.
</br></br>
[ChessConnect code](/src/chessconnect/) is [git-crypt](https://github.com/AGWA/git-crypt) encrypted. Access to the decryption key may be granted to trusted contributors.
You can still contribute to the open-source parts of the project without the key: the ChessConnect feature is automatically disabled when building from encrypted sources.

## 📄 License
This project is based on [Open-Chess](https://github.com/Concept-Bytes/Open-Chess) by [Concept-Bytes](https://github.com/Concept-Bytes), which is licensed under the [MIT License](/LICENSE-MIT).

Original contributions made by [joojoooo](https://github.com/joojoooo) and all modifications in this repository are licensed under the [PolyForm Noncommercial License 1.0.0](/LICENSE.md).

By submitting a pull request to this repository, you agree that your contribution will be licensed under the PolyForm Noncommercial License 1.0.0.