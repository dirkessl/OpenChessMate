# OpenChessMate UI Slave — Serial Protocol API

## Connection

| Parameter        | Value                                  |
|------------------|----------------------------------------|
| **Interface**    | UART0 (ESP32-S3)                       |
| **TX pin**       | IO43                                   |
| **RX pin**       | IO44                                   |
| **Baud rate**    | 115200                                 |
| **Line format**  | Newline-terminated (`\n`). `\r` is silently ignored. |
| **Max line**     | 1024 bytes                             |
| **Encoding**     | ASCII                                  |

> The simulator uses TCP port **8765** instead of serial (same message format).

---

## Messages: Master → Slave (incoming)

### `MODE|value=<N>`

Switch the UI mode / screen.

| Param   | Type | Values                                                        |
|---------|------|---------------------------------------------------------------|
| `value` | int  | `0` = Welcome screen, `1` = Human vs Human, `2` = vs Stockfish, `3` = Online (Lichess), `4` = Sensor Test |

**Behaviour:**
- `value=0` — returns to the welcome / mode-selection screen.
- `value=1–4` — switches to the game screen, resets the board to the starting position, resets the chess clock and move history.
  - Mode 1 (HvH) shows dual player areas with a swap-sides button.
  - Modes 2–4 show a single control area with hint / undo / resign / new / home buttons.

---

### `STATE|fen=<FEN>[;move=<UCI>]`

Update the board position and optionally highlight the last move.

| Param  | Required | Type   | Description                                              |
|--------|----------|--------|----------------------------------------------------------|
| `fen`  | yes      | string | Piece-placement part of a FEN string (ranks separated by `/`, e.g. `rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR`) |
| `move` | no       | string | Last move in UCI notation (e.g. `e2e4`). Four characters: file+rank of source, file+rank of destination. |

**Behaviour:**
- Redraws all pieces on the board according to `fen`.
- If `move` is present:
  - Highlights the from/to squares on the board.
  - Updates the status label with the move text.
  - Appends the move to the on-screen move history list.
  - **Clock:** On the first move the chess clock starts running. On subsequent moves, the Fischer increment is added to the player who just moved and the active clock side toggles.
  - **HvH:** Hides the swap-sides button after the first move.

**Example:**
```
STATE|fen=rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR;move=e2e4
```

---

### `HINT|move=<UCI>`

Show a hint on the board.

| Param  | Required | Type   | Description                            |
|--------|----------|--------|----------------------------------------|
| `move` | yes      | string | Suggested move in UCI notation (e.g. `e7e5`) |

**Behaviour:**
- Highlights the from/to squares and sets the status label to `"Hint: <move>"`.

**Example:**
```
HINT|move=e7e5
```

---

### `CLOCK`

Open the clock configuration screen (no parameters).

**Behaviour:**
- Hides the current game screen and shows the clock preset / custom setup screen.

---

### `ERROR`

Display an error state (no parameters).

**Behaviour:**
- Sets the status label to `"Error"`.

---

## Messages: Slave → Master (outgoing)

All outgoing messages use the format:

```
TOUCH|action=<action>;<key>=<value>;...\n
```

### Board tap

```
TOUCH|action=board;row=<R>;col=<C>\n
```

| Param | Type | Description                                              |
|-------|------|----------------------------------------------------------|
| `row` | int  | Row 0–7 (0 = rank 8 / top of board, 7 = rank 1 / bottom) |
| `col` | int  | Column 0–7 (0 = file a, 7 = file h)                      |

**Trigger:** User taps a square on the chess board.

---

### Mode selection

```
TOUCH|action=mode;value=<N>\n
```

| Param   | Type | Description                                                |
|---------|------|------------------------------------------------------------|
| `value` | int  | `1` = HvH, `2` = Stockfish, `3` = Lichess, `4` = Sensor Test |

**Trigger:** User taps a mode button on the welcome screen. The slave also switches to the game screen locally.

---

### Hint request

```
TOUCH|action=hint;x=0;y=0\n
```

**Trigger:** User taps the "Hint" button.

---

### Undo

```
TOUCH|action=undo;x=0;y=0\n
```

**Trigger:** User taps the "Undo" / "Back" button.

---

### Home (return to welcome)

```
TOUCH|action=home;x=0;y=0\n
```

**Trigger:** User confirms the "Return to home screen?" dialog. The slave also returns to the welcome screen locally.

---

### New game

```
TOUCH|action=new;x=0;y=0\n
```

**Trigger:** User confirms the "Start a new game?" dialog.

---

### Resign

```
TOUCH|action=resign;x=0;y=0\n
```

**Trigger:** User taps the "Resign" button.

---

### Swap sides (HvH only)

```
TOUCH|action=swap;x=0;y=0\n
```

**Trigger:** User taps the "Swap" button (only visible in HvH mode before the first move).

---

## Typical Interaction Flow

```
MASTER                              SLAVE (display)
  │                                    │
  │  ──── (power on) ────────────────► │  Shows welcome screen
  │                                    │
  │  ◄── TOUCH|action=mode;value=2 ── │  User picks "vs Stockfish"
  │                                    │
  │  ── MODE|value=2 ────────────────► │  Confirms mode, resets board
  │                                    │
  │  ◄── TOUCH|action=board;row=6;col=4  │  User taps e2
  │  ◄── TOUCH|action=board;row=4;col=4  │  User taps e4
  │                                    │
  │  ── STATE|fen=...;move=e2e4 ─────► │  Board updates, clock starts
  │                                    │
  │  ◄── TOUCH|action=hint;x=0;y=0 ── │  User requests hint
  │  ── HINT|move=e7e5 ──────────────► │  Shows hint
  │                                    │
  │  ◄── TOUCH|action=undo;x=0;y=0 ── │  User taps undo
  │  ── STATE|fen=<prev>;move=... ───► │  Board reverts
  │                                    │
  │  ◄── TOUCH|action=home;x=0;y=0 ── │  User goes home
  │                                    │  Shows welcome screen
```

---

## Notes

- The slave never validates moves. It renders whatever `fen` the master sends.
- The chess clock runs locally on the slave. The master drives it indirectly: each `STATE` message with a `move` parameter toggles the active side and adds the Fischer increment.
- The `x=0;y=0` parameters on hint/undo/home/new/resign/swap are placeholders (not used by the master).
- Board coordinates: row 0 / col 0 = top-left of the visual board = square a8.

---

## Independent WiFi & OTA (no serial protocol)

The ui_slave runs its **own** WiFi stack — completely independent of the master's networking. It exposes:

- A small embedded web page on `http://<slave-ip>/` for credentials + firmware OTA.
- An on-screen LVGL panel (`WiFi & Updates` button on the settings screen) with a touch keyboard for credentials and the same OTA controls.

This adds **no new serial messages**. The slave does its own GitHub release polling, downloads `ui_slave_firmware.bin` from the latest `dirkessl/OpenChessMate` release, and reboots into the new image. See `src/wifi_manager_ui.{h,cpp}`, `src/ota_updater_ui.{h,cpp}`, `src/wifi_ui.{h,cpp}`.
