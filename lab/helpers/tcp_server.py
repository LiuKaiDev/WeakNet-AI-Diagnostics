#!/usr/bin/env python3
"""Small local TCP echo workload server for an isolated lab namespace."""

from __future__ import annotations

import argparse
from pathlib import Path
import signal
import socket


running = True


def stop(*_: object) -> None:
    global running
    running = False


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--ready-file", required=True, type=Path)
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.bind, args.port))
        server.listen(4)
        server.settimeout(0.5)
        args.ready_file.write_text("ready\n", encoding="utf-8")
        while running:
            try:
                connection, _ = server.accept()
            except socket.timeout:
                continue
            with connection:
                connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                connection.settimeout(0.5)
                while running:
                    try:
                        data = connection.recv(65536)
                    except socket.timeout:
                        continue
                    except OSError:
                        break
                    if not data:
                        break
                    try:
                        connection.sendall(data)
                    except OSError:
                        break
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
