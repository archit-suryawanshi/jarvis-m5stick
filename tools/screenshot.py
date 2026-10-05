"""Grabs screenshots from the stick over USB serial.

    python3 tools/screenshot.py PORT [screen ...]

With screen names (home, listen, work, answer, error) it shows each sample
screen first; with none it captures whatever is on screen. PNGs land in
screenshots/ at 2x scale.
"""

import os
import sys
import time

import serial
from PIL import Image


def capture(ser, name, tries=3):
    if name != "current":
        ser.write(f"show {name}\n".encode())
        time.sleep(0.8)  # let animations settle
    for attempt in range(tries):
        try:
            return grab(ser, name)
        except ValueError as e:  # a firmware log line landed inside the image data
            print(f"{name}: retrying ({e})")
    raise SystemExit(f"{name}: capture kept failing")


def grab(ser, name):
    ser.reset_input_buffer()
    ser.write(b"shot\n")
    w = h = None
    data = bytearray()
    deadline = time.time() + 30
    while time.time() < deadline:
        line = ser.readline().decode(errors="ignore").strip()
        if line.startswith("SHOT "):
            w, h = map(int, line.split()[1:3])
        elif line == "END":
            break
        elif w and line:
            if all(c in "0123456789abcdef" for c in line) and len(line) % 2 == 0:
                data += bytes.fromhex(line)
            elif line[0] in "0123456789abcdef":
                raise ValueError(f"garbled line: {line[:40]}")
            else:
                print("  log:", line[:100])
    if not w or len(data) != w * h * 2:
        raise ValueError(f"incomplete capture ({len(data)} bytes)")

    img = Image.new("RGB", (w, h))
    px = img.load()
    for i in range(w * h):
        v = (data[2 * i] << 8) | data[2 * i + 1]  # sprite pixels are big-endian RGB565
        r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
        px[i % w, i // w] = (r * 255 // 31, g * 255 // 63, b * 255 // 31)
    os.makedirs("screenshots", exist_ok=True)
    path = f"screenshots/{name}.png"
    img.resize((w * 2, h * 2), Image.NEAREST).save(path)
    print(path)


def main():
    port = sys.argv[1]
    names = sys.argv[2:] or ["current"]
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 115200, 2
    ser.dtr = ser.rts = False
    ser.open()
    time.sleep(4)  # opening the port still resets the stick; let it boot
    for name in names:
        capture(ser, name)
    ser.close()


if __name__ == "__main__":
    main()
