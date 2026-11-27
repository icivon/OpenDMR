/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 * DM36 adaptation by BG2LNY
 *
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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "hal.h"
#include "bluetooth.h"
#include "bluetooth_menu.h"
#include "uart2_dm36.h"
#include "cable_link.h"
#include "custom_data.h"
#include "ui.h"
#include "list_screen.h"
#include "widgets.h"

#define BT_UART_BAUD      115200
#define BT_RX_LINE        160
#define BT_NAME_SIZE      16
#define BT_MAX_DEVICES    8
#define BT_SETTINGS_TYPE  0x42544C54UL // "BTLT"

typedef struct {
    uint8_t enabled;
    uint8_t mode;
    char name[BT_NAME_SIZE];
} bt_config_t;

typedef enum {
    ROW_SWITCH = 0,
    ROW_MODE,
    ROW_NAME,
    ROW_ADDRESS,
    ROW_DEVICES,
    ROW_INFO,
} menu_row_t;

static StaticTask_t task_tcb;
static StackType_t task_stack[256];
static TaskHandle_t task;

static volatile bool link_up;
static SemaphoreHandle_t uart2_mutex;
static StaticSemaphore_t uart2_mutex_buffer;
static char line[BT_RX_LINE];
static uint16_t line_len;
static char module_version[32];
static char local_address[13];
static char remote_address[13];
static char last_event[32];
static volatile uint8_t cps_channel; // 0: DATA, 1: ADATA; the reply goes back on the request's channel
static volatile bool cps_reply_expected;
static volatile TickType_t cps_reply_deadline;
static char devices[BT_MAX_DEVICES][13];
static int device_count;

static bt_config_t config = {1, BT_MODE_PASSIVE, "BFDM36"};
static uint8_t cps_data[CABLE_LINK_BUFFER_SIZE];
static uint16_t cps_data_len;
static uint32_t cps_data_remaining;
static bool cps_drop_frame;

static void apply_config(void);
static void write_cps_frame(uint8_t channel, const uint8_t *data, uint32_t length);

static char name_edit[BT_NAME_SIZE];
static int8_t name_tap_key = -1;
static uint8_t name_tap_index;

static const ui_screen_t ui_bluetooth_name_screen;
static const ui_screen_t ui_bluetooth_info_screen;
static const ui_pick_list_t bluetooth_devices;

static const char *const name_taps[] = {
    "0", "1", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9",
};

static bool is_alnum(char c)
{
    return ((c >= '0') && (c <= '9')) || ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z'));
}

static void sanitize_name(char *name)
{
    char clean[BT_NAME_SIZE];
    size_t n = 0;

    for (size_t i = 0; (name[i] != '\0') && (n < (sizeof(clean) - 1)); i++) {
        if (is_alnum(name[i])) {
            clean[n++] = name[i];
        }
    }
    clean[n] = '\0';
    if (n == 0) {
        strcpy(clean, "BFDM36");
    }
    strcpy(name, clean);
}

static void config_load(void)
{
    if (!custom_data_read(BT_SETTINGS_TYPE, (uint8_t *)&config, sizeof(config))) {
        config.enabled = 1;
        config.mode = BT_MODE_PASSIVE;
        strcpy(config.name, "BFDM36");
    }
    config.enabled = config.enabled ? 1 : 0;
    config.mode = (config.mode == BT_MODE_ACTIVE) ? BT_MODE_ACTIVE : BT_MODE_PASSIVE;
    sanitize_name(config.name);
}

static void config_save(void)
{
    (void)custom_data_write(BT_SETTINGS_TYPE, (const uint8_t *)&config, sizeof(config));
}

static void send_raw(const char *s)
{
    uart2_write((const uint8_t *)s, (uint32_t)strlen(s));
}

static void send(const char *s)
{
    if (uart2_mutex == NULL) {
        send_raw(s);
        return;
    }
    xSemaphoreTake(uart2_mutex, portMAX_DELAY);
    send_raw(s);
    xSemaphoreGive(uart2_mutex);
}

