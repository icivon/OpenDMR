/*
 * OpenDMR, service: the DM-36 LytLink Bluetooth module (UART2, 115200 8N1).
 * The stock DM36.NRFB.01.026 drives a separate Bluetooth PTT/audio module over AT commands; the commands and
 * responses below were extracted from that firmware (strings at 0x030ba948 ... 0x030bab90).  Only the DM-36
 * build includes this service; DM-32 has no Bluetooth hardware.
 */

#ifndef DM32_BLUETOOTH_H
#define DM32_BLUETOOTH_H

#include <stdbool.h>
#include <stdint.h>

void bluetooth_start(void);          // UART2 and the module, DM-36 only
bool bluetooth_connected(void);      // HFP/SCO link up: route the speaker and mic to Bluetooth
void bluetooth_audio(bool on);       // enable/disable the module's audio (AT+AUDIO/MICEN/SPKEN)
void bluetooth_write_data(const uint8_t *data, uint32_t length); // CPS bytes to the module (AT+DATA)

#endif
