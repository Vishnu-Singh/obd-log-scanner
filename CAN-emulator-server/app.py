"""Pretends to be the ESP32 so you can test the API without the car.

Sends the same JSON format as the firmware, with realistic OBD request/response pairs.
Set CAT_FAILING=1 to make the downstream O2 sensor swing like a worn catalyst.

Run:  OBD_API_KEY=change-me python fake_device.py
"""

import json
import math
import os
import random
import secrets
import time
import urllib.request

URL = os.environ.get("OBD_API_URL", "http://127.0.0.1:8000/api/v1/frames")
KEY = os.environ.get("OBD_API_KEY", "change-me")
CAT_FAILING = os.environ.get("CAT_FAILING") == "1"

BOOT_ID = secrets.token_hex(4)
START = time.monotonic()


def now_us() -> int:
    return int((time.monotonic() - START) * 1_000_000)


def pad8(values) -> str:
    return (bytes(values) + bytes(8))[:8].hex().upper()


def fake_pid_value(pid: int, t: float) -> list[int]:
    rpm = 1800 + 700 * math.sin(t / 5)
    if pid == 0x0C:                      # RPM = (256A + B) / 4
        raw = int(rpm * 4)
        return [raw >> 8, raw & 0xFF]
    if pid == 0x0D:                      # speed km/h
        return [int(40 + 20 * math.sin(t / 7))]
    if pid == 0x05:                      # coolant = A - 40
        return [90 + 40]
    if pid == 0x15:                      # O2S2 voltage = A / 200, B = trim (0xFF = unused)
        volts = 0.45 + 0.4 * math.sin(t * 3) if CAT_FAILING else 0.68 + random.uniform(-0.02, 0.02)
        return [int(volts * 200), 0xFF]
    return [0]


def exchange(pid: int) -> list[dict]:
    t_req = now_us()
    value = fake_pid_value(pid, time.monotonic() - START)
    return [
        {"t": t_req, "id": 0x7DF, "ext": 0, "dir": 1, "data": pad8([0x02, 0x01, pid])},
        {"t": t_req + 4000, "id": 0x7E8, "ext": 0, "dir": 0,
         "data": pad8([2 + len(value), 0x41, pid, *value])},
    ]


def send(frames: list[dict]) -> dict:
    body = json.dumps({
        "device_id": "fake-ecosport",
        "boot_id": BOOT_ID,
        "sent_us": now_us(),
        "dropped": 0,
        "frames": frames,
    }).encode()
    req = urllib.request.Request(URL, data=body, method="POST", headers={
        "Content-Type": "application/json", "X-API-Key": KEY,
    })
    with urllib.request.urlopen(req, timeout=5) as resp:
        return json.load(resp)


def main(batches: int = 0) -> None:
    n = 0
    while batches == 0 or n < batches:
        frames = []
        for pid in (0x0C, 0x0D, 0x05, 0x15):
            frames += exchange(pid)
        print(send(frames))
        n += 1
        time.sleep(1)


if __name__ == "__main__":
    main(int(os.environ.get("BATCHES", "0")))