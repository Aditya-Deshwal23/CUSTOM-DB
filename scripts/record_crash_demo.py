#!/usr/bin/env python3
"""Record the real Docker crash demo as an asciinema v2 cast."""

import argparse
import errno
import fcntl
import json
import os
import pty
import select
import struct
import subprocess
import sys
import termios
import time
import tty
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def record(output, image, volume, host_port):
    output.parent.mkdir(parents=True, exist_ok=True)
    environment = os.environ.copy()
    environment.update(
        {"IMAGE": image, "VOLUME": volume, "HOST_PORT": str(host_port), "TERM": "xterm-256color"}
    )
    master, slave = pty.openpty()
    tty.setraw(slave)
    fcntl_window_size = struct.pack("HHHH", 36, 120, 0, 0)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, fcntl_window_size)
    process = subprocess.Popen(
        ["bash", "scripts/demo_crash.sh"],
        cwd=ROOT,
        env=environment,
        stdin=slave,
        stdout=slave,
        stderr=slave,
        close_fds=True,
    )
    os.close(slave)

    started = time.monotonic()
    events = []
    try:
        while True:
            ready, _, _ = select.select([master], [], [], 0.1)
            if ready:
                try:
                    data = os.read(master, 65536)
                except OSError as error:
                    if error.errno == errno.EIO:
                        break
                    raise
                if not data:
                    break
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
                events.append(
                    [round(time.monotonic() - started, 6), "o", data.decode(errors="replace")]
                )
            elif process.poll() is not None:
                break
        exit_code = process.wait()
    finally:
        os.close(master)
        if process.poll() is None:
            process.terminate()
            process.wait()

    header = {
        "version": 2,
        "width": 120,
        "height": 36,
        "timestamp": int(time.time()),
        "env": {"SHELL": "/bin/bash", "TERM": "xterm-256color"},
    }
    with output.open("w", encoding="utf-8") as cast:
        cast.write(json.dumps(header, separators=(",", ":")) + "\n")
        for event in events:
            cast.write(json.dumps(event, ensure_ascii=False, separators=(",", ":")) + "\n")
    if exit_code != 0:
        raise subprocess.CalledProcessError(exit_code, "scripts/demo_crash.sh")
    print(f"\nRecorded actual demo output to {output.relative_to(ROOT)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "docs/assets/recovery.cast")
    parser.add_argument("--image", default="custom-db:local")
    parser.add_argument("--volume", default="custom-db-recorded-proof")
    parser.add_argument("--host-port", type=int, default=18082)
    args = parser.parse_args()
    record(args.output, args.image, args.volume, args.host_port)


if __name__ == "__main__":
    main()
