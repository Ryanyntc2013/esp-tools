/*
 * USB 复合设备：2 路 CDC-ACM 虚拟串口 <-> 2 路 UART 3.3V TTL 双向透传桥
 *              + 2 路复位输出（AT 指令控制）
 *
 * - 原生 USB (GPIO19/20) 枚举为复合设备，出现 2 个独立 COM 口：
 *     CDC0 (COMx) <-> UART1: TX=GPIO17, RX=GPIO18
 *     CDC1 (COMy) <-> UART2: TX=GPIO15, RX=GPIO16
 * - 各通道主机端设置的波特率等参数独立同步到对应 UART
 * - 两个 CDC 口均解析 AT 复位指令，响应返回原端口；协议见 docs/at-rst-control.md
 * - 复位输出 GPIO4/5/6/7/8/9（6 台设备）：每脚独立的脉冲宽度与默认电平，
 *   多路复位序列的间隔共享；均可运行时配置
 * - ESP32-S3 USB 端点限制：TINYUSB_CDC_COUNT 最大为 2
 */
#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_cdc_acm.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_rom_sys.h"

#define NUM_CDC             2

#define RESET_PIN_1         4
#define RESET_PIN_2         5
#define RESET_PIN_3         6
#define RESET_PIN_4         7
#define RESET_PIN_5         8
#define RESET_PIN_6         9
#define RESET_PIN_COUNT     6
#define RESET_INTERVAL_MS_DEFAULT 100
#define RESET_PULSE_MS_DEFAULT    100
#define RESET_DEFAULT_LEVEL_DEFAULT 0

#define BRIDGE_UART_BAUD    115200
#define UART_RX_RING_SIZE   (8 * 1024)  /* UART->USB 环形缓冲，吸收 USB 空闲期突发 */
#define CDC_RX_BUF_SIZE     512

/* 每通道配置表：CDC itf <-> UART num / TX / RX 引脚 */
typedef struct {
    int uart_num;
    int tx_pin;
    int rx_pin;
} bridge_ch_t;

static const bridge_ch_t s_ch[NUM_CDC] = {
    { UART_NUM_1, 17, 18 },   /* CDC0 -> UART1 */
    { UART_NUM_2, 15, 16 },   /* CDC1 -> UART2 */
};

static const char *TAG = "bridge";
static RingbufHandle_t s_uart_rx_ring[NUM_CDC];
static volatile bool s_usb_opened[NUM_CDC];  /* 主机已打开端口(DTR=1)才向 USB 转发 */

/* 每个复位脚独立配置：脉冲宽度 + 空闲默认电平 */
typedef struct {
    uint32_t pulse_ms;
    uint8_t default_level;
} reset_pin_cfg_t;

static const int s_reset_pins[RESET_PIN_COUNT] = {
    RESET_PIN_1, RESET_PIN_2, RESET_PIN_3,
    RESET_PIN_4, RESET_PIN_5, RESET_PIN_6,
};

/* 多路复位序列时相邻脉冲之间的间隔（各路共享） */
static uint32_t s_reset_interval_ms = RESET_INTERVAL_MS_DEFAULT;

/* 空闲高阻模式：平时引脚为输入(高阻)，仅在复位脉冲期间临时切为输出。
 * 可避免持续驱动目标 MCU 的复位脚造成电平冲突/漏电。 */
static bool s_reset_idle_high_z = true;

static reset_pin_cfg_t s_reset_cfg[RESET_PIN_COUNT] = {
    { RESET_PULSE_MS_DEFAULT, RESET_DEFAULT_LEVEL_DEFAULT },
    { RESET_PULSE_MS_DEFAULT, RESET_DEFAULT_LEVEL_DEFAULT },
    { RESET_PULSE_MS_DEFAULT, RESET_DEFAULT_LEVEL_DEFAULT },
    { RESET_PULSE_MS_DEFAULT, RESET_DEFAULT_LEVEL_DEFAULT },
    { RESET_PULSE_MS_DEFAULT, RESET_DEFAULT_LEVEL_DEFAULT },
    { RESET_PULSE_MS_DEFAULT, RESET_DEFAULT_LEVEL_DEFAULT },
};
static QueueHandle_t s_reset_queue;

/* 复位顺序串：1..RESET_PIN_COUNT 的不重复排列，最多 RESET_PIN_COUNT 个字符 + '\0' */
#define RESET_ORDER_MAX_LEN (RESET_PIN_COUNT + 1)

