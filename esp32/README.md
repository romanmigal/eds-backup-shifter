# wheeltop_pod: ESP32 firmware

Firmware for an ESP32 board with two buttons that shifts a WHEELTOP EDS rear derailleur (RD) over Bluetooth LE.

Any board with Bluetooth LE works: ESP32, ESP32-C3, ESP32-S3 (not ESP32-S2, it has no Bluetooth).
For a standalone board with buttons and a battery, see [WIRING.md](WIRING.md) (ESP32-C3 SuperMini).

## Two modes

- **app** (default, verified 2026-11-03): the board connects to the derailleur the way the Wheeltop phone app
  does. The native shifter stays paired and keeps working, so both can be used at the same time.
  A press shifts once; a held button shifts again every 0.3 s after 0.54 s. There is no fine-tune in this mode.
- **pod** (`mode pod`): the board pretends to be the shifter (pod). The derailleur has to be paired with the board
  instead of the native shifter. Kept for protocol research; see [WIRING.md](WIRING.md#pod-mode).

## Pairing (app mode)

On first start the board has no derailleur and pairs by itself (verified 2026-11-03):

1. Power the board; the blue LED blinks fast.
2. Wake the derailleur by moving the bike, and keep the board close to it.
3. The board waits 3 s, picks the strongest derailleur it heard, connects and remembers it. The LED stays on.

To pair with another derailleur, hold both buttons for 5 s or send `pair`. If several bikes are nearby,
keep the board right next to the derailleur you want while pairing.

| Blue LED | Meaning |
|---|---|
| fast blinking | pairing |
| short flash once a second | looking for the paired derailleur (move the bike to wake it) |
| steady | connected, the buttons shift |

## Flashing

```sh
arduino-cli lib install NimBLE-Arduino
arduino-cli compile --fqbn esp32:esp32:esp32 wheeltop_pod
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/cu.usbserial-XXXX wheeltop_pod
```

Use `--fqbn esp32:esp32:esp32c3` or `esp32:esp32:esp32s3` for C3 and S3 boards.

### ESP32-C3 (tested on this board)

`CDCOnBoot=cdc` is needed, otherwise the board is silent on USB. The normal upload fails with
"Failed to start stub flasher", so flash without the stub:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc --output-dir build wheeltop_pod
ESPTOOL=$(ls -d ~/Library/Arduino15/packages/esp32/tools/esptool_py/*/ | tail -1)esptool
$ESPTOOL --chip esp32c3 --port /dev/cu.usbmodem101 --no-stub write-flash 0x0 build/wheeltop_pod.ino.merged.bin
```

The full image (`0x0`, merged) erases the settings, including the paired derailleur. To update without losing
them, flash only the application: `write-flash 0x10000 build/wheeltop_pod.ino.bin`.

## Board commands

Type them in any serial monitor at 115200 baud, or after `:` in `../mac/wheeltop-shifter --esp32`.

| Command | What it does |
|---|---|
| `u` / `d` | shift up / down |
| `pair` | forget the derailleur and pair with the nearest one (app mode) |
| `mode app\|pod` | switch mode (restarts) |
| `info` | current settings |
| `log` | events since start-up |
| `reset` | restore defaults (restarts) |
| `f` | fine-tune on/off (pod mode) |
| `type ox\|tx-front\|tx-left\|tx-right` | pod type (pod mode) |
| `mac AA:BB:CC:DD:EE:FF [public\|random]`, `mac off` | own BLE address (pod mode) |
| `raw 04 02 06` | send any frame (pod mode) |
| `s` / `a` | scan 5 s: WHEELTOP devices / all devices |
| `clone N` | copy the address and advertisement of device N from the scan |
| `gatt N` | print the GATT table of device N from the scan |
| `layout` | print the handles of the board's own GATT |

Settings are kept in the board's flash and survive restarts.
