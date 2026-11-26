/*
 * OpenDMR, service: the DM-36 LytLink Bluetooth module (UART2, 115200 8N1).
 *
 * Extracted from the stock DM36.NRFB.01.026 firmware (strings 0x030ba948-0x030bab90, code 0x03061a2c/
 * 0x030624e4/0x03062764; UART2 base 0x14090000, divisor (clock_apb>>4)/115200):
 *
 *   Commands (module <- radio):
 *     AT+VER?          AT+LENAME?       AT+PIN?          AT+DNAME?       AT+AUTOCONN?
 *     AT+LENAME=...    AT+PIN=...       AT+DNAME=...     AT+CONN=<addr>
 *     AT+SCAN=1/2      AT+LESCAN=1/2    AT+DISC=<addr>   AT+LEDISC=<addr>
 *     AT+AGHF=0/1      (HFP audio on/off)
 *     AT+MICEN=0/1     AT+SPKEN=0/1     AT+AUDIO=0/1
 *     AT+MICGAIN=15    AT+SPKGAIN=25
 *     AT+DATA=00,<n>   AT+ADATA=01,<n>  (CPS / transparent data frames)
 *
 *   Unsolicited (radio <- module):
 *     READY  CONN  DISC  SCAN:001122334455  HOSTDEVICE  PTTKEY  PTTBATT  HFREQ
 *     VER:...  LENAME:...  PIN:...  +DATA:00  +ADATA:00
 *
 * The module carries the HFP audio (a Bluetooth headset / PTT speaker-mic) and transparent CPS data on the same
 * UART2 link.  This service owns that link: it sends the module's AT commands, parses its responses and tracks the
 * link state.  UART2 is polled from the task (the stock firmware registers no UART2 interrupt either).
 *
 * DM-32 has no Bluetooth hardware: the whole file compiles empty there.
 */

#include "board.h"

#if defined(BOARD_DM36)

#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "hal.h"
#include "bluetooth.h"
#include "uart2_dm36.h"
#include "cable_link.h"

#define BT_UART_BAUD      115200
#define BT_RX_LINE        128

static StaticTask_t task_tcb;
static StackType_t task_stack[256];
static TaskHandle_t task;

static volatile bool link_up;
static char line[BT_RX_LINE];
static uint16_t line_len;
static char module_version[32];
static uint8_t cps_data[CABLE_LINK_BUFFER_SIZE];
static uint16_t cps_data_len;
static uint16_t cps_data_remaining;

static void send(const char *s)
{
    uart2_write((const uint8_t *)s, (uint32_t)strlen(s));
}

static uint32_t parse_decimal(const char *s)
{
    uint32_t value = 0;

    while ((*s >= '0') && (*s <= '9')) {
        value = (value * 10) + (uint32_t)(*s - '0');
        s++;
    }
    return value;
}

// Parse one complete response line from the module; returns the byte count of a following data frame.
static uint32_t parse_line(const char *s)
{
    if (!strncmp(s, "CONN", 4)) {
        link_up = true;
    } else if (!strncmp(s, "DISC", 4)) {
        link_up = false;
    } else if (!strncmp(s, "VER:", 4)) {
        strncpy(module_version, s + 4, sizeof(module_version) - 1);
        module_version[sizeof(module_version) - 1] = '\0';
    } else if (!strncmp(s, "+DATA:00,", 9)) {
        return parse_decimal(s + 9);
    } else if (!strncmp(s, "+ADATA:01,", 10)) {
        return parse_decimal(s + 10);
    }
    return 0;
}

static void rx(uint8_t c)
{
    if (cps_data_remaining != 0) {
        if (cps_data_len < sizeof(cps_data)) {
            cps_data[cps_data_len++] = c;
            cps_data_remaining--;
        } else {
            cps_data_remaining = 0;
            cps_data_len = 0;
        }
        if (cps_data_remaining == 0) {
            if (cps_data_len != 0) {
                cable_link_inject(cps_data, cps_data_len);
                cps_data_len = 0;
            }
        }
        return;
    }

    if ((c == '\n') || (c == '\r')) {
        if (line_len != 0) {
            uint32_t length;

            line[line_len] = '\0';
            length = parse_line(line);
            line_len = 0;
            if (length != 0) {
                cps_data_len = 0;
                cps_data_remaining = (uint16_t)((length < sizeof(cps_data)) ? length : sizeof(cps_data));
            }
        }
        return;
    }
    if (line_len < sizeof(line) - 1) {
        line[line_len++] = (char)c;
    }
}

static void bt_task(void *arg)
{
    uint8_t c;

    (void)arg;
    // The stock firmware asks the module for its version first (the "LytLink Ver:" line of Device Info) and
    // enables auto-reconnect.  READY arrives from the module once it is alive.
    vTaskDelay(pdMS_TO_TICKS(200));
    send("AT+VER?\r\n");
    send("AT+AUTOCONN?\r\n");
    send("AT+MICGAIN=15\r\n");
    send("AT+SPKGAIN=25\r\n");

    for (;;) {
        while (uart2_read(&c)) {
            rx(c);
        }
        vTaskDelay(2);
    }
}

void bluetooth_start(void)
{
    if (task) {
        return;
    }
    uart2_init(BT_UART_BAUD);
    task = xTaskCreateStatic(bt_task, "bluetooth", sizeof(task_stack) / sizeof(task_stack[0]), NULL,
                             tskIDLE_PRIORITY + 3, task_stack, &task_tcb);
}

bool bluetooth_connected(void)
{
    return link_up;
}

const char *bluetooth_version(void)
{
    return module_version;
}

void bluetooth_audio(bool on)
{
    // HFP audio, speaker and microphone enables, as on the stock firmware's Bluetooth menu
    send(on ? "AT+AGHF=1\r\n" : "AT+AGHF=0\r\n");
    send(on ? "AT+SPKEN=1\r\n" : "AT+SPKEN=0\r\n");
    send(on ? "AT+MICEN=1\r\n" : "AT+MICEN=0\r\n");
    send(on ? "AT+AUDIO=1\r\n" : "AT+AUDIO=0\r\n");
}

void bluetooth_write_data(const uint8_t *data, uint32_t length)
{
    // AT+DATA=00,<length>\r\n<data>: the stock firmware's transparent CPS channel
    char header[32];
    int n = snprintf(header, sizeof(header), "AT+DATA=00,%u\r\n", (unsigned)length);

    if (n > 0) {
        uart2_write((const uint8_t *)header, (uint32_t)n);
    }
    uart2_write(data, length);
}

#endif // BOARD_DM36
