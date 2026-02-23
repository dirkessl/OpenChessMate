OpenChess UI Slave Simulator

This provides a quick browser-based simulator for the `ui_slave` touchscreen UI and a small Python master-emulator that speaks the same text protocol.

Requirements
- Python 3.8+
- Install Python deps:

```bash
python3 -m pip install -r requirements.txt
```

Run the master emulator (in this folder):

```bash
python3 master_emulator.py
```

Serve the UI files and open in a browser (recommended to avoid CORS/file issues):

```bash
python3 -m http.server 8000
# then open http://localhost:8000 in your browser
```

Usage
- The web UI connects to ws://localhost:8765. The emulator will send an initial `STATE|fen=...` message.
- Click cells or the buttons to send `TOUCH|...` messages; the emulator prints received messages.
- From the emulator terminal you can send messages to the UI:
  - `state <fen>` — update board
  - `hint <uci>`  — show a hint (e.g. `hint e2e4`)
  - `raw <text>`  — send arbitrary text

Files created
- `index.html`, `app.js`, `styles.css` — browser UI
- `master_emulator.py` — WebSocket server
- `requirements.txt` — Python deps

Let me know if you want the simulator to load piece images from the `data/pieces` folder or to more closely emulate LVGL styling.
