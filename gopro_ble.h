/*
 * gopro_ble.h
 *
 * Arduino IDE port of embeddedclub/bluetooth-keychain-remote's bt_app.h,
 * built on Adafruit's Bluefruit52Lib (Adafruit nRF52 Arduino core).
 *
 * Board requirements (Arduino IDE > Boards Manager):
 *   - Install "Adafruit nRF52 Boards" (by Adafruit)
 *   - Select a board target such as:
 *       Adafruit Feather nRF52840 Express
 *       Adafruit ItsyBitsy nRF52840 Express
 *       Seeed XIAO nRF52840 (via Seeed's board package, also Bluefruit-based)
 *       Nordic nRF52840 DK (supported as "Adafruit's nRF52840 DK" variant)
 *   - Bluefruit52Lib ships with the core, no separate library install needed.
 */

#ifndef GOPRO_BLE_H_
#define GOPRO_BLE_H_

#include <bluefruit.h>

/* Command request IDs, confirmed against KonradIT/goprowifihack's
 * Bluetooth/bluetooth-api.md (verified on HERO4/5/7/8 hardware). This
 * matches the original bt_app.h's CMD_TRIGGER/CMD_MODE opcodes, plus two
 * extra commands documented there that the original code didn't use. */
enum gopro_cmd_t {
    GOPRO_CMD_TRIGGER   = 0x01, /* shutter start/stop, 1-byte param   */
    GOPRO_CMD_MODE      = 0x02, /* switch mode, 1-byte param          */
    GOPRO_CMD_POWER_OFF = 0x04, /* power off / sleep, no param        */
    GOPRO_CMD_LOCATE    = 0x16, /* "locate camera" beep, 1-byte param */
};

/* IMPORTANT: this corrects a bug carried over from the original repo.
 * The original bt_app.h defined PHOTO_MODE=0/VIDEO_MODE=1/TWRAP_MODE=2,
 * but bluetooth-api.md's "Switch mode" command table documents the actual
 * wire values as 0=Video, 1=Photo, 2=Multishot. Using the original's
 * values would silently swap Photo and Video on a real camera. */
enum gopro_mode_t {
    GOPRO_MODE_VIDEO     = 0,
    GOPRO_MODE_PHOTO     = 1,
    GOPRO_MODE_MULTISHOT = 2,
};

/* Call once from setup(). Starts Bluefruit in central mode and begins
 * scanning for a GoPro advertising the FEA6 Control & Query service
 * (equivalent of bt_app_init() + wiced_bt_ble_scan() in the original). */
void gopro_ble_begin();

/* Mirrors the original bt_connected / bt_pairing_done flags */
bool gopro_ble_connected();
bool gopro_ble_ready(); /* connected AND command characteristic discovered */

/* Generic entry point: writes [length][cmd_id][param_len][param...] (or
 * just [length][cmd_id] for zero-param commands) to the GoPro's command
 * characteristic, per bluetooth-api.md's request framing. See the .cpp
 * for the exact byte layout. */
bool gopro_ble_send_raw(gopro_cmd_t cmd, const uint8_t* param, uint8_t param_len);

/* Equivalent of bt_app_gatt_write_goprodata(cmd, value) in the original:
 * sends a command with a single 1-byte parameter. */
inline bool gopro_ble_send_command(gopro_cmd_t cmd, uint8_t value)
{
    return gopro_ble_send_raw(cmd, &value, 1);
}

inline bool gopro_ble_shutter(bool start)
{
    return gopro_ble_send_command(GOPRO_CMD_TRIGGER, start ? 1 : 0);
}

inline bool gopro_ble_set_mode(gopro_mode_t mode)
{
    return gopro_ble_send_command(GOPRO_CMD_MODE, (uint8_t)mode);
}

/* No-parameter command (per bluetooth-api.md, Power off's request has no
 * parameter at all, unlike Trigger/Mode which always carry one byte). */
inline bool gopro_ble_power_off()
{
    return gopro_ble_send_raw(GOPRO_CMD_POWER_OFF, nullptr, 0);
}

/* "Locate camera" beep - handy given this started life as a keychain finder */
inline bool gopro_ble_locate(bool start)
{
    return gopro_ble_send_command(GOPRO_CMD_LOCATE, start ? 1 : 0);
}

#endif /* GOPRO_BLE_H_ */
