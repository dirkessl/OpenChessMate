#!/usr/bin/env python3
"""
Simple WebSocket master emulator for the UI simulator.
Run: pip install -r requirements.txt
Start: python master_emulator.py
It sends an initial STATE with starting FEN and echoes TOUCH messages.
Type commands in the server console to send messages to the UI:
  state <fen>        -> send STATE|fen=<fen>
  hint <uci>         -> send HINT|move=<uci>
  raw <TEXT>         -> send raw text to client
"""
import asyncio
import websockets
import sys

PORT = 8765
clients = set()

async def handler(ws, path=None):
    print('Client connected')
    clients.add(ws)
    try:
        # send initial board
        start = 'STATE|fen=rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1\n'
        await ws.send(start)
        async for msg in ws:
            print('RX from UI:', msg.strip())
    except websockets.ConnectionClosed:
        pass
    finally:
        clients.remove(ws)
        print('Client disconnected')

async def stdin_loop():
    loop = asyncio.get_running_loop()
    reader = asyncio.StreamReader()
    protocol = asyncio.StreamReaderProtocol(reader)
    await loop.connect_read_pipe(lambda: protocol, sys.stdin)
    while True:
        line = (await reader.readline()).decode().strip()
        if not line:
            await asyncio.sleep(0.1); continue
        if line.startswith('state '):
            fen = line[len('state '):]
            payload = f'STATE|fen={fen}\n'
        elif line.startswith('hint '):
            uci = line[len('hint '):]
            payload = f'HINT|move={uci}\n'
        elif line.startswith('raw '):
            payload = line[len('raw '):] + '\n'
        else:
            print('commands: state <fen> | hint <uci> | raw <text>')
            continue
        if clients:
            await asyncio.wait([c.send(payload) for c in clients])
            print('Sent:', payload.strip())
        else:
            print('No clients connected')

async def main():
    server = await websockets.serve(handler, '0.0.0.0', PORT)
    print(f'Listening on ws://localhost:{PORT}')
    await stdin_loop()

if __name__=='__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print('Bye')
