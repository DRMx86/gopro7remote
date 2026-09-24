# Control your GoPro Hero 7 with nRF52840

Arduino IDE port of [embeddedclub/bluetooth-keychain-remote](https://github.com/embeddedclub/bluetooth-keychain-remote) (originally Infineon PSoC6 + WICED BT stack) using the Adafruit nRF52 Arduino core and its bundled Bluefruit52Lib, which is the standard way to build nRF52840 BLE central-role apps from the Arduino IDE.

> **Tested only on my GoPro Hero 7.**
> **Use it at your own risk — I'm not responsible if you brick your GoPro 😁**

## Pinout

```cpp
#define SHUTTER_BUTTON_PIN A0   // tap = record start/stop, hold 1.5s = power off
#define MODE_BUTTON_PIN    A1   // cycles Video -> Photo -> Multishot
#define LOCATE_BUTTON_PIN  A2   // hold = camera beeps so you can find it (not working)
#define STATUS_LED_PIN     LED_BUILTIN
```

Buttons are read as `INPUT_PULLUP` (active LOW) — wire each button between the pin and GND.

## Notes on BLE responses

Responses are logged over Serial with a decoded `request_id`/`status` whenever the notification is a simple single-packet response (status `0x00` = OK, `0x01` = error/busy). The multi-packet response format `bluetooth-api.md` documents for larger payloads (like "get camera info") isn't reassembled here, since none of this sketch's commands trigger one.

GoPro never published an official BLE spec for the Hero 7 (Open GoPro only formally documents Hero9+), so this is still community reverse engineering rather than a vendor guarantee. If a command doesn't respond on your camera's firmware, connect with a generic BLE scanner app (nRF Connect for Mobile) while the camera is in pairing mode and confirm the advertised/discovered UUIDs match the ones used here.

## Pairing

Put the Hero7 into pairing mode first — **Connections → Connect Device → GoPro App** on the camera's own menu — before powering on this remote, the same requirement the original firmware had. Bluefruit handles the SMP pairing handshake itself; no extra code is needed for Just Works pairing.

## Building / uploading

Just click **Upload** in Arduino IDE with the correct board and port selected. Open **Tools → Serial Monitor** at **115200 baud** to see scan/connect/discovery/button logs.
connect/discovery/button logs.