typedef struct {
    int itf;
    char order[RESET_ORDER_MAX_LEN];
} reset_cmd_t;

/* 把 '1'..'6' 映射为 0..5 的引脚下标，非法返回 -1 */
static int reset_order_to_index(char c)
{
    if (c < '1' || c > '1' + (RESET_PIN_COUNT - 1)) return -1;
    return c - '1';
}

/* 解析复位顺序：只允许 '1'..'6'，长度 1..6，不允许重复；"BOTH"/"ALL" 为别名 */
static bool parse_reset_order(const char *s, char *out, size_t out_size)
{
    if (strcmp(s, "BOTH") == 0) s = "12";
    else if (strcmp(s, "ALL") == 0) s = "123456";

    size_t len = strlen(s);
    if (len == 0 || len >= out_size) return false;

    bool seen[RESET_PIN_COUNT] = { false };
    for (size_t i = 0; i < len; i++) {
        int idx = reset_order_to_index(s[i]);
        if (idx < 0 || seen[idx]) return false;
        seen[idx] = true;
        out[i] = s[i];
    }
    out[len] = '\0';
    return true;
}

/* ---------------- 复位与 AT 控制 ---------------- */

static void cdc_reply(int itf, const char *text)
{
    tinyusb_cdcacm_write_queue(itf, (const uint8_t *)text, strlen(text));
    tinyusb_cdcacm_write_queue(itf, (const uint8_t *)"\r\n", 2);
    tinyusb_cdcacm_write_flush(itf, 0);
}

/* 引脚切为"高阻空闲"：关闭输出与上下拉，但保留输入使能以便读取目标电平。
 * 输入使能不会向外部驱动电流（仅高阻采样），对外仍是高阻。 */
static void reset_pin_high_z(int pin)
{
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
    gpio_pullup_dis((gpio_num_t)pin);
    gpio_pulldown_dis((gpio_num_t)pin);
}

/* 引脚切为推挽输出，并立即驱动到指定电平 */
static void reset_pin_drive(int pin, int level)
{
    gpio_set_level((gpio_num_t)pin, level);          /* 先设电平，避免切换瞬间毛刺 */
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
}

/* 读取单个复位脚的当前电平（高阻模式下为外部真实电平） */
static int reset_pin_read(int idx)
{
    return gpio_get_level((gpio_num_t)s_reset_pins[idx]);
}

static void reset_one(int idx)
{
    const int pin = s_reset_pins[idx];
    const reset_pin_cfg_t *cfg = &s_reset_cfg[idx];
    const int asserted_level = cfg->default_level ? 0 : 1;

    if (s_reset_idle_high_z) {
        /* 先建立驱动电平再打开输出，脉冲结束后立刻回到高阻 */
        reset_pin_drive(pin, asserted_level);
        vTaskDelay(pdMS_TO_TICKS(cfg->pulse_ms));
        reset_pin_high_z(pin);
    } else {
        gpio_set_level((gpio_num_t)pin, asserted_level);
        vTaskDelay(pdMS_TO_TICKS(cfg->pulse_ms));
        gpio_set_level((gpio_num_t)pin, cfg->default_level);
    }
}

static void reset_sequence(const char *order)
{
    for (size_t i = 0; order[i] != '\0'; i++) {
        reset_one(reset_order_to_index(order[i]));
        if (order[i + 1] != '\0') {
            vTaskDelay(pdMS_TO_TICKS(s_reset_interval_ms));
        }
    }
}

static void enter_download_mode(void);

/* 设置单个复位脚参数；pin 为 1..6 */
static void apply_pin_cfg(int pin, uint32_t pulse_ms, uint8_t default_level)
{
    int idx = reset_order_to_index((char)('0' + pin));
    s_reset_cfg[idx].pulse_ms = pulse_ms;
    s_reset_cfg[idx].default_level = default_level;

    /* 高阻空闲模式下不主动驱动引脚；仅推挽空闲模式才立即更新空闲电平 */
    if (!s_reset_idle_high_z) {
        gpio_set_level((gpio_num_t)s_reset_pins[idx], default_level);
    }
}

/* 切换空闲模式：true=高阻输入，false=推挽输出驱动到默认电平 */
static void set_idle_high_z(bool high_z)
{
    s_reset_idle_high_z = high_z;
    for (int i = 0; i < RESET_PIN_COUNT; i++) {
        if (high_z) {
            reset_pin_high_z(s_reset_pins[i]);
        } else {
            reset_pin_drive(s_reset_pins[i], s_reset_cfg[i].default_level);
        }
    }
}

