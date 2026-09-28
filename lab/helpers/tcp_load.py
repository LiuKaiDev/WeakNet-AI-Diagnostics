#!/usr/bin/env python3
"""Persistent TCP echo traffic generator for a WeakNet Lab namespace."""

from __future__ import annotations

import argparse
import signal
import socket
import time


running = True


def stop(*_: object) -> None:
    global running
    running = False


def exchange(host: str, port: int, payload: bytes) -> None:
    with socket.create_connection((host, port), timeout=2) as connection:
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        connection.settimeout(3)
        while running:
            connection.sendall(payload)
            received = 0
            while received < len(payload) and running:
                block = connection.recv(min(65536, len(payload) - received))
                if not block:
                    raise ConnectionError("server closed connection")
                received += len(block)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--payload-bytes", type=int, default=65536)
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    payload = b"w" * max(4096, min(args.payload_bytes, 1024 * 1024))
    while running:
        try:
            exchange(args.host, args.port, payload)
        except (ConnectionError, OSError, socket.timeout):
            if running:
                time.sleep(0.2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
