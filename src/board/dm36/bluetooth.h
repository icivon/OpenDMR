/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 * DM36 adaptation by BG2LNY
 *
 * OpenDMR, service: the DM-36 LytLink Bluetooth module (UART2, 115200 8N1).
 * The stock DM36.NRFB.01.026 drives a separate Bluetooth PTT/audio module over AT commands; the commands and
 * responses below were extracted from that firmware (strings at 0x030ba948 ... 0x030bab90).  Only the DM-36
 * build includes this service; DM-32 has no Bluetooth hardware.
 */

#ifndef DM32_BLUETOOTH_H
#define DM32_BLUETOOTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BT_MODE_PASSIVE = 0, // the radio is discoverable as BFDM36 (or the saved name)
    BT_MODE_ACTIVE  = 1, // the radio scans and connects to a headset
} bluetooth_mode_t;

void bluetooth_start(void);          // UART2, settings and the module, DM-36 only
bool bluetooth_connected(void);      // HFP/SCO link up: route the speaker and mic to Bluetooth
bool bluetooth_enabled(void);
void bluetooth_set_enabled(bool on);
bluetooth_mode_t bluetooth_mode(void);
void bluetooth_set_mode(bluetooth_mode_t mode);
const char *bluetooth_name(void);
void bluetooth_set_name(const char *name); // letters and digits only, saved at once
const char *bluetooth_address(void);       // cached HOSTDEVICE response, "" until reported
const char *bluetooth_version(void);       // cached response to AT+VER?
void bluetooth_audio(bool on);             // enable/disable the module's audio (AT+AUDIO/MICEN/SPKEN)
void bluetooth_scan_start(void);           // AT+SCAN=1: clear the list and search for headsets
int bluetooth_device_count(void);
void bluetooth_device_name(int index, char *buffer, size_t size);
void bluetooth_device_connect(int index);  // AT+CONN=<addr>
const char *bluetooth_last_event(void);    // development info
void bluetooth_write_data(const uint8_t *data, uint32_t length); // CPS bytes to the module (AT+DATA=00)
void bluetooth_cps_reply(const uint8_t *data, uint32_t length);  // CPS reply on the channel of the last request

#endif