static bool cdc_handle_line(int itf, char *line)
{
    static const char cmd_prefix[] = "AT+RST=";
    unsigned interval_ms;
    unsigned pin;
    unsigned pulse_ms;
    unsigned default_level;
    unsigned mode;
    char order[RESET_ORDER_MAX_LEN];

    if (strcmp(line, "AT") == 0) {
        cdc_reply(itf, "OK");
    } else if (strncmp(line, cmd_prefix, sizeof(cmd_prefix) - 1) == 0 &&
               parse_reset_order(line + sizeof(cmd_prefix) - 1, order, sizeof(order))) {
        reset_cmd_t cmd = { .itf = itf };
        memcpy(cmd.order, order, sizeof(cmd.order));
        if (xQueueSend(s_reset_queue, &cmd, 0) != pdTRUE) cdc_reply(itf, "ERROR");
    } else if (strcmp(line, "AT+RSTCFG?") == 0) {
        /* 6 路时约 200 字节，留足余量 */
        char response[256];
        int n = snprintf(response, sizeof(response),
                         "+RSTCFG:INTERVAL=%" PRIu32 ",IDLE=%s",
                         s_reset_interval_ms, s_reset_idle_high_z ? "HIGHZ" : "DRIVE");
        for (int i = 0; i < RESET_PIN_COUNT && n > 0 && n < (int)sizeof(response); i++) {
            n += snprintf(response + n, sizeof(response) - n,
                          ",PIN%d=%d,PULSE%d=%" PRIu32 ",DEFAULT%d=%u",
                          i + 1, s_reset_pins[i], i + 1,
                          s_reset_cfg[i].pulse_ms, i + 1,
                          s_reset_cfg[i].default_level);
        }
        cdc_reply(itf, response);
    } else if (strcmp(line, "AT+RSTMODE?") == 0) {
        cdc_reply(itf, s_reset_idle_high_z ? "+RSTMODE:HIGHZ" : "+RSTMODE:DRIVE");
    } else if (sscanf(line, "AT+RSTMODE=%u", &mode) == 1 && mode <= 1) {
        /* 1/0 两种写法：1=高阻空闲(默认)，0=推挽空闲并驱动到默认电平 */
        set_idle_high_z(mode != 0);
        cdc_reply(itf, "OK");
    } else if (strcmp(line, "AT+RSTLVL") == 0 || strcmp(line, "AT+RSTLVL?") == 0) {
        /* 读取全部复位脚当前电平；HIGHZ 模式读到的是目标板真实电平 */
        char response[96];
        int n = snprintf(response, sizeof(response), "+RSTLVL:");
        for (int i = 0; i < RESET_PIN_COUNT && n > 0 && n < (int)sizeof(response); i++) {
            n += snprintf(response + n, sizeof(response) - n, "%sP%d=%d",
                          i == 0 ? "" : ",", i + 1, reset_pin_read(i));
        }
        cdc_reply(itf, response);
    } else if ((strncmp(line, "AT+RSTLVL=", 10) == 0) &&
               sscanf(line + 10, "%u", &pin) == 1 && pin >= 1 && pin <= RESET_PIN_COUNT) {
        /* 读取单个复位脚电平 */
        char response[32];
        snprintf(response, sizeof(response), "+RSTLVL:P%u=%d",
                 pin, reset_pin_read((int)pin - 1));
        cdc_reply(itf, response);
    } else if (sscanf(line, "AT+RSTCFG=%u,%u,%u,%u", &interval_ms, &pin, &pulse_ms,
                      &default_level) == 4 &&
               interval_ms <= 60000 && pin >= 1 && pin <= RESET_PIN_COUNT &&
               pulse_ms > 0 && pulse_ms <= 60000 && default_level <= 1) {
        /* 单脚配置：AT+RSTCFG=<间隔>,<脚号>,<脉冲>,<默认电平> */
        s_reset_interval_ms = interval_ms;
        apply_pin_cfg((int)pin, pulse_ms, (uint8_t)default_level);
        cdc_reply(itf, "OK");
    } else if (sscanf(line, "AT+RSTCFG=%u,%u,%u", &interval_ms, &pulse_ms,
                      &default_level) == 3 &&
               interval_ms <= 60000 && pulse_ms > 0 && pulse_ms <= 60000 && default_level <= 1) {
        /* 全局配置：AT+RSTCFG=<间隔>,<脉冲>,<默认电平>，对所有复位脚生效 */
        s_reset_interval_ms = interval_ms;
        for (int i = 0; i < RESET_PIN_COUNT; i++) {
            apply_pin_cfg(i + 1, pulse_ms, (uint8_t)default_level);
        }
        cdc_reply(itf, "OK");
    } else if (strcmp(line, "AT+DL") == 0 || strcmp(line, "dl") == 0) {
        enter_download_mode();
    } else {
        return false;
    }
    return true;
}

