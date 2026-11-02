# Buttons and power for the ESP32-C3 SuperMini

The board works on its own, without a computer: two buttons shift gears and a battery powers it.
A computer is only needed to flash the firmware.

**Verified:** on external power without a computer, the buttons shift (2026-11-02, pod mode). In app mode the
board's buttons and the native shifter shift in any order, and the shifter stays paired (2026-11-03).

## Parts

| Part | Qty | Why |
|---|---|---|
| Momentary push button (normally open), e.g. a 6×6 mm tactile switch or a waterproof handlebar button | 2 | up and down |
| Slide switch (SPDT) | 1, optional | R/T position like the pod's slider (pod mode only) |
| 3.7 V LiPo battery, 300–1000 mAh, **with a protection circuit** | 1 | power |
| TP4056 charger module **with protection** (6 pins: IN, B+, B−, OUT+, OUT−) | 1 | charge the battery from USB |
| Schottky diode 1N5817 or SS14 | 1 | keeps USB power out of the battery |
| Power switch (any small one) | 1 | turn the board on and off |
| Wires, heat shrink | | |

## Buttons

Each button goes between a board pin and `GND`. No resistors are needed: the firmware enables the pull-ups.

```
          ESP32-C3 SuperMini
         ┌───────────────────┐
  UP    ─┤ 4             GND ├─┬───── common wire of all buttons
  DOWN  ─┤ 5                 │ │
  R/T   ─┤ 6                 │ │
         └───────────────────┘ │
                               │
  UP button:   pin 4 ──[ button ]── GND
  DOWN button: pin 5 ──[ button ]── GND
  switch:      pin 6 ──[ closed = T, open = R ]── GND
```

- The pins are labelled `4`, `5`, `6` on the board. The SuperMini has `GND` on both sides; either works.
- The R/T switch is optional. Without it the board is always in R (normal shifting).
- Test without soldering: touch a wire from pin `4` to `GND`. That shifts up.

## Battery power

```
  LiPo +  ──── B+   TP4056   OUT+ ──[ switch ]──▶|── 5V   SuperMini
  LiPo −  ──── B−            OUT− ─────────────────── GND
                 (module USB-C: charging)       diode: stripe towards 5V
```

1. Battery to `B+` and `B−` of the TP4056 module.
2. `OUT+` through the switch to the diode's **anode**; the **cathode** (striped side) to the board's `5V` pin.
3. `OUT−` to the board's `GND`.

Why:

- **The `5V` pin, not `3V3`.** Power on `5V` goes through the board's regulator. The battery gives up to 4.2 V,
  and `3V3` must not see more than 3.6 V or the chip dies.
- **The diode.** On the SuperMini the `5V` pin is wired straight to USB. Without the diode, plugging the board
  into a computer with the battery switched on would push 5 V from USB into the charger output and the battery.
- **Charge** through the TP4056 module's USB port, not the board's.
- A module **with protection** cuts off the battery when it is empty and keeps it from being damaged.

Simpler, without soldering the power: plug a power bank into the board's USB-C. The catch: many power banks
switch off when the current is below ~50 mA, and the board draws less. You need one with a low-current mode.

## Getting started

1. Flash the firmware ([README.md](README.md)).
2. Power the board. The blue LED blinks fast: the board is pairing.
3. Wake the derailleur by moving the bike, keep the board close to it. After a few seconds the LED stays on.
4. Press the buttons.

The red LED only shows power. The blue LED is described in [README.md](README.md#pairing-app-mode).

## Battery life

Not measured. By the datasheet an ESP32-C3 with Bluetooth active draws around 20–40 mA, plus the red LED.
Rough guide: 12–20 hours from a 500 mAh battery. Switch the board off when you're not riding.
Removing the red LED extends it a little.

## Pins to avoid

`8` and `9` take part in booting (`9` is the BOOT button), `20` and `21` are the debug UART, and `18` and `19`
are USB. If pins `4`–`6` are needed for something else, `0`, `1`, `3`, `7`, `10` work: change `PIN_UP`,
`PIN_DOWN`, `PIN_FINE` at the top of `wheeltop_pod.ino`.

## Pod mode

In pod mode the board replaces the native shifter. The derailleur remembers only one shifter: whichever was
paired last works, the other is ignored.

1. Switch the board to shifter mode in the Android app, or send `mode pod` over USB. The board advertises as a
   TX Right shifter with its own address (made from the chip's MAC). Never give the board the native shifter's
   address with `mac` while the shifter has its battery in.
2. Pair the board: plug power into the derailleur briefly to start pairing; after a couple of seconds
   (the derailleur has connected to the board) press a board button or "Press shifter button" in the app.
   You have about 8 seconds.
3. Back to the native shifter: plug power into the derailleur and press a button on the shifter.

The derailleur doesn't accept the TX Left type (`0x36`) when pairing.
