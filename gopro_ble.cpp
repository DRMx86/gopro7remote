/*
 * gopro_ble.cpp
 *
 * Arduino/Bluefruit52Lib port of embeddedclub/bluetooth-keychain-remote's
 * source/bt/bt_app.c.
 *
 * Mapping from the original (WICED, on PSoC6) to here (Bluefruit52Lib, nRF52840):
 *
 *   original                                  here
 *   -----------------------------------       -----------------------------------
 *   wiced_bt_stack_init()                     Bluefruit.begin(0, 1)
 *   wiced_bt_ble_scan() + scan_result_cback   Bluefruit.Scanner + filterUuid(FEA6)
 *   wiced_bt_gatt_le_connect()                Bluefruit.Central.connect(report)
 *   BTM_PAIRING_* events                      (Bluefruit handles pairing internally)
 *   wiced_bt_gatt_client_send_discover()      BLEClientService::discover()
 *   bt_app_gatt_write_goprodata()             gopro_ble_send_command() -> BLEClientCharacteristic::write()
 *
 * GoPro identifiers and command framing confirmed against
 * KonradIT/goprowifihack's Bluetooth/bluetooth-api.md, which documents
 * PacketLogger captures of the real GoPro app tested on HERO4/5/7/8:
 *   - Advertised services  : 0xFEA5 and 0xFEA6 (this code uses FEA6, same
 *                            as the original repo's GOPRO3 array)
 *   - Command char (write) : b5f90072-aa8d-11e3-9046-0002a5d5c51b (handle ~0x2F)
 *   - Command response/notify : b5f90073-aa8d-11e3-9046-0002a5d5c51b (handle ~0x31)
 *
 * Command request framing per bluetooth-api.md's "Command requests" table:
 *   [length][request_id][param_len][param...]   e.g. shutter start: 03 01 01 01
 *   [length][request_id]                        e.g. power off:     01 04
 * where `length` counts every byte after itself.
 *
 * This also FIXES a bug carried over from the original repo: its
 * PHOTO_MODE=0/VIDEO_MODE=1/TWRAP_MODE=2 enum doesn't match the camera's
 * real wire values. bluetooth-api.md's "Switch mode" table documents the
 * actual values as 0=Video, 1=Photo, 2=Multishot (see gopro_ble.h).
 */

#include <string.h>
#include "gopro_ble.h"

/* 128-bit UUIDs below are in Bluefruit's expected byte order (little-endian /
 * "on-air" order, i.e. the reverse of how the UUID is normally written as text).
 * b5f90072-aa8d-11e3-9046-0002a5d5c51b reversed -> array below. */
static uint8_t const GOPRO_CMD_CHAR_UUID[16] = {
    0x1b, 0xc5, 0xd5, 0xa5, 0x02, 0x00, 0x46, 0x90,
    0xe3, 0x11, 0x8d, 0xaa, 0x72, 0x00, 0xf9, 0xb5
};

/* b5f90073-aa8d-11e3-9046-0002a5d5c51b reversed */
static uint8_t const GOPRO_RSP_CHAR_UUID[16] = {
    0x1b, 0xc5, 0xd5, 0xa5, 0x02, 0x00, 0x46, 0x90,
    0xe3, 0x11, 0x8d, 0xaa, 0x73, 0x00, 0xf9, 0xb5
};

/* Primary service is just the 16-bit UUID 0xFEA6, same as the 128-bit
 * "GOPRO3" array the original bt_app.c decoded to (0000fea6-0000-1000-8000-00805f9b34fb). */
static BLEClientService gopro_svc(0xFEA6);
static BLEClientCharacteristic gopro_cmd_char(GOPRO_CMD_CHAR_UUID);
static BLEClientCharacteristic gopro_rsp_char(GOPRO_RSP_CHAR_UUID);

static volatile bool s_connected = false;
static volatile bool s_ready = false; /* mirrors bt_connected in the original */

bool gopro_ble_connected() { return s_connected; }
bool gopro_ble_ready() { return s_ready; }

/* Equivalent of GATTC_OPTYPE_NOTIFICATION handling in bt_app_gatt_event_cb().
 * Response framing per bluetooth-api.md: [length][request_id][status][content...],
 * where length excludes itself and status 0x00 = success, 0x01 = error/busy.
 * We only decode the simple single-packet case here (length < 0x20); a
 * multi-packet response (length byte 0x20/0x21 continuation) would need
 * reassembly across several notifications, which most single-word remote
 * commands (shutter/mode/locate/power-off) never trigger. */