static void reset_task(void *arg)
{
    (void)arg;
    reset_cmd_t cmd;

    while (1) {
        if (xQueueReceive(s_reset_queue, &cmd, portMAX_DELAY) == pdTRUE) {
            reset_sequence(cmd.order);
            cdc_reply(cmd.itf, "OK");
        }
    }
}

static void reset_gpio_init(void)
{
    /* 先把引脚初始化为高阻输入（无上下拉），避免上电瞬间驱动目标复位脚 */
    uint64_t mask = 0;
    for (int i = 0; i < RESET_PIN_COUNT; i++) {
        mask |= (1ULL << s_reset_pins[i]);
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    for (int i = 0; i < RESET_PIN_COUNT; i++) {
        reset_pin_high_z(s_reset_pins[i]);
    }

    if (!s_reset_idle_high_z) {
        /* 推挽空闲模式：切为输出并驱动到默认电平 */
        gpio_config_t out_cfg = {
            .pin_bit_mask = mask,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&out_cfg));
        for (int i = 0; i < RESET_PIN_COUNT; i++) {
            gpio_set_level((gpio_num_t)s_reset_pins[i], s_reset_cfg[i].default_level);
        }
    }
    ESP_LOGI(TAG, "reset pins idle mode: %s", s_reset_idle_high_z ? "HIGH-Z (input)" : "driven");
}

/* ---------------- 诊断命令 ---------------- */

/* 进入下载模式：置 force_download_boot 后软复位，ROM bootloader 进 UART 下载 */
static void enter_download_mode(void)
{
    ESP_LOGW(TAG, "entering download mode...");
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_rom_software_reset_system();
}

/* ---------------- USB -> UART ---------------- */

static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buf[CDC_RX_BUF_SIZE];
    size_t rx = 0;
    static uint8_t cmd_buf[NUM_CDC][96];
    static size_t cmd_len[NUM_CDC];
    static bool cmd_candidate[NUM_CDC];

    while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &rx) == ESP_OK && rx > 0) {
        for (size_t i = 0; i < rx; i++) {
            uint8_t c = buf[i];

            if (!cmd_candidate[itf]) {
                if (c == 'A' || c == 'd') {
                    cmd_candidate[itf] = true;
                    cmd_len[itf] = 1;
                    cmd_buf[itf][0] = c;
                } else {
                    uart_write_bytes(s_ch[itf].uart_num, &c, 1);
                }
                continue;
            }

            if (cmd_len[itf] < sizeof(cmd_buf[itf]) - 1) {
                cmd_buf[itf][cmd_len[itf]++] = c;
            } else {
                uart_write_bytes(s_ch[itf].uart_num, cmd_buf[itf], cmd_len[itf]);
                uart_write_bytes(s_ch[itf].uart_num, &c, 1);
                cmd_len[itf] = 0;
                cmd_candidate[itf] = false;
                continue;
            }

            if (c == '\r' || c == '\n') {
                size_t line_len = cmd_len[itf];
                while (line_len > 0 &&
                       (cmd_buf[itf][line_len - 1] == '\r' || cmd_buf[itf][line_len - 1] == '\n')) {
                    line_len--;
                }
                uint8_t delimiter = cmd_buf[itf][line_len];
                cmd_buf[itf][line_len] = '\0';
                bool handled = cdc_handle_line(itf, (char *)cmd_buf[itf]);
                cmd_buf[itf][line_len] = delimiter;
                if (!handled) {
                    uart_write_bytes(s_ch[itf].uart_num, cmd_buf[itf], cmd_len[itf]);
                }
                cmd_len[itf] = 0;
                cmd_candidate[itf] = false;
            }
        }
    }
}

/* 主机改串口参数（波特率等）→ 同步到对应 UART */
static void cdc_line_coding_cb(int itf, cdcacm_event_t *event)
{
    const cdc_line_coding_t *c = event->line_coding_changed_data.p_line_coding;
    ESP_LOGI(TAG, "ch%d host line coding: %" PRIu32 " 8N%d",
             itf, c->bit_rate, c->data_bits + 5);
    uart_set_baudrate(s_ch[itf].uart_num, c->bit_rate);
}

