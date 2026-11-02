# Mac tools

Build all of them with `./build.sh`. On first run macOS asks for Bluetooth access for Terminal.

## rd-app

The Mac connects to the derailleur the way the Wheeltop phone app does and shifts gears.
The native shifter stays paired. Keys: `u` up, `d` down, `q` quit.

```sh
./rd-app
```

## board-buttons

Two big on-screen buttons (and the ↑/↓ keys) that shift through the ESP32 board over USB.
Works in both board modes.

```sh
./board-buttons [/dev/cu.usbmodemXXX]
```

## wheeltop-shifter

The original experiment: the Mac pretends to be the shifter (pod). It also controls the ESP32 board and
records a real pod.

```sh
./wheeltop-shifter                 # Mac as an OX pod (3-byte frames)
./wheeltop-shifter --type tx-right # Mac as a TX pod
./wheeltop-shifter --esp32 [PORT]  # control the ESP32 board over USB
./wheeltop-shifter --listen        # connect to a real pod and record its frames
./wheeltop-shifter --scan          # show all BLE devices nearby
```

Keys: `↑`/`k`/`+` shift up, `↓`/`j`/`-` shift down, `t` toggles fine-tune, `q` quits.

Limitation: macOS doesn't let an app advertise manufacturer data, so the Mac is visible only with a name and
the NUS service UUID, without the pod prefix `07 0f 00 14 55 6a 84`. The derailleur doesn't connect to it,
which is why the ESP32 board exists.

## dongle_sniff.py

Reads the nRF52840 dongle sniffer. See [../SNIFFER.md](../SNIFFER.md).
