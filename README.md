# wheeltop

Shift a WHEELTOP EDS electronic rear derailleur with your own buttons: an ESP32 board connects to the derailleur over Bluetooth LE the way the Wheeltop phone app does, so the stock shifter keeps working too.

An unofficial project, not affiliated with or endorsed by WHEELTOP. See [Disclaimer](#disclaimer).

## Why

I was on a bikepacking trip in Karelia, in the north-west of Russia, with a WHEELTOP EDS OX 2.0 rear derailleur. The derailleur worked flawlessly for the whole 900 km route, but the shifter didn't: after a few days of constant heavy rain, water got inside it and it stopped working, so I could no longer change gear. The backup plan, shifting from the Wheeltop phone app, turned out to need an internet connection, and there was none that far from civilisation.

So this project: a backup shifter you can build yourself and an app that shifts from the phone with no internet.

## Quick start

1. Get an ESP32-C3 SuperMini (any ESP32 with Bluetooth LE works) and two push buttons.
   Wire the buttons between pins 4/5 and GND: [esp32/WIRING.md](esp32/WIRING.md).
2. Flash `esp32/wheeltop_pod`: [esp32/README.md](esp32/README.md).
3. Power the board, wake the derailleur by moving the bike and keep the board close to it.
   The board pairs with the nearest derailleur and remembers it; the blue LED stays on when connected.
4. Press the buttons to shift. The stock shifter still works.

The Android app in [android/](android/) shifts gears straight from the phone (no board needed), and pairs the board
and switches its mode.

## Contents

| Path | What |
|---|---|
| [wheeltop_eds_protocol_spec.md](wheeltop_eds_protocol_spec.md) | the protocols: shifter → derailleur and app → derailleur |
| [esp32/](esp32/) | board firmware (Arduino, NimBLE-Arduino) and wiring |
| [android/](android/) | Android app: shift from the phone, pair the board, choose its mode |
| [mac/](mac/) | macOS tools: the Mac as the app (`rd-app`), on-screen buttons for the board, the original pod emulator |
| [dongle/](dongle/), [SNIFFER.md](SNIFFER.md) | BLE sniffer firmware for the nRF52840 Dongle that follows CSA #2 connections |

Tested with an EDS OX2 rear derailleur and a TX Right shifter.

## Disclaimer

This is an independent, unofficial project. It is not affiliated with, endorsed by or supported by WHEELTOP.
"WHEELTOP", "EDS" and the product names are used only to say which devices this works with; they belong to
their owners.

The protocol was worked out for interoperability by recording the Bluetooth traffic between devices the author
owns. The repository contains no code, firmware or other material from WHEELTOP.

Use it at your own risk. A derailleur that shifts unexpectedly can cause a crash, so test on a stand before
riding. Using this with your derailleur may affect its warranty: WHEELTOP's warranty excludes products that
have been modified or misused.

## License

MIT, see [LICENSE](LICENSE).