static void cdc_line_state_cb(int itf, cdcacm_event_t *event)
{
    s_usb_opened[itf] = (event->line_state_changed_data.dtr != 0);
    ESP_LOGI(TAG, "ch%d host DTR=%d RTS=%d -> usb %s", itf,
             event->line_state_changed_data.dtr,
             event->line_state_changed_data.rts,
             s_usb_opened[itf] ? "OPEN" : "CLOSED");
}

/* ---------------- UART -> USB ---------------- */

/* 每通道一个 UART 读取任务，参数为通道号 */
static void uart_rx_task(void *arg)
{
    int ch = (int)(intptr_t)arg;
    uint8_t buf[256];
    while (1) {
        int n = uart_read_bytes(s_ch[ch].uart_num, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (n > 0) {
            xRingbufferSend(s_uart_rx_ring[ch], buf, n, pdMS_TO_TICKS(100));
        }
    }
}

/* 每通道一个转发任务 */
static void uart_to_usb_task(void *arg)
{
    int ch = (int)(intptr_t)arg;
    uint8_t chunk[512];
    while (1) {
        size_t len = 0;
        uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(s_uart_rx_ring[ch], &len,
                                                          pdMS_TO_TICKS(20), sizeof(chunk));
        if (data && len > 0) {
            if (s_usb_opened[ch]) {
                tinyusb_cdcacm_write_queue(ch, data, len);
                tinyusb_cdcacm_write_flush(ch, 0);
            }
            vRingbufferReturnItem(s_uart_rx_ring[ch], data);
        }
    }
}

/* ---------------- init ---------------- */

static void bridge_uart_init(int ch)
{
    uart_config_t cfg = {
        .baud_rate  = BRIDGE_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    int un = s_ch[ch].uart_num;
    ESP_ERROR_CHECK(uart_driver_install(un, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(un, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(un, s_ch[ch].tx_pin, s_ch[ch].rx_pin,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void app_main(void)
{
    ESP_LOGI(TAG, "USB composite: 2x CDC <-> 2x UART (3.3V TTL)");
    ESP_LOGI(TAG, "  CDC0 <-> UART1 TX=%d RX=%d", s_ch[0].tx_pin, s_ch[0].rx_pin);
    ESP_LOGI(TAG, "  CDC1 <-> UART2 TX=%d RX=%d", s_ch[1].tx_pin, s_ch[1].rx_pin);
    ESP_LOGI(TAG, "  RESET out: GPIO%d/GPIO%d/GPIO%d/GPIO%d/GPIO%d/GPIO%d",
             RESET_PIN_1, RESET_PIN_2, RESET_PIN_3,
             RESET_PIN_4, RESET_PIN_5, RESET_PIN_6);

    reset_gpio_init();
    s_reset_queue = xQueueCreate(4, sizeof(reset_cmd_t));
    assert(s_reset_queue);
    xTaskCreate(reset_task, "reset_task", 3072, NULL, 11, NULL);

    for (int ch = 0; ch < NUM_CDC; ch++) {
        bridge_uart_init(ch);
        s_uart_rx_ring[ch] = xRingbufferCreate(UART_RX_RING_SIZE, RINGBUF_TYPE_BYTEBUF);
        assert(s_uart_rx_ring[ch]);
    }

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    tinyusb_config_cdcacm_t acm_cfg = {
        .callback_rx = cdc_rx_cb,
        .callback_line_coding_changed = cdc_line_coding_cb,
        .callback_line_state_changed = cdc_line_state_cb,
    };
    for (int itf = 0; itf < NUM_CDC; itf++) {
        acm_cfg.cdc_port = (tinyusb_cdcacm_itf_t)itf;  /* TINYUSB_CDC_ACM_0 / _1 */
        ESP_ERROR_CHECK(tinyusb_cdcacm_init(&acm_cfg));
    }

    for (int ch = 0; ch < NUM_CDC; ch++) {
        xTaskCreate(uart_rx_task, "uart_rx", 3072, (void *)(intptr_t)ch, 10, NULL);
        xTaskCreate(uart_to_usb_task, "uart2usb", 3072, (void *)(intptr_t)ch, 10, NULL);
    }
    ESP_LOGI(TAG, "ready: 2 virtual COM ports, data flows both ways");
}
