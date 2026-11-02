# WT Board for Android

A small app that shifts a WHEELTOP EDS rear derailleur from the phone and configures the board
(`esp32/wheeltop_pod`) over Bluetooth. Verified 2026-11-03.

- **Up / Down** shift gears. The phone connects to the derailleur directly, the way the Wheeltop app does
  (spec section 19), so this works with or without the board. Holding a button keeps shifting.
  The current gear comes from the derailleur's reports. The board's buttons keep working while the app is open:
  the board passes their presses to the phone.
- **Board mode**: like the phone app (default; your shifter keeps working) or like a shifter with the board's
  own address. The board remembers the mode.
- **Pairing the board**: in phone mode the board pairs with the nearest derailleur; in shifter mode the dialog has
  a button that acts as a shifter button press during the derailleur's pairing window.

The phone connects to the derailleur the board is paired with (it learns the address from the board), or else to
the first `EDS OX…` it finds, and remembers it.

English and Russian. Android 8.0 or newer with Bluetooth LE.

## Build

Needs JDK 17 and the Android SDK (platform 36). Put the SDK path in `local.properties`
(`sdk.dir=/path/to/sdk`) or set `ANDROID_HOME`.

```sh
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

## The board's settings service

The board advertises as `WT Board` with a settings service `b5f90001-1a2b-4c3d-8e9f-0a1b2c3d4e5f`:

| Characteristic | Properties | Content |
|---|---|---|
| `b5f90002-…` | write | a command: `u`, `d`, `pair`, `mode app`, `mode pod` |
| `b5f90003-…` | read, notify | status, e.g. `mode=app;paired=…;rd=1;ready=1;pairing=0;yield=0;gear=10;addr=…` |
| `b5f90004-…` | notify | `u` or `d` for each board button press while the board yields to the phone |

The derailleur accepts only one app at a time. While the app is subscribed to `b5f90004`, a board in phone mode
disconnects from the derailleur (`yield=1`) and forwards its button presses; the app shifts for it. When the app
closes, the board connects to the derailleur again. The app waits for `yield=1` before it connects to the
derailleur, and reconnects if the derailleur doesn't answer its hello within 6 s.
