# ble-hid-bridge

A Raspberry Pi Pico W that turns a Bluetooth keyboard (with trackpad) into a
plain **USB keyboard + mouse**. Plug the Pico into any machine — servers, BIOS /
boot menus, KVMs, anything without Bluetooth — and type on the wireless keyboard.

Tested with a Samsers foldable "Bluetooth 5.1 Keyboard" with touchpad.

## Use

| Pico LED | Meaning |
|---|---|
| solid | connected — type away |
| fast blink | reconnecting to the saved keyboard (press a key to wake it) |
| slow blink | pairing mode — put the keyboard into pairing mode |

- **First time / new keyboard:** hold **BOOTSEL** while plugging the Pico in, then
  put the keyboard into pairing mode (Samsers: hold **Fn + 1/2/3** until its light
  flashes fast). LED goes solid when paired. The keyboard is saved to flash.
- **After that:** just plug in. It reconnects by itself.
- All keys pass through raw: modifiers, F1–F24, arrows, media keys. Trackpad
  movement, scroll and left/right/middle click become a USB mouse.

## Flash

Prebuilt: hold BOOTSEL, plug in, copy `ble-hid-bridge.uf2` onto the `RPI-RP2` drive.

Build from source (arduino-cli + [arduino-pico](https://github.com/earlephilhower/arduino-pico) core 6.3.0):

```bash
FQBN=rp2040:rp2040:rpipicow:ipbtstack=ipv4btcble,usbstack=picosdk
arduino-cli compile --fqbn $FQBN --output-dir build ble-hid-bridge
arduino-cli upload -p /dev/ttyACM0 --fqbn $FQBN --input-dir build   # no BOOTSEL needed once running
```

Debug build (Bluetooth stack log on the serial port): add
`,dbgport=Serial,dbglvl=Bluetooth` to the FQBN. The serial log never includes
keystrokes.

Serial access without sudo needs a udev rule:
`SUBSYSTEM=="tty", ATTRS{idVendor}=="2e8a", MODE="0666"` in `/etc/udev/rules.d/99-pico.rules`.

## Why it's not just the library example

Three things had to change for the Samsers (and likely most cheap combo keyboards):

1. **Classic, not BLE.** Despite "Bluetooth 5.1" it pairs over BR/EDR. `USE_BLE`
   in the sketch switches back to BLE (HID-over-GATT) for keyboards that need it.
2. **Just Works pairing.** The keyboard is NoInputNoOutput; the library demanded
   MITM (passkey) pairing and never answered the SSP confirmation, so pairing
   failed with HCI auth failure `0x05`. `relaxPairing()` sets NoInputNoOutput,
   no MITM, and accepts the confirmation.
3. **Bigger HID descriptor.** Keyboard + touchpad descriptors overflow the
   library's 300-byte buffer (`0x07` MEMORY_CAPACITY_EXCEEDED). `src/HIDMaster.*`
   is a vendored copy of arduino-pico's `BluetoothHIDMaster` (LGPL-2.1) with a
   1024-byte buffer.

## Limits

- One keyboard at a time.
- HID usages above 0x77 (rare international/system keys) can't be sent through
  the Arduino Keyboard API and are dropped.
- Caps Lock LED on the Bluetooth keyboard isn't driven from the host.