static void rsp_notify_callback(BLEClientCharacteristic* chr, uint8_t* data, uint16_t len)
{
    Serial.print("GoPro response raw: ");
    for (uint16_t i = 0; i < len; i++) {
        if (data[i] < 0x10) Serial.print('0');
        Serial.print(data[i], HEX);
        Serial.print(' ');
    }
    Serial.println();

    if (len >= 3 && data[0] < 0x20) {
        Serial.print("  -> request_id=0x");
        Serial.print(data[1], HEX);
        Serial.print(" status=0x");
        Serial.print(data[2], HEX);
        Serial.println(data[2] == 0x00 ? " (OK)" : " (ERROR/BUSY)");
    }
}

/* Equivalent of bt_app_gatt_write_goprodata() / bt_app_gatt_write_goprodLED(),
 * generalized to also support zero-param commands (e.g. Power off) per
 * bluetooth-api.md's request table. Frame layout:
 *   with param:    [1 + 1 + param_len][cmd_id][param_len][param...]
 *   without param: [1][cmd_id]
 */
bool gopro_ble_send_raw(gopro_cmd_t cmd, const uint8_t* param, uint8_t param_len)
{
    if (!s_ready) {
        Serial.println("gopro_ble: not ready, GoPro not connected/discovered yet");
        return false;
    }
    if (param_len > 4) {
        Serial.println("gopro_ble: param_len too large for this helper");
        return false;
    }

    uint8_t payload[8];
    uint8_t n = 0;

    if (param_len == 0) {
        payload[n++] = 0x01;          /* length: just the cmd_id byte follows */
        payload[n++] = (uint8_t)cmd;
    } else {
        payload[n++] = 1 + 1 + param_len; /* cmd_id + param_len byte + param bytes */
        payload[n++] = (uint8_t)cmd;
        payload[n++] = param_len;
        memcpy(&payload[n], param, param_len);
        n += param_len;
    }

    uint16_t written = gopro_cmd_char.write(payload, n);
    if (written != n) {
        Serial.println("gopro_ble: command write failed");
        return false;
    }

    Serial.print("gopro_ble: sent cmd 0x");
    Serial.println(cmd, HEX);
    return true;
}

/* Fires when the SMP pairing procedure finishes (success or failure). */
static void pair_complete_callback(uint16_t conn_handle, uint8_t auth_status)
{
    if (auth_status == BLE_GAP_SEC_STATUS_SUCCESS) {
        Serial.println("gopro_ble: pairing SUCCEEDED - camera should now show connected");
    } else {
        Serial.print("gopro_ble: pairing FAILED, auth_status 0x");
        Serial.println(auth_status, HEX);
    }
}

/* Fires once the link is actually encrypted (either right after pairing,
 * or immediately on reconnect to an already-bonded camera). */
static void secured_callback(uint16_t conn_handle)
{
    Serial.println("gopro_ble: link secured/encrypted");
}

/* Equivalent of bt_app_scan_result_cback(): the Scanner's UUID filter
 * (set in gopro_ble_begin()) already restricts callbacks to devices
 * advertising FEA6, so any report reaching here is a GoPro candidate. */
static void scan_callback(ble_gap_evt_adv_report_t* report)
{
    Serial.println("gopro_ble: found a device advertising FEA6, connecting...");
    Bluefruit.Central.connect(report);
    /* Must resume scanning manually if the connect attempt fails/expires;
     * Bluefruit re-arms scanning automatically otherwise (checkParams). */
    Bluefruit.Scanner.resume();
}

/* Equivalent of GATT_CONNECTION_STATUS_EVT (connected branch) +
 * wiced_bt_gatt_client_send_discover() in bt_app_gatt_conn_status_cb().
 *
 * IMPORTANT: this explicitly requests pairing/bonding. Without it, the
 * BLE link can connect and even let you discover services (as the FEA6
 * service and its characteristics did in your log), but the Hero7 won't
 * show any "connected" indicator on its screen and may reject command
 * writes, because its firmware ties both of those to an encrypted/bonded
 * link rather than a bare unauthenticated GATT connection. */
