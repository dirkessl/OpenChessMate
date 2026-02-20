# OpenChess - Smart Chess Board [![Build & Release](https://github.com/joojoooo/OpenChess/actions/workflows/release.yml/badge.svg)](https://github.com/joojoooo/OpenChess/actions/workflows/release.yml)

OpenChess is a smart chessboard, it can show legal moves, plays against you using Stockfish or let you play online Lichess games on the physical board. It saves all games, allowing you to review them later

<p align="center"><img src="docs/BuildGuide/OpenChess - Plastic PCB (Bot config).webp" width="50%"></p>

## 🔨 Build Guide

**👉 [Step-by-step build guide](https://joojoooo.github.io/OpenChess)** - covers materials, schematics, assembly, and [software setup](https://joojoooo.github.io/OpenChess/flash.html)

## ☕ Support the project
Love this project? You can [support it on Ko-fi](https://ko-fi.com/joojooo) Every contribution makes a difference!

[![ko-fi](https://www.ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/joojooo)

## ✨ Features
Features that differentiate this fork from the original Concept-Bytes project:

- **Game history**: Saves all the games in LittleFS storage so they can be reviewed later. If power is lost during gameplay, the game is automatically recovered on reboot.
- **Lichess**: Play online Lichess games on your physical board!
- **Check**: Detects checks, doesn't display or allow illegal moves that would put or leave your king in check
- **Checkmate/Stalemate**: Detects when the game is over and shows an animation with the winner color (Blue for black, Cyan for draws). Enforces 50-move, 3-fold repetition and insufficient material rules
- **Draw/Resign**: Lift both kings (non playing color first) off the board and hold them lifted for 2 seconds to end the game.
- **Castling**: Castling is now possible, just move the king 2 squares towards the side you want to castle and it will show you where to move the rook.
- **En passant**: En passant captures are now possible. Also correctly sets en passant square in the FEN (so Stockfish can take en passant too)
- **Bot**: You can now pick the bot starting side and difficulty
- **Turns**: Doesn't allow white to play infinite moves in a row, enforces turns
- **Calibration**: Automatically orders GPIOs, shift-register outputs and LED index mapping. You won't need to care about pin order or LED strip layout. In simple terms: it can rotate/flip the board. Also makes it easier to throubleshoot magnet detection issues by printing info in the serial monitor console.
- **QK**: Queen and King now on the correct squares!
- **Async**: Web server is now Async, website doesn't become unresponsive by moving or not moving a piece. LED animations are now Async and also look cooler
- **Sensors**: Added real debounce logic. Allows you to slide pieces without getting them immediately detected on the first square they briefly touch. Prevents accidental gamemode selection. Optimized sensor column scan (shift-register) for more reliable sensor reads
- **Web UI**: Many improvements to the Web UI, now has functionality instead of being completely useless: displays evaluation and board state correctly, allows for board edit (drag n drop pieces), WiFi credentials and Lichess token change and save, gamemode selection. Customizable piece theme and square colors, board flip and zoom, move sounds. Now has easily editable HTML/CSS/JS files (instead of crazy string concatenations) which are automatically minified and compressed into LittleFS.
- **Brightness**: Dark squares now have 70% brightness by default. Adjustable in WebUI. This is because the perceived light is more on a dark background, which gives more contrast. With this simple change the dark squares look as bright as the light squares.
- **OTA Updates**: Over-the-air firmware and web assets updates. Automatically checks GitHub releases at boot (configurable toggle). Manual Firmware (.BIN) or Web assets (.TAR) updates via drag & drop in Web UI.
- **CI/CD**: GitHub Actions workflow automatically builds firmware, LittleFS image, and web assets TAR on tagged releases. Those file are then used by the web flasher and OTA updates.

## Contributing
Contributions are welcome!
If you have any new ideas to add or feedback to share, I'd love to hear it!

## License

This project is based on [Open-Chess](https://github.com/Concept-Bytes/Open-Chess) by [Concept-Bytes](https://github.com/Concept-Bytes), which is licensed under the [MIT License](/LICENSE-MIT).

Original contributions made by [joojoooo](https://github.com/joojoooo) and all modifications in this repository are licensed under the [PolyForm Noncommercial License 1.0.0](/LICENSE.md).

By submitting a pull request to this repository, you agree that your contribution will be licensed under the PolyForm Noncommercial License 1.0.0.