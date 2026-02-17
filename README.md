# OpenChess - Smart Chess Board

OpenChess is a smart chessboard, it can show legal moves, plays against you using Stockfish or let you play online Lichess games on the physical board. It saves all games, allowing you to review them later

<p align="center"><img src="docs/BuildGuide/OpenChess - Plastic PCB (Bot config).webp" width="50%"></p>

## 🔨 Build Guide

**👉 [Step-by-step build guide with photos](https://joojoooo.github.io/OpenChess)** - covers materials, schematics, assembly, and software setup

## ☕ Support the project
Love this project? You can [support it on Ko-fi](https://ko-fi.com/joojooo) Every contribution makes a difference!

[![ko-fi](https://www.ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/joojooo)

## ✨ Features
Features that differentiate this fork from the original Concept-Bytes project:

- **Lichess**: Play online Lichess games on your physical board!
- **Turns**: Doesn't allow white to play infinite moves in a row, now turns are enforced
- **Check**: Detects checks, doesn't display or allow illegal moves that would put or leave your king in check
- **Checkmate/Stalemate**: Automatically detects when the game is over and shows an animation with the winner color (Blue for black) or cyan if it's a draw (also enforces 50-move and threefold repetition rules)
- **Castling**: Castling is now a valid move! Finally you get to finish a game without the bot getting stuck because of impossible FEN with wrong castling rights
- **En passant**: En passant captures are now possible. Also correctly sets en passant square in the FEN (so Stockfish can take en passant too)
- **Bot**: You can now pick the bot starting side and difficulty
- **Calibration**: Allows you to rotate the board, so you can finally play in the correct orientation (original orientation is wrong). Allows for easier debug of sensors grid issues
- **QK**: Queen and King now on the correct squares!
- **Async**: Web server is now Async, website doesn't become unresponsive by moving or not moving a piece. LED animations are now Async and also look cooler
- **Sensors**: Added real debounce logic. Allows you to slide pieces without getting them immediately detected on the first square they briefly touch. Prevents accidental gamemode selection. Optimized sensor column scan (shift-register) for more reliable sensor reads
- **Web UI**: Many improvements to the Web UI, now has functionality instead of being completely useless: displays evaluation and board state correctly, allows for board edit (drag n drop pieces), WiFi credentials and Lichess token change and save, gamemode selection. Customizable piece theme and square colors, board flip and zoom, move sounds. Now has easily editable HTML/CSS/JS files (instead of crazy string concatenations) which are automatically minified and compressed into LittleFS.
- **Game history**: Saves all the games in LittleFS storage so they can be reviewed later. If power is lost during gameplay, it automatically recovers where you left it on reboot.
- **Brightness**: Dark squares now have 70% brightness by default. Adjustable in WebUI. This is because the perceived light is more on a dark background, which gives more contrast. With this simple change the dark squares look as bright as the light squares.

## Contributing
Contributions are welcome!
If you have any new ideas to add or feedback to share, I'd love to hear it!

## License

PolyForm Noncommercial License 1.0.0 - See [LICENSE.md](/LICENSE.md) for details (applies to code added after the fork, the original repo doesn't have a LICENSE file)
