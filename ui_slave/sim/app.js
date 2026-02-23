const WS_URL = 'ws://localhost:8765';
let ws = null;
const boardEl = document.getElementById('board');
const lastMoveEl = document.getElementById('last-move');
const logEl = document.getElementById('log');
let highlighted = [];

const glyphs = {
    'K': '♔', 'Q': '♕', 'R': '♖', 'B': '♗', 'N': '♘', 'P': '♙',
    'k': '♚', 'q': '♛', 'r': '♜', 'b': '♝', 'n': '♞', 'p': '♟'
};

function log(s) {
    const d = document.createElement('div'); d.textContent = s; logEl.appendChild(d); logEl.scrollTop = logEl.scrollHeight;
}

function buildBoard() {
    boardEl.innerHTML = '';
    for (let r = 0; r < 8; r++) {
        for (let c = 0; c < 8; c++) {
            const cell = document.createElement('div');
            cell.className = 'cell ' + (((r + c) % 2 === 0) ? 'light' : 'dark');
            cell.dataset.row = r; cell.dataset.col = c;
            cell.addEventListener('click', () => onCellClick(r, c, cell));
            boardEl.appendChild(cell);
        }
    }
}

function onCellClick(r, c, el) {
    send(`TOUCH|action=board;row=${r};col=${c}\n`);
    // simple local flash
    el.classList.add('highlight');
    setTimeout(() => el.classList.remove('highlight'), 300);
}

function setLastMove(s) { lastMoveEl.textContent = 'Last move: ' + s; }

function clearHighlights() { highlighted.forEach(x => x.classList.remove('highlight')); highlighted = []; }

function highlightCells(fr, fc, tr, tc) {
    clearHighlights();
    const idx = (r, c) => r * 8 + c;
    const cells = boardEl.querySelectorAll('.cell');
    const a = cells[fr * 8 + fc]; const b = cells[tr * 8 + tc];
    if (a) { a.classList.add('highlight'); highlighted.push(a); }
    if (b && b !== a) { b.classList.add('highlight'); highlighted.push(b); }
    setTimeout(clearHighlights, 3000);
}

function renderFen(fen) {
    const parts = fen.split(' ');
    const rows = parts[0].split('/');
    const cells = boardEl.querySelectorAll('.cell');
    cells.forEach(c => c.textContent = '');
    for (let r = 0; r < 8; r++) {
        const rank = rows[r];
        let c = 0;
        for (let i = 0; i < rank.length && c < 8; i++) {
            const ch = rank[i];
            if (ch >= '1' && ch <= '8') {
                const empties = parseInt(ch, 10);
                for (let e = 0; e < empties; e++) c++;
            } else {
                const glyph = glyphs[ch] || ch;
                const cell = cells[r * 8 + c];
                cell.textContent = glyph;
                c++;
            }
        }
    }
}

function handleMessage(msg) {
    log('RX: ' + msg);
    const sep = msg.indexOf('|');
    const type = (sep >= 0) ? msg.substring(0, sep) : msg;
    const payload = (sep >= 0) ? msg.substring(sep + 1) : '';
    if (type === 'STATE') {
        const idx = payload.indexOf('fen=');
        if (idx >= 0) {
            const fen = payload.substring(idx + 4).split(';')[0].trim();
            setLastMove('FEN: ' + fen);
            renderFen(fen);
        } else {
            setLastMove('Board updated');
        }
    } else if (type === 'HINT') {
        const idx = payload.indexOf('move=');
        if (idx >= 0) {
            const move = payload.substring(idx + 5).trim();
            setLastMove(move);
            // parse uci like e2e4
            if (move.length >= 4) {
                const fc = move.charCodeAt(0) - 97;
                const fr = 8 - (move.charCodeAt(1) - 48);
                const tc = move.charCodeAt(2) - 97;
                const tr = 8 - (move.charCodeAt(3) - 48);
                highlightCells(fr, fc, tr, tc);
            }
        }
    } else if (type === 'ERROR') {
        setLastMove('Hint error');
    }
}

function send(s) {
    if (ws && ws.readyState === WebSocket.OPEN) { ws.send(s); log('TX: ' + s.trim()); }
    else log('TX failed, socket not open: ' + s.trim());
}

function setupWs() {
    ws = new WebSocket(WS_URL);
    ws.addEventListener('open', () => { log('WS open'); });
    ws.addEventListener('message', e => { handleMessage(e.data); });
    ws.addEventListener('close', () => { log('WS closed - retry in 2s'); setTimeout(setupWs, 2000); });
    ws.addEventListener('error', ev => { log('WS error'); });
}

// buttons
document.getElementById('btn-hint').addEventListener('click', () => send('TOUCH|action=hint;x=0;y=0\n'));
document.getElementById('btn-undo').addEventListener('click', () => send('TOUCH|action=undo;x=0;y=0\n'));
document.getElementById('btn-menu').addEventListener('click', () => send('TOUCH|action=menu;x=0;y=0\n'));

buildBoard();
setupWs();
