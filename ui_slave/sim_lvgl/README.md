# OpenChessMate — Desktop LVGL Simulator

A desktop simulator for the OpenChessMate chess UI. Renders the same LVGL-based interface that runs on the ESP32 display, using SDL2 for rendering and a TCP server for protocol messages.

![LVGL v8.3](https://img.shields.io/badge/LVGL-v8.3-blue) ![SDL2](https://img.shields.io/badge/SDL2-required-green)

## Prerequisites

| Dependency | macOS (Homebrew) | Ubuntu / Debian | Windows (MSYS2) |
|------------|-----------------|-----------------|-----------------|
| **CMake** ≥ 3.16 | `brew install cmake` | `sudo apt install cmake` | `pacman -S mingw-w64-x86_64-cmake` |
| **SDL2** | `brew install sdl2` | `sudo apt install libsdl2-dev` | `pacman -S mingw-w64-x86_64-SDL2` |
| **C/C++ compiler** (C11 / C++17) | Xcode CLT: `xcode-select --install` | `sudo apt install build-essential` | `pacman -S mingw-w64-x86_64-gcc` |

LVGL v8.3.0 is fetched automatically by CMake via `FetchContent` — no manual download needed.

## Build

```bash
cd ui_slave/sim_lvgl
mkdir -p build && cd build
cmake ..
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu)
```

The executable is created at `build/ui_slave_lvgl`.

## Run

```bash
./build/ui_slave_lvgl
```

This opens a 480×800 SDL window showing the chess UI (welcome screen → game board) and starts a TCP server on port **8765**.

## Sending Protocol Messages

The simulator accepts the same newline-terminated protocol messages that the ESP32 master sends over serial. Connect via TCP:

```bash
# Using netcat
nc localhost 8765

# Then type messages, e.g.:
MODE|value=1
STATE|fen=rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR;move=e2e4
HINT|move=e7e5
```

### Supported Messages

| Message | Description |
|---------|-------------|
| `MODE\|value=N` | Switch mode: 0=welcome, 1=HvH, 2=Stockfish, 3=Lichess, 4=Sensor Test |
| `STATE\|fen=...;move=...` | Update board position and highlight last move |
| `HINT\|move=e2e4` | Show a hint arrow on the board |
| `CLOCK` | Open the clock setup screen |
| `ERROR` | Display error status |

Outgoing touch/button events are printed to **stdout** (prefixed with `TX:`).

## Project Structure

```
sim_lvgl/
├── CMakeLists.txt          # Build config (fetches LVGL, links SDL2)
├── main.cpp                # SDL2 platform layer + TCP server
├── include/
│   └── lv_conf.h           # LVGL config for desktop (512KB heap, no swap)
└── build/                  # Build output (gitignored)
```

The simulator compiles the shared UI source files directly from `../src/`:
- `chess_ui.cpp` — all LVGL widgets, screens, and game logic
- `pieces/*.c` — piece image data (30×30 LVGL image descriptors)
- `fonts/open_chess_font_32.c` — custom chess piece font

## Troubleshooting

**SDL2 not found (macOS)**
If CMake can't find SDL2 after `brew install sdl2`, try:
```bash
cmake .. -DSDL2_DIR=$(brew --prefix sdl2)/lib/cmake/SDL2
```

**Port 8765 already in use**
Kill any existing instance or change `TCP_PORT` in `main.cpp`.

**Black/blank window**
The simulator starts on the welcome screen. Click a mode button or send `MODE|value=1` via TCP to switch to the game board.