static void sendf(const char *fmt, ...)
{
    char buffer[128];
    va_list args;
    int n;

    va_start(args, fmt);
    n = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    if (n > 0) {
        send(buffer);
    }
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

static int hex_value(char c)
{
    if ((c >= '0') && (c <= '9')) {
        return c - '0';
    }
    if ((c >= 'A') && (c <= 'F')) {
        return c - 'A' + 10;
    }
    if ((c >= 'a') && (c <= 'f')) {
        return c - 'a' + 10;
    }
    return -1;
}

static bool parse_address(const char *s, char address[13], const char **end)
{
    size_t length = strlen(s);

    for (size_t offset = 0; (offset + 12) <= length; offset++) {
        bool valid = true;

        for (int i = 0; i < 12; i++) {
            if (hex_value(s[offset + i]) < 0) {
                valid = false;
                break;
            }
        }
        if (valid) {
            for (int i = 0; i < 12; i++) {
                int v = hex_value(s[offset + i]);

                address[i] = (char)((v < 10) ? ('0' + v) : ('A' + v - 10));
            }
            address[12] = '\0';
            if (end != NULL) {
                *end = s + offset + 12;
            }
            return true;
        }
    }
    return false;
}

static void add_device(const char *address)
{
    for (int i = 0; i < device_count; i++) {
        if (!strcmp(devices[i], address)) {
            return;
        }
    }
    if (device_count < BT_MAX_DEVICES) {
        strcpy(devices[device_count++], address);
        ui_redraw();
    }
}

static void parse_scan(const char *s)
{
    char address[13];

    while (*s) {
        const char *end;

        if (!parse_address(s, address, &end)) {
            break;
        }
        add_device(address);
        s = end;
    }
}

static void store_event(const char *prefix, const char *s)
{
    snprintf(last_event, sizeof(last_event), "%s%s", prefix, s);
    last_event[sizeof(last_event) - 1] = '\0';
}

// The stock firmware asks the module for its version, reads the auto-connect state and sets the headset gains.
// Re-issued on READY so a module that boots later than the radio still gets its setup.
static void module_init_commands(void)
{
    send("AT+VER?\r\n");
    send("AT+AUTOCONN?\r\n");
    send("AT+MICGAIN=15\r\n");
    send("AT+SPKGAIN=25\r\n");
}

// Parse one complete response line from the module; returns the byte count of a following data frame.
static uint32_t parse_line(const char *s)
{
    if (!strncmp(s, "CONN", 4)) {
        link_up = true;
        parse_address(s + 4, remote_address, NULL);
        bluetooth_audio(true);
        ui_redraw();
    } else if (!strncmp(s, "DISC", 4)) {
        link_up = false;
        remote_address[0] = '\0';
        bluetooth_audio(false);
        ui_redraw();
    } else if (!strncmp(s, "READY", 5)) {
        module_init_commands();
        apply_config();
        store_event("READY", "");
    } else if (!strncmp(s, "VER:", 4)) {
        strncpy(module_version, s + 4, sizeof(module_version) - 1);
        module_version[sizeof(module_version) - 1] = '\0';
    } else if (!strncmp(s, "SCAN:", 5)) {
        parse_scan(s + 5);
    } else if (!strncmp(s, "HOSTDEVICE", 10)) {
        if (parse_address(s + 10, local_address, NULL)) {
            ui_redraw();
        }
    } else if (!strncmp(s, "LENAME:", 7)) {
        store_event("NAME ", s + 7);
    } else if (!strncmp(s, "PIN:", 4)) {
        store_event("PIN ", s + 4);
    } else if (!strncmp(s, "PTTKEY", 6)) {
        store_event("PTT ", s + 6);
    } else if (!strncmp(s, "PTTBATT", 7)) {
        store_event("BATT ", s + 7);
    } else if (!strncmp(s, "HFREQ", 5)) {
        store_event("HFREQ ", s + 5);
    } else if (!strncmp(s, "+DATA:00,", 9)) {
        cps_channel = 0;
        return parse_decimal(s + 9);
    } else if (!strncmp(s, "+ADATA:00,", 10) || !strncmp(s, "+ADATA:01,", 10)) {
        cps_channel = 1;
        return parse_decimal(s + 10);
    }
    return 0;
}

static void rx(uint8_t c)
{
    if (cps_data_remaining != 0) {
        if (!cps_drop_frame && (cps_data_len < sizeof(cps_data))) {
            cps_data[cps_data_len++] = c;
        }
        cps_data_remaining--;
        if (cps_data_remaining == 0) {
            if (!cps_drop_frame && (cps_data_len != 0)) {
                // The Web Serial CPS tries 'D S <divisor>' to speed up the cable. UART2 is fixed at 115200 and
                // that command would only change UART0, so answer as an old firmware and keep the link in sync.
                if ((cps_data_len >= 2) && (cps_data[0] == 'D') && (cps_data[1] == 'S')) {
                    static const uint8_t no_speed[] = {'D', 'S', 0};

                    write_cps_frame(cps_channel, no_speed, sizeof(no_speed));
                } else {
                    cps_reply_expected = true;
                    cps_reply_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
                    cable_link_inject(cps_data, cps_data_len);
                }
            }
            cps_data_len = 0;
            cps_drop_frame = false;
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
                // Consume at most one buffer's worth: an oversized or disabled link's frame is dropped,
                // not truncated into a valid-looking CPS request.
                cps_drop_frame = !config.enabled || (length > sizeof(cps_data));
                cps_data_remaining = (length < sizeof(cps_data)) ? length : sizeof(cps_data);
            }
        }
        return;
    }
    if (line_len < sizeof(line) - 1) {
        line[line_len++] = (char)c;
    }
}

