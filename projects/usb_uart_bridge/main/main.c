/*
 * USB 复合设备：2 路 CDC-ACM 虚拟串口 <-> 2 路 UART 3.3V TTL 双向透传桥
 *
 * - 原生 USB (GPIO19/20) 枚举为复合设备，出现 2 个独立 COM 口：
 *     CDC0 (COMx) <-> UART1: TX=GPIO17, RX=GPIO18
 *     CDC1 (COMy) <-> UART2: TX=GPIO15, RX=GPIO16
 * - 各通道主机端设置的波特率等参数独立同步到对应 UART
 * - ESP32-S3 USB 端点限制：TINYUSB_CDC_COUNT 最大为 2
 */
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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

/* ---------------- 诊断命令（CDC0 输入） ----------------
 * 'lb'  : 内部环回自检（UART TX->RX 本地回环，无需跳线）
 * 'dl'  : 软复位进下载模式（TinyUSB 接管 PHY 后 USJ 消失，烧录用）
 */

/* 进入下载模式：置 force_download_boot 后软复位，ROM bootloader 进 UART 下载 */
static void enter_download_mode(void)
{
    ESP_LOGW(TAG, "entering download mode...");
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_rom_software_reset_system();
}

/* 内部环回自检：UART 本地回环（TX 直通 RX），验证 UART 驱动 + 通道映射 */
static void loopback_selftest(int ch)
{
    const char *msg = "SELFTEST-CH0-OK";
    if (ch == 1) msg = "SELFTEST-CH1-OK";
    int un = s_ch[ch].uart_num;
    uint8_t rbuf[64] = {0};

    uart_flush_input(un);
    ESP_ERROR_CHECK(uart_set_loop_back(un, true));
    uart_write_bytes(un, msg, strlen(msg));
    int n = uart_read_bytes(un, rbuf, strlen(msg), pdMS_TO_TICKS(200));
    ESP_ERROR_CHECK(uart_set_loop_back(un, false));

    if (n == (int)strlen(msg) && memcmp(rbuf, msg, n) == 0) {
        ESP_LOGI(TAG, "ch%d loopback PASS (%d bytes)", ch, n);
        tinyusb_cdcacm_write_queue(ch, (const uint8_t *)"LOOPBACK PASS\r\n", 15);
    } else {
        ESP_LOGE(TAG, "ch%d loopback FAIL (got %d)", ch, n);
        tinyusb_cdcacm_write_queue(ch, (const uint8_t *)"LOOPBACK FAIL\r\n", 15);
    }
    tinyusb_cdcacm_write_flush(ch, 0);
}

/* CDC0 行命令解析：lb / dl */
static void handle_cmd(const uint8_t *line, size_t len)
{
    if (len == 2 && memcmp(line, "lb", 2) == 0) {
        for (int ch = 0; ch < NUM_CDC; ch++) loopback_selftest(ch);
    } else if (len == 2 && memcmp(line, "dl", 2) == 0) {
        enter_download_mode();
    }
}

/* ---------------- USB -> UART ---------------- */

static void cdc_rx_cb(int itf, cdcacm_event_t *event)
{
    (void)event;
    uint8_t buf[CDC_RX_BUF_SIZE];
    size_t rx = 0;
    while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &rx) == ESP_OK && rx > 0) {
        /* CDC0 透传前先提取行命令（仅识别完整行，不影响透传数据） */
        if (itf == 0) {
            static uint8_t cmd_buf[32];
            static size_t cmd_len = 0;
            for (size_t i = 0; i < rx; i++) {
                uint8_t c = buf[i];
                if (c == '\r' || c == '\n') {
                    if (cmd_len > 0) {
                        handle_cmd(cmd_buf, cmd_len);
                        cmd_len = 0;
                    }
                } else if (cmd_len < sizeof(cmd_buf)) {
                    cmd_buf[cmd_len++] = c;
                } else {
                    cmd_len = 0;  /* 超长行丢弃 */
                }
            }
        }
        uart_write_bytes(s_ch[itf].uart_num, buf, rx);
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
