# OBD raw logger: ESP32 → API

The ESP32 only captures raw CAN frames and uploads them. All decoding and logic lives on the server.

```
Car OBD port ── SN65HVD230 ── ESP32-C3 ──(Wi-Fi, HTTP JSON)──> FastAPI ──> SQLite (raw frames)
                                                                             │
                                                             decoders / analysis (next step)
```

Prototype: **ESP32-C3 mini**, HS-CAN only (engine, ABS, airbags; everything standard OBD-II uses).
Later: **ESP32-C6** with a second transceiver for Ford's MS-CAN. The data format already has a `bus` field for this.

## Parts

- ESP32-C3 mini board (C3 SuperMini or ESP32-C3-DevKitM-1)
- SN65HVD230 CAN transceiver module (3.3 V)
- 12 V → 5 V buck converter (automotive input range, e.g. MP1584 based)
- OBD-II male plug with pigtail wires
- 1 A inline fuse, 1N5819 (or similar) diode for reverse polarity, SMBJ18A TVS diode

## Wiring

| OBD pin | Signal          | Goes to                                  |
|--------:|-----------------|------------------------------------------|
| 16      | +12 V battery   | fuse → diode → buck IN+ (TVS across IN)  |
| 4, 5    | Ground          | buck IN−, C3 GND, SN65HVD230 GND         |
| 6       | HS-CAN High     | SN65HVD230 CANH                          |
| 14      | HS-CAN Low      | SN65HVD230 CANL                          |

| From            | To                 |
|-----------------|--------------------|
| Buck OUT 5 V    | C3 **5V** pin      |
| C3 **3V3**      | SN65HVD230 3V3     |
| C3 **GPIO5**    | SN65HVD230 CTX     |
| C3 **GPIO4**    | SN65HVD230 CRX     |

- **Remove the 120 Ω termination resistor on the SN65HVD230 module** (small SMD resistor, often marked "121"). The car's bus is already terminated. Leave it on for the bench self-test.
- **Set the buck converter to 5.0 V with a multimeter before connecting the C3.** Many modules ship set to a higher voltage and will destroy the board.
- On the C3, avoid GPIO 2, 8, 9 (read at boot) and 20, 21 (serial port).
- While the C3 is on USB at your desk, don't also power it from the buck converter.

## Server

```bash
cd server
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
OBD_API_KEY=change-me uvicorn app:app --host 0.0.0.0 --port 8000
```

Test without any hardware:

```bash
OBD_API_KEY=change-me python fake_device.py                # healthy catalyst
CAT_FAILING=1 OBD_API_KEY=change-me python fake_device.py  # worn catalyst pattern
```

Check data (API docs at http://127.0.0.1:8000/docs):

```bash
curl -H "X-API-Key: change-me" "http://127.0.0.1:8000/api/v1/stats"
curl -H "X-API-Key: change-me" "http://127.0.0.1:8000/api/v1/frames?can_id=0x7E8&limit=20"
```

## Firmware

1. Arduino IDE → Boards Manager → install **esp32 by Espressif** (3.x).
2. Open `firmware/obd_logger/obd_logger.ino`.
3. Board: **ESP32C3 Dev Module**. Tools → **USB CDC On Boot: Enabled** (otherwise Serial Monitor stays empty).
4. Edit the config block: Wi-Fi, `API_URL`, `API_KEY`, `RUN_MODE`.
5. Upload, open Serial Monitor at 115200.

If upload fails, hold **BOOT**, tap **RESET**, release BOOT, and upload again.
If it never connects to Wi-Fi, set `WIFI_LOW_TX_POWER = true` (a known fix for some C3 SuperMini boards).

## Bring-up plan (do these in order)

**Step 1: Server alone.** Run the server and `fake_device.py`. Frames appear in `/api/v1/frames`.

**Step 2: Bench self-test (no car).** `RUN_MODE = MODE_SELFTEST`. Connect only the C3 and the transceiver (termination resistor still on), powered by USB. The chip sends requests plus fake engine replies to itself. Serial Monitor should show `Uploaded N frames` and the server should show `0x7DF` requests and `0x7E8` replies. This proves the pins, CAN controller, Wi-Fi, and API path.

**Step 3: Sniff in the car.** Remove the termination resistor. `RUN_MODE = MODE_SNIFF`. Plug in, ignition on. The device never transmits in this mode. If the car exposes normal traffic on the OBD port, many different CAN IDs appear on the server. If nothing appears, that's still possible on some cars (the port may only carry diagnostic traffic), so move to step 4.

**Step 4: Poll in the car.** `RUN_MODE = MODE_POLL`, ignition on or engine running. You should see `0x7DF` requests and `0x7E8` replies. Then drive and log.

## Modes

- **`MODE_SELFTEST`**: bench test. No ACK needed, frames loop back inside the chip, fake ECU replies.
- **`MODE_SNIFF`**: listen-only. The ESP32 never transmits.
- **`MODE_POLL`**: sends standard OBD-II requests to `0x7DF` and records replies from `0x7E8`–`0x7EF`. Edit the `REQUESTS` table to change what is asked for. Multi-frame replies (DTC lists, mode 06) get an automatic ISO-TP flow-control frame.

The C3 is single-core, so in `MODE_SNIFF` on a busy bus it may drop frames. The `dropped` count in `/api/v1/stats` shows this. Polling load is small and is not affected.

## Networking for the first test

Turn on your phone hotspot, connect both the C3 and your laptop to it, run the server on the laptop, and set `API_URL` to the laptop's IP on that hotspot (check with `ipconfig` / `ip addr`). Allow port 8000 through the laptop's firewall.

## Data format (device → server)

```json
{
  "device_id": "ecosport-2014-c3",
  "boot_id": "a1b2c3d4",
  "sent_us": 12345678,
  "dropped": 0,
  "frames": [
    {"t": 12000000, "bus": 0, "id": 2015, "ext": 0, "dir": 1, "data": "0201150000000000"},
    {"t": 12004000, "bus": 0, "id": 2024, "ext": 0, "dir": 0, "data": "0441155AFF000000"}
  ]
}
```

- `t` / `sent_us`: device uptime in microseconds. The server estimates wall time as `received_at - (sent_us - t) / 1e6`. Relative timing between frames is exact.
- `bus`: 0 = HS-CAN, 1 = MS-CAN (ESP32-C6 later). Optional, defaults to 0.
- `boot_id`: changes on every power-up, so uptime resets don't get mixed together.
- `dir`: 1 = request we sent, 0 = received.
- `dropped`: cumulative frames lost on the device because the buffer was full.

## Safety

- Only read requests are sent (modes 01, 03, 06, 07). Don't add write, routine-control, or programming services.
- Start in `MODE_SNIFF` in the car until you trust the wiring.
- Unplug when parked. Polling drains the battery and can keep modules awake.
- Mount it so the plug and wires can't interfere with the pedals.