static void apply_config(void)
{
    sendf("AT+LENAME=%s\r\n", config.name);
    sendf("AT+AUTOCONN=%u\r\n", (unsigned)((config.enabled && (config.mode == BT_MODE_PASSIVE)) ? 1 : 0));
    bluetooth_audio(link_up && config.enabled);
}

static void bt_task(void *arg)
{
    uint8_t c;

    (void)arg;
    // The stock firmware asks the module for its version first (the "LytLink Ver:" line of Device Info) and
    // enables auto-reconnect.  READY arrives from the module once it is alive.
    vTaskDelay(pdMS_TO_TICKS(200));
    module_init_commands();
    apply_config();

    for (;;) {
        while (uart2_read(&c)) {
            rx(c);
        }
        if (cps_reply_expected && ((int32_t)(xTaskGetTickCount() - cps_reply_deadline) >= 0)) {
            cps_reply_expected = false; // the request was dropped or never answered: do not mirror a later reply
        }
        vTaskDelay(2);
    }
}

void bluetooth_start(void)
{
    config_load();
    if (task) {
        return;
    }
    uart2_init(BT_UART_BAUD);
    uart2_mutex = xSemaphoreCreateMutexStatic(&uart2_mutex_buffer);
    task = xTaskCreateStatic(bt_task, "bluetooth", sizeof(task_stack) / sizeof(task_stack[0]), NULL,
                             tskIDLE_PRIORITY + 3, task_stack, &task_tcb);
}

bool bluetooth_connected(void)
{
    return link_up && config.enabled;
}

bool bluetooth_enabled(void)
{
    return config.enabled;
}

void bluetooth_set_enabled(bool on)
{
    bool next = on;

    if (config.enabled == next) {
        return;
    }
    config.enabled = next ? 1 : 0;
    config_save();
    if (!next && remote_address[0]) {
        sendf("AT+DISC=%s\r\n", remote_address);
    }
    apply_config();
}

bluetooth_mode_t bluetooth_mode(void)
{
    return (bluetooth_mode_t)config.mode;
}

void bluetooth_set_mode(bluetooth_mode_t mode)
{
    bluetooth_mode_t next = (mode == BT_MODE_ACTIVE) ? BT_MODE_ACTIVE : BT_MODE_PASSIVE;

    if (config.mode == next) {
        return;
    }
    config.mode = (uint8_t)next;
    config_save();
    apply_config();
}

const char *bluetooth_name(void)
{
    return config.name;
}

void bluetooth_set_name(const char *name)
{
    snprintf(config.name, sizeof(config.name), "%s", name ? name : "");
    config.name[sizeof(config.name) - 1] = '\0';
    sanitize_name(config.name);
    config_save();
    sendf("AT+LENAME=%s\r\n", config.name);
}

const char *bluetooth_address(void)
{
    return local_address;
}

const char *bluetooth_version(void)
{
    return module_version;
}

void bluetooth_audio(bool on)
{
    bool enable = on && config.enabled && link_up;

    if (uart2_mutex == NULL) {
        send(enable ? "AT+AGHF=1\r\n" : "AT+AGHF=0\r\n");
        send(enable ? "AT+SPKEN=1\r\n" : "AT+SPKEN=0\r\n");
        send(enable ? "AT+MICEN=1\r\n" : "AT+MICEN=0\r\n");
        send(enable ? "AT+AUDIO=1\r\n" : "AT+AUDIO=0\r\n");
        return;
    }
    xSemaphoreTake(uart2_mutex, portMAX_DELAY);
    send_raw(enable ? "AT+AGHF=1\r\n" : "AT+AGHF=0\r\n");
    send_raw(enable ? "AT+SPKEN=1\r\n" : "AT+SPKEN=0\r\n");
    send_raw(enable ? "AT+MICEN=1\r\n" : "AT+MICEN=0\r\n");
    send_raw(enable ? "AT+AUDIO=1\r\n" : "AT+AUDIO=0\r\n");
    xSemaphoreGive(uart2_mutex);
}

