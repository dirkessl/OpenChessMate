# OpenChessMate — Smart Chess Board Addon

<img width="1129" height="716" alt="OpenChessMateRendering" src="https://github.com/user-attachments/assets/8e309647-229b-466c-babd-c80fda3a973b" />

## What is OpenChessMate?

**OpenChessMate** is an addon project for the [OpenChess](https://github.com/joojoooo/OpenChess) smart chessboard. Its primary goal is to add a **standalone touch display** to the OpenChess board, powered by a second ESP32 running an [LVGL](https://lvgl.io/)-based UI.

With the display addon you can:

- **Start and manage games** directly from the board — no phone or computer required.
- **Switch game modes** (human vs. human, vs. Stockfish AI, or online via Lichess).
- **Run a chess clock** for over-the-board matches.
- Configure settings such as AI difficulty, LED brightness, and WiFi credentials.

The main board firmware (this repo) handles all chess logic, hardware sensors, LED feedback, and WiFi connectivity. The display slave firmware lives in the [`ui_slave/`](ui_slave/) folder and communicates with the main board over Serial.

> **Building the physical chessboard?**
> The hardware build guide (PCB design, hall-effect sensors, LED strip, wiring, etc.) belongs to the original **OpenChess** project.
> Please visit the original repository for all hardware construction details:
> 👉 **[github.com/joojoooo/OpenChess](https://github.com/joojoooo/OpenChess)**

## Features (Main Board)

- Detects piece movements via hall-effect sensors and a shift-register grid.
- Shows legal moves, highlights check, and animates captures with a WS2812B LED strip.
- Plays against you using the [Stockfish online API](https://stockfish.online/) at four difficulty levels.
- Streams and submits moves for live [Lichess](https://lichess.org/) games on the physical board.
- Saves game settings and credentials to ESP32 NVS (non-volatile storage).
- Hosts a built-in web interface (served from LittleFS) for configuration and game management.

## Repository Structure

| Path | Description |
|------|-------------|
| `src/` | Main ESP32 firmware (chess logic, board driver, WiFi manager) |
| `ui_slave/` | Second ESP32 firmware — LVGL touch display addon |
| `data/` | Web assets for the built-in web interface (gzip-compressed, committed to git) |
| `docs/` | Web flash tool and build guide images |
| `platformio.ini` | PlatformIO build configuration |

## Getting Started

1. **Clone** this repository.
2. Open in **VS Code** with the [PlatformIO IDE](https://platformio.org/) extension.
3. Build and upload the main firmware: `Ctrl+Alt+B` then `Ctrl+Alt+U`.
4. *(Optional)* Build and upload the `ui_slave/` project to a second ESP32 connected to a touch display.

For full hardware assembly instructions, see the original OpenChess repo linked above.

## Contributing

Contributions are welcome!
If you have any new ideas to add or feedback to share, feel free to open an issue or pull request.

For hardware-related features of the OpenChess board itself, please contribute to the [original repo](https://github.com/joojoooo/OpenChess).

## License

This project is based on [Open-Chess](https://github.com/Concept-Bytes/Open-Chess) by [Concept-Bytes](https://github.com/Concept-Bytes), which is licensed under the [MIT License](/LICENSE-MIT).

Original contributions made by [joojoooo](https://github.com/joojoooo) and all modifications in this repository are licensed under the [PolyForm Noncommercial License 1.0.0](/LICENSE.md).

By submitting a pull request to this repository, you agree that your contribution will be licensed under the PolyForm Noncommercial License 1.0.0.