static void connect_callback(uint16_t conn_handle)
{
    Serial.println("gopro_ble: connected, discovering GoPro service...");
    s_connected = true;

    /* Let the link settle briefly before hammering it with discovery
     * requests. Not strictly required on hardware with a solid antenna,
     * but cheap insurance against a marginal radio racing the still-
     * stabilizing connection (see the 0x3E disconnect note above
     * gopro_ble_begin()). */
    delay(300);

    if (!gopro_svc.discover(conn_handle)) {
        Serial.println("gopro_ble: FEA6 service not found, disconnecting");
        Bluefruit.disconnect(conn_handle);
        return;
    }

    if (!gopro_cmd_char.discover()) {
        Serial.println("gopro_ble: command characteristic not found, disconnecting");
        Bluefruit.disconnect(conn_handle);
        return;
    }

    if (gopro_rsp_char.discover()) {
        gopro_rsp_char.enableNotify();
    } else {
        Serial.println("gopro_ble: response characteristic not found (continuing without it)");
    }

    s_ready = true;
    Serial.println("gopro_ble: GoPro ready to accept commands");

    /* Ask the camera to pair/bond now. This is non-blocking: results
     * arrive later via pair_complete_callback()/secured_callback(). If
     * the camera was already bonded to this board from a previous
     * session, Bluefruit re-secures automatically using the stored key
     * and this call is effectively a no-op. */
    BLEConnection* connection = Bluefruit.Connection(conn_handle);
    if (connection && !connection->secured()) {
        Serial.println("gopro_ble: requesting pairing...");
        connection->requestPairing();
    }
}

/* Equivalent of the disconnected branch of bt_app_gatt_conn_status_cb(). */
static void disconnect_callback(uint16_t conn_handle, uint8_t reason)
{
    (void)conn_handle;
    Serial.print("gopro_ble: disconnected, reason 0x");
    Serial.println(reason, HEX);
    s_connected = false;
    s_ready = false;
    /* Bluefruit.Scanner.restartOnDisconnect(true) (set in gopro_ble_begin())
     * automatically resumes scanning, same as re-arming
     * wiced_bt_ble_scan() after BTM_BLE_SCAN_STATE_CHANGED_EVT. */
}

/* Equivalent of bt_app_init(): registers callbacks and starts scanning,
 * filtered to the FEA6 service just like wiced_bt_ble_scan(...) +
 * checking BTM_BLE_ADVERT_TYPE_16SRV_PARTIAL in the original. */
void gopro_ble_begin()
{
    Bluefruit.begin(0, 1); /* 0 peripheral, 1 central link */
    Bluefruit.setName("GoPro-Remote");

    /* Ask for a slower, more RF-forgiving connection interval (20-40ms)
     * instead of Bluefruit's tighter default. Boards with a smaller/weaker
     * antenna (e.g. nice!nano) are more prone to sync failures (HCI
     * disconnect reason 0x3E, "Connection Failed to be Established") on
     * tighter intervals. Units are 1.25ms each: 16*1.25=20ms, 32*1.25=40ms. */
    Bluefruit.Central.setConnInterval(16, 32);

    Bluefruit.Central.setConnectCallback(connect_callback);
    Bluefruit.Central.setDisconnectCallback(disconnect_callback);

    /* Just Works pairing (no PIN/passkey UI needed on this board) - this
     * is also Bluefruit's default, set explicitly here for clarity. */
    Bluefruit.Security.setIOCaps(false, false, false); /* no display, no yes/no, no keyboard */
    Bluefruit.Security.setPairCompleteCallback(pair_complete_callback);
    Bluefruit.Security.setSecuredCallback(secured_callback);

    gopro_svc.begin();
    gopro_cmd_char.begin();
    gopro_rsp_char.begin();
    gopro_rsp_char.setNotifyCallback(rsp_notify_callback);

    Bluefruit.Scanner.setRxCallback(scan_callback);
    Bluefruit.Scanner.restartOnDisconnect(true);
    Bluefruit.Scanner.filterUuid(gopro_svc.uuid);
    Bluefruit.Scanner.useActiveScan(true); /* need scan responses to see full adv data */
    Bluefruit.Scanner.start(0);            /* 0 = scan forever */

    Serial.println("gopro_ble: scanning for GoPro camera (service FEA6)...");
}