void bluetooth_scan_start(void)
{
    if (!config.enabled || (config.mode != BT_MODE_ACTIVE)) {
        return;
    }
    device_count = 0;
    send("AT+SCAN=1\r\n");
}

int bluetooth_device_count(void)
{
    return device_count;
}

void bluetooth_device_name(int index, char *buffer, size_t size)
{
    if ((index < 0) || (index >= device_count) || (size == 0)) {
        if (size != 0) {
            buffer[0] = '\0';
        }
        return;
    }
    snprintf(buffer, size, "%s", devices[index]);
}

void bluetooth_device_connect(int index)
{
    if ((index < 0) || (index >= device_count) || !config.enabled) {
        return;
    }
    sendf("AT+CONN=%s\r\n", devices[index]);
}

const char *bluetooth_last_event(void)
{
    return last_event;
}

static void write_cps_frame(uint8_t channel, const uint8_t *data, uint32_t length)
{
    // AT+DATA=00,<length>\r\n<data> / AT+ADATA=01,<length>\r\n<data>: the stock firmware's CPS channels
    char header[32];
    int n;

    if (!config.enabled || (data == NULL) || (length == 0)) {
        return;
    }
    n = snprintf(header, sizeof(header),
                 (channel == 1) ? "AT+ADATA=01,%u\r\n" : "AT+DATA=00,%u\r\n", (unsigned)length);
    if (n <= 0) {
        return;
    }
    if (uart2_mutex == NULL) {
        send_raw(header);
        uart2_write(data, length);
        return;
    }
    xSemaphoreTake(uart2_mutex, portMAX_DELAY);
    send_raw(header);
    uart2_write(data, length);
    xSemaphoreGive(uart2_mutex);
}

void bluetooth_write_data(const uint8_t *data, uint32_t length)
{
    write_cps_frame(0, data, length);
}

void bluetooth_cps_reply(const uint8_t *data, uint32_t length)
{
    if (!cps_reply_expected) {
        return; // a wired CPS request must not be mirrored onto the Bluetooth link
    }
    cps_reply_expected = false;
    if (!config.enabled) {
        return;
    }
    write_cps_frame(cps_channel, data, length);
}

/* ---------------- DM-36 menu ---------------- */

const char *bluetooth_menu_text(text_id_t id)
{
    switch ((int)id) {
    case (int)BT_TEXT_MENU: return "蓝牙互联";
    case (int)BT_TEXT_SWITCH: return "蓝牙开关";
    case (int)BT_TEXT_MODE: return "蓝牙模式";
    case (int)BT_TEXT_NAME: return "蓝牙名称";
    case (int)BT_TEXT_ADDRESS: return "蓝牙地址";
    case (int)BT_TEXT_DEVICES: return "设备列表";
    case (int)BT_TEXT_INFO: return "蓝牙信息";
    case (int)BT_TEXT_ACTIVE: return "主动";
    case (int)BT_TEXT_PASSIVE: return "被动";
    case (int)BT_TEXT_VERSION: return "版本";
    case (int)BT_TEXT_LINK: return "连接";
    case (int)BT_TEXT_LOCAL_NAME: return "名称";
    case (int)BT_TEXT_LOCAL_ADDR: return "地址";
    case (int)BT_TEXT_LAST_EVENT: return "事件";
    case (int)BT_TEXT_CONNECTED: return "已连接";
    case (int)BT_TEXT_IDLE: return "未连接";
    case (int)BT_TEXT_NO_DEVICES: return "无设备";
    case (int)BT_TEXT_SAVING: return "已保存";
    default: return "?";
    }
}

static int menu_count(void)
{
    return 5; // switch, mode, name/address, devices/address, info
}

static menu_row_t menu_row(int index)
{
    if (config.mode == BT_MODE_PASSIVE) {
        static const menu_row_t rows[] = {ROW_SWITCH, ROW_MODE, ROW_NAME, ROW_ADDRESS, ROW_INFO};
        return rows[index];
    }
    static const menu_row_t rows[] = {ROW_SWITCH, ROW_MODE, ROW_ADDRESS, ROW_DEVICES, ROW_INFO};
    return rows[index];
}

