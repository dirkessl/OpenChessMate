# OpenChess - Smart Chess Board

OpenChess is a smart chessboard, it can show legal moves, plays against you using Stockfish or let you play online Lichess games on the physical board. It saves all games, allowing you to review them later

<p align="center">
   <img src="BuildGuide/51_PCB_SOLDER_ROWS_N_COLS.webp" height="400px">
   <img src="BuildGuide/schematics/ESP32_PCB.webp" height="400px">
</p>

##  Features (main differences with the original repo)
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

##  Hardware Requirements
- **Dev Board**: The cheapest ESP32 you can find
- **Led Strip**: WS2812B LED Strip 30LEDs/m 3 meters
- **Hall Effect Sensors**: Hall Effect Sensor TO-92 A3144 (64PCS)
- **Shift Register**: 74HC595 For sensor matrix scanning (1PCS)
- **Iron discs**: 12x1mm (64PCS) Spreads the magnetic field, without it magnets won't be easily detected and the pieces will stick to each other (washers don't work, must be a disc)
- **Magnets**:  Neodymium magnetes 8x4mm (32PCS) or 8x2mm (64PCS) (8x2mm is weak, stacking 2 is required)
- **USB-C Female port**: 1PCS Allows for power injection at different points in the LED strip
- **Resistors**: Any value from 10k to 100k Ohm works fine, prefer 10k
- **Level Shifter**: 3.3V to 5V, 4 Channels. Needed for LED data and Shift Register signals (using the ESP32 GPIO 3.3V directly is not reliable)
- **PNP transistors**: 2N3906, BC557, etc (8PCS) For powering the A3144 sensors (5V Vcc) (using shift register outputs directly causes voltage drop and damages the SR)
##  Quick Start

### 1. Hardware Setup
1. Wire the led strip Data IN pin to GPIO 32
2. Wire shift register pins to: SRCLR'=5V OE'=GND SRCLK=GPIO14 RCLK=GPIO26 SER=GPIO33
3. Connect HE sensor output rows to pins: [4, 16, 17, 18, 19, 21, 22, 23]
4. Connect each HE sensor 5V column to each shift register output
5. Embed magnets in chess piece bases. Embed the iron discs under the board

### 2. Software Configuration
1. Open the .code-workspace file in Visual studio Code
2. Install the **PlatformIO IDE** extension
3. Edit **board_driver.h** if you're using different GPIO pins (row pin order doesn't matter)
4. Edit **wifi_manager_esp32.h** with your WiFi credentials (or edit them later via Web UI)
5. Click Upload (Ctrl+Alt+U)
6. On first boot follow the Serial monitor instructions to calibrate the board

### 3. Game Selection
1. Power on the board
2. Wait for LEDs in the center
3. Place any piece on a LED to select mode:
   - **Gold**: Human vs Human
   - **White**: Human vs Bot
   - **Purple**: Lichess
   - **Red**: Sensor Test

##  How to Play

### Human vs Human
1. Set up pieces in starting position
2. Pick up your piece (valid moves in white, capture in red)
3. Place on valid square (green confirmation flash)
4. Opponent's turn

### Human vs Bot
1. Connect to WiFi (automatic with credentials)
2. Set up pieces in starting position
3. Choose bot side and difficulty
4. Follow LED indicators to make Bot's move physically

### Lichess
1. Connect to WiFi (automatic with credentials)
2. Save your Lichess API token in the WebUI
3. Start a game on Lichess
4. Follow LED indicators to make Lichess's move physically

### Sensor Test
1. LEDs light up where pieces are detected
2. Remove/add pieces, leds will light up
3. This fork implements a REAL calibration feature, this one is useless in comparison and will probably be removed

##  Troubleshooting
Enable Serial Monitor (115200 baud) for detailed diagnostic information

##  Contributing

Contributions are welcome!
If you have any new ideas to add or feedback to share, I'd love to hear it!

##  License

PolyForm Noncommercial License 1.0.0 - See [LICENSE.md](/LICENSE.md) for details (applies to code added after the fork, the original repo doesn't have a LICENSE file)
