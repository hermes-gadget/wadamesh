#!/usr/bin/env python3
"""Host client for the wadamesh remote-test controller.

Drives a T-Deck (or any board built with -D WADAMESH_REMOTE_TEST=1) over the
USB serial console: navigation, taps, typed text, status, and screenshot
capture in the upstream <<<WMSHOT ...>>> BMP framing.

Works over a local serial port or any pyserial URL, e.g. a ser2net bridge:

    ./rt.py -p /dev/ttyACM0 status
    ./rt.py -p socket://hermes-portable:2000 nav home
    ./rt.py -p socket://hermes-portable:2000 capture home -o shots/
    ./rt.py -p /dev/ttyACM0 -i          # interactive

Screenshots decode to PNG when Pillow is available, else to binary PPM (P6).
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import time

try:
    import serial
except ImportError:  # pragma: no cover - environment guidance
    serial = None

WMSHOT_RE = re.compile(rb"<<<WMSHOT name=(\S+) w=(\d+) h=(\d+) bytes=(\d+)>>>")
BMP_HDR_BYTES = 66


class Link:
    def __init__(self, port: str, baud: int = 115200, timeout: float = 2.0):
        if serial is None:
            raise RuntimeError("pyserial not found: pip install pyserial")
        self.ser = serial.serial_for_url(port, baudrate=baud, timeout=timeout,
                                         write_timeout=5.0)
        self.buf = bytearray()

    def close(self) -> None:
        try:
            self.ser.close()
        except Exception:
            pass

    # ---- raw -------------------------------------------------------------
    def _fill(self, timeout: float) -> bool:
        deadline = time.time() + timeout
        while time.time() < deadline:
            chunk = self.ser.read(4096)
            if chunk:
                self.buf += chunk
                return True
            time.sleep(0.01)
        return False

    def _read_until(self, marker: bytes, timeout: float) -> bytes:
        deadline = time.time() + timeout
        while marker not in self.buf:
            if time.time() > deadline:
                raise TimeoutError(f"timed out waiting for {marker!r}")
            if not self._fill(min(0.5, max(0.01, deadline - time.time()))):
                continue
        idx = self.buf.index(marker) + len(marker)
        out = bytes(self.buf[:idx])
        del self.buf[:idx]
        return out

    def _read_exact(self, n: int, timeout: float) -> bytes:
        deadline = time.time() + timeout
        while len(self.buf) < n:
            if time.time() > deadline:
                raise TimeoutError(f"timed out reading {n} bytes ({len(self.buf)})")
            self._fill(min(0.5, max(0.01, deadline - time.time())))
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out

    # ---- commands --------------------------------------------------------
    def send(self, command: str, wait: float = 1.0, quiet: bool = False) -> list[str]:
        self.ser.write((command.rstrip("\r\n") + "\n").encode())
        self.ser.flush()
        lines: list[str] = []
        deadline = time.time() + wait
        while time.time() < deadline:
            if not self._fill(min(0.25, max(0.01, deadline - time.time()))):
                continue
            while b"\n" in self.buf:
                raw, _, rest = self.buf.partition(b"\n")
                self.buf = bytearray(rest)
                line = raw.decode("utf-8", "replace").strip("\r")
                if line.startswith("[rt]"):
                    lines.append(line[4:].strip())
                    print(line) if not quiet else None
        return lines

    def capture(self, name: str, outdir: str, timeout: float = 20.0) -> str:
        self.send(f"capture {name}", wait=0.3, quiet=True)
        head = self._read_until(b">>>\n", timeout)
        m = WMSHOT_RE.search(head)
        if not m:
            raise RuntimeError(f"no WMSHOT marker in {head[:200]!r}")
        shot, w, h, total = m.group(1).decode(), int(m.group(2)), int(m.group(3)), int(m.group(4))
        frame = self._read_exact(total, timeout)
        self._read_until(b"<<<WMEND>>>", timeout)
        path = _save_frame(frame, shot or name, w, h, outdir)
        # Drain the trailing "[rt] capture end" line.
        self._drain(0.4)
        return path

    def _drain(self, seconds: float) -> None:
        end = time.time() + seconds
        while time.time() < end:
            if not self._fill(0.1):
                continue
            self.buf.clear()


def _save_frame(frame: bytes, name: str, w: int, h: int, outdir: str) -> str:
    """Convert the 16-bit BI_BITFIELDS BMP payload to PNG (or PPM)."""
    if frame[:2] != b"BM":
        raise RuntimeError("unexpected capture payload (not BMP)")
    data_offset = int.from_bytes(frame[10:14], "little")
    width = int.from_bytes(frame[18:22], "little")
    height = int.from_bytes(frame[22:26], "little")
    bpp = int.from_bytes(frame[28:30], "little")
    if bpp != 16:
        raise RuntimeError(f"expected 16-bpp capture, got {bpp}")
    row_bytes = ((width * 16 + 31) // 32) * 4
    pixels = frame[data_offset:data_offset + row_bytes * height]
    if len(pixels) < row_bytes * height:
        raise RuntimeError("short capture payload")

    rgb = bytearray(width * height * 3)
    for row in range(height):
        src_y = height - 1 - row            # BMP rows are bottom-up
        base = src_y * row_bytes
        dst = row * width * 3
        for x in range(width):
            v = pixels[base + x * 2] | (pixels[base + x * 2 + 1] << 8)
            r = ((v >> 11) & 0x1F) * 255 // 31
            g = ((v >> 5) & 0x3F) * 255 // 63
            b = (v & 0x1F) * 255 // 31
            rgb[dst + x * 3] = r
            rgb[dst + x * 3 + 1] = g
            rgb[dst + x * 3 + 2] = b

    os.makedirs(outdir, exist_ok=True)
    try:
        from PIL import Image
        path = os.path.join(outdir, f"{name}.png")
        Image.frombytes("RGB", (width, height), bytes(rgb)).save(path)
        return path
    except ImportError:
        path = os.path.join(outdir, f"{name}.ppm")
        with open(path, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (width, height))
            f.write(rgb)
        return path


def main() -> int:
    ap = argparse.ArgumentParser(description="wadamesh remote-test client")
    ap.add_argument("-p", "--port", required=True,
                    help="serial port or pyserial URL (socket://host:port)")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-w", "--wait", type=float, default=1.0,
                    help="seconds to collect command output (default 1.0)")
    ap.add_argument("-o", "--outdir", default="shots", help="screenshot directory")
    ap.add_argument("-i", "--interactive", action="store_true")
    ap.add_argument("command", nargs=argparse.REMAINDER,
                    help="controller command + args (e.g. 'nav home')")
    args = ap.parse_args()

    link = Link(args.port, args.baud)
    try:
        if args.interactive or not args.command:
            print("wadamesh remote console — 'help' for commands, Ctrl-D to quit")
            while True:
                try:
                    line = input("rt> ").strip()
                except EOFError:
                    break
                if not line:
                    continue
                if line.split()[0] in ("capture", "snapshot"):
                    parts = line.split()
                    name = parts[1] if len(parts) > 1 else "rt"
                    print("saved:", link.capture(name, args.outdir))
                else:
                    link.send(line, wait=args.wait)
            return 0

        cmd = " ".join(args.command).strip()
        if not cmd:
            ap.error("no command given")
        if cmd.split()[0] in ("capture", "snapshot"):
            parts = cmd.split()
            name = parts[1] if len(parts) > 1 else "rt"
            print("saved:", link.capture(name, args.outdir))
        else:
            lines = link.send(cmd, wait=args.wait, quiet=False)
            return 0 if lines else 2
        return 0
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