static void menu_enter(void *arg)
{
    (void)arg;
    *ui_state() = 0;
}

static bool menu_handle(const ui_event_t *ev)
{
    int32_t *selected = ui_state();
    int count = menu_count();
    bool press = (ev->type == UI_EVENT_KEY_PRESS) || (ev->type == UI_EVENT_KEY_REPEAT);

    if (press && ((ev->key == UI_KEY_UP) || (ev->key == UI_KEY_DOWN))) {
        *selected = (*selected + ((ev->key == UI_KEY_DOWN) ? 1 : (count - 1))) % count;
        return true;
    }
    if (press && BOARD_IS_EXTRA_VALUE_CHANGE_KEY(ev->key)) {
        menu_row_t row = menu_row(*selected);

        if (row == ROW_MODE) {
            bluetooth_set_mode((config.mode == BT_MODE_ACTIVE) ? BT_MODE_PASSIVE : BT_MODE_ACTIVE);
            return true;
        }
        if (row == ROW_SWITCH) {
            bluetooth_set_enabled(!config.enabled);
            return true;
        }
        return true;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN)) {
        switch (menu_row(*selected)) {
        case ROW_NAME:
            ui_push(&ui_bluetooth_name_screen, NULL);
            break;
        case ROW_DEVICES:
            bluetooth_scan_start();
            ui_push(&pick_list_screen, (void *)&bluetooth_devices);
            break;
        case ROW_INFO:
            ui_push(&ui_bluetooth_info_screen, NULL);
            break;
        default:
            break;
        }
        return true;
    }
    list_handle_back(ev);
    return false;
}

static void menu_draw(void)
{
    char position[16];
    int32_t selected = *ui_state();
    int count = menu_count();

    snprintf(position, sizeof(position), "%d/%d", (int)selected + 1, count);
    list_view_draw_title(text(BT_TEXT_MENU), position);

    for (int i = 0; i < count; i++) {
        list_view_row_t row = {.icon = ICONS_COUNT, .selected = (i == selected)};
        char value[24];

        switch (menu_row(i)) {
        case ROW_SWITCH:
            row.name = text(BT_TEXT_SWITCH);
            snprintf(value, sizeof(value), "%s", config.enabled ? text(TEXT_ON) : text(TEXT_OFF));
            break;
        case ROW_MODE:
            row.name = text(BT_TEXT_MODE);
            snprintf(value, sizeof(value), "%s",
                     (config.mode == BT_MODE_ACTIVE) ? text(BT_TEXT_ACTIVE) : text(BT_TEXT_PASSIVE));
            break;
        case ROW_NAME:
            row.name = text(BT_TEXT_NAME);
            snprintf(value, sizeof(value), "%s", config.name);
            break;
        case ROW_ADDRESS:
            row.name = text(BT_TEXT_ADDRESS);
            snprintf(value, sizeof(value), "%s", local_address[0] ? local_address : text(TEXT_NONE));
            break;
        case ROW_DEVICES:
            row.name = text(BT_TEXT_DEVICES);
            snprintf(value, sizeof(value), "%d", device_count);
            break;
        case ROW_INFO:
        default:
            row.name = text(BT_TEXT_INFO);
            value[0] = '\0';
            break;
        }
        row.value = value;
        list_view_draw_row(i, &row);
    }
    bottom_bar_draw_soft_keys(text(TEXT_UI_SELECT), text(TEXT_UI_BACK));
}

/* ---------------- Bluetooth name editing ---------------- */

static void name_enter(void *arg)
{
    (void)arg;
    snprintf(name_edit, sizeof(name_edit), "%s", config.name);
    name_edit[sizeof(name_edit) - 1] = '\0';
    name_tap_key = -1;
    name_tap_index = 0;
}

static void name_commit(void)
{
    if (name_tap_key < 0) {
        return;
    }
    name_tap_key = -1;
    name_tap_index = 0;
}

static bool name_handle(const ui_event_t *ev)
{
    if (ev->type == UI_EVENT_TIMER) {
        if (name_tap_key >= 0) {
            name_commit();
            return true;
        }
        return false;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_GREEN)) {
        name_commit();
        sanitize_name(name_edit);
        bluetooth_set_name(name_edit);
        ui_pop();
        return true;
    }
    if ((ev->type == UI_EVENT_KEY_PRESS) && (ev->key == UI_KEY_HASH)) {
        size_t len = strlen(name_edit);

        if (len > 0) {
            name_edit[len - 1] = '\0';
        }
        name_commit();
        return true;
    }
    if (ev->type == UI_EVENT_KEY_PRESS) {
        int digit = ui_key_digit(ev->key);
        const char *chars;

        if ((digit < 0) || (digit > 9)) {
            return false;
        }
        chars = name_taps[digit];
        if (name_tap_key == digit) {
            name_tap_index++;
        } else {
            name_commit();
            name_tap_key = (int8_t)digit;
            name_tap_index = 0;
        }
        if (chars[name_tap_index] == '\0') {
            name_tap_index = 0;
        }
        size_t len = strlen(name_edit);

        if (len > 0) {
            name_edit[len - 1] = '\0';
        }
        if (strlen(name_edit) < (sizeof(name_edit) - 1)) {
            size_t pos = strlen(name_edit);

            name_edit[pos++] = chars[name_tap_index];
            name_edit[pos] = '\0';
        }
        ui_timer_start(1000);
        return true;
    }
    list_handle_back(ev);
    return false;
}

static void name_draw(void)
{
    list_view_row_t row = {.icon = ICONS_COUNT, .selected = true};

    list_view_draw_title(text(BT_TEXT_NAME), NULL);
    row.name = text(BT_TEXT_LOCAL_NAME);
    row.value = name_edit;
    row.cursor = (int16_t)(strlen(name_edit) + 1);
    list_view_draw_row(0, &row);
    bottom_bar_draw_soft_keys(text(TEXT_UI_SAVE), text(TEXT_UI_BACK));
}

/* ---------------- Device list and debug info ---------------- */

static const ui_pick_list_t bluetooth_devices = {
    .title = BT_TEXT_DEVICES,
    .icon = ICONS_COUNT,
    .count = bluetooth_device_count,
    .name = bluetooth_device_name,
    .pick = bluetooth_device_connect,
};

static bool info_handle(const ui_event_t *ev)
{
    list_handle_back(ev);
    return false;
}

static void info_draw(void)
{
    list_view_row_t row = {.icon = ICONS_COUNT};
    char value[24];

    list_view_draw_title(text(BT_TEXT_INFO), NULL);

    row.name = text(BT_TEXT_VERSION);
    snprintf(value, sizeof(value), "%.23s", module_version[0] ? module_version : text(TEXT_NONE));
    row.value = value;
    list_view_draw_row(0, &row);

    row.name = text(BT_TEXT_LINK);
    snprintf(value, sizeof(value), "%s", bluetooth_connected() ? text(BT_TEXT_CONNECTED) : text(BT_TEXT_IDLE));
    row.value = value;
    list_view_draw_row(1, &row);

    row.name = text(BT_TEXT_SWITCH);
    snprintf(value, sizeof(value), "%s", config.enabled ? text(TEXT_ON) : text(TEXT_OFF));
    row.value = value;
    list_view_draw_row(2, &row);

    row.name = text(BT_TEXT_MODE);
    snprintf(value, sizeof(value), "%s",
             (config.mode == BT_MODE_ACTIVE) ? text(BT_TEXT_ACTIVE) : text(BT_TEXT_PASSIVE));
    row.value = value;
    list_view_draw_row(3, &row);

    row.name = text(BT_TEXT_LOCAL_NAME);
    snprintf(value, sizeof(value), "%.23s", config.name);
    row.value = value;
    list_view_draw_row(4, &row);

    row.name = text(BT_TEXT_LOCAL_ADDR);
    snprintf(value, sizeof(value), "%.23s", local_address[0] ? local_address : text(TEXT_NONE));
    row.value = value;
    list_view_draw_row(5, &row);

    row.name = text(BT_TEXT_LAST_EVENT);
    snprintf(value, sizeof(value), "%.23s", last_event[0] ? last_event : text(TEXT_NONE));
    row.value = value;
    list_view_draw_row(6, &row);

    bottom_bar_draw_soft_keys(NULL, text(TEXT_UI_BACK));
}

static const ui_screen_t ui_bluetooth_name_screen = {
    .enter = name_enter,
    .handle = name_handle,
    .draw = name_draw,
};

static const ui_screen_t ui_bluetooth_info_screen = {
    .handle = info_handle,
    .draw = info_draw,
};

const ui_screen_t bluetooth_menu_screen = {
    .enter = menu_enter,
    .handle = menu_handle,
    .draw = menu_draw,
};

#endif // BOARD_DM36
