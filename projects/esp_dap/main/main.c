/* ESP-DAP: CMSIS-DAP HID (64B) + 1x CDC-UART 桥 + nRESET
 * USB: 原生 USB 19/20，HID 64B 用于 OpenOCD/pyOCD，CDC 透传到 UART1(17/18)
 * SWD: DIO=4 CLK=5 nRST=6 (可改 swd.h)
 * 控制台走 USJ，不与 TinyUSB 抢 PHY
 */
#include <string.h>
#include <inttypes.h>
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
#include "tusb.h"
#include "usb_descriptors.h"
#include "dap.h"
#include "swd.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_rom_sys.h"

static const char *TAG = "esp_dap";

#define CDC_ITF 0
#define HID_ITF 2
#define UART_NUM UART_NUM_1
#define UART_TX  PIN_UART_TX
#define UART_RX  PIN_UART_RX
#define UART_BAUD 115200

static RingbufHandle_t s_uart_ring;

// HID 缓冲
static uint8_t s_hid_req[64];
static uint8_t s_hid_resp[64];
static volatile bool s_hid_pending = false;

// CDC -> UART
static void cdc_rx_cb(int itf, cdcacm_event_t *event) {
    (void)event;
    if (itf != CDC_ITF) return;
    uint8_t buf[512];
    size_t n=0;
    while (tinyusb_cdcacm_read(itf, buf, sizeof(buf), &n)==ESP_OK && n>0) {
        uart_write_bytes(UART_NUM, buf, n);
    }
}
static void cdc_line_coding_cb(int itf, cdcacm_event_t *event) {
    if (itf!=CDC_ITF) return;
    const cdc_line_coding_t *c = event->line_coding_changed_data.p_line_coding;
    ESP_LOGI(TAG, "CDC line coding %" PRIu32, c->bit_rate);
    uart_set_baudrate(UART_NUM, c->bit_rate);
}

// UART -> CDC
static void uart_rx_task(void *arg) {
    (void)arg;
    uint8_t buf[256];
    while(1){
        int n = uart_read_bytes(UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(20));
        if(n>0) xRingbufferSend(s_uart_ring, buf, n, pdMS_TO_TICKS(50));
    }
}
static void uart_to_usb_task(void *arg){
    (void)arg;
    while(1){
        size_t len=0;
        uint8_t *data = xRingbufferReceiveUpTo(s_uart_ring, &len, pdMS_TO_TICKS(20), 512);
        if(data && len>0){
            // 仅当主机打开时发送，避免堆积
            tinyusb_cdcacm_write_queue(CDC_ITF, data, len);
            tinyusb_cdcacm_write_flush(CDC_ITF, 0);
            vRingbufferReturnItem(s_uart_ring, data);
        }
    }
}

// HID callbacks (TinyUSB)
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)reqlen; (void)buffer;
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type;
    if (bufsize==0 || buffer==NULL) return;
    // CMSIS-DAP HID Output report: 64 bytes (host->device)
    uint8_t req[64]={0};
    size_t copy = bufsize>64?64:bufsize;
    memcpy(req, buffer, copy);
    // process
    uint8_t resp[64]={0};
    int rlen = dap_process(req, resp);
    (void)rlen;
    memcpy(s_hid_resp, resp, 64);
    s_hid_pending = true;
    // 触发下一次 Input report (device->host)
    // TinyUSB 会在 tud_hid_report 准备好后通过中断 IN 端点发送
    // 若主机使用 Get_Report 轮询，set 后需主动调用 tud_hid_report
}

// TinyUSB 需要不断调用 tud_hid_report 来把响应送回主机
// 在 hid_task 中轮询
static void hid_task(void *arg){
    (void)arg;
    while(1){
        if(s_hid_pending){
            // TinyUSB HID report ID 0 (无ID)
            // 需等待主机可接收
            if (tud_hid_ready()) {
                tud_hid_report(0, s_hid_resp, 64);
                s_hid_pending=false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// 备用：tud_hid_report_complete_cb 可用于链式
// 进入下载模式 (CDC 波特率 1200 触发，已在usb_uart_bridge验证，用同样RTC寄存器)
static void enter_download_mode(void){
    ESP_LOGW(TAG, "enter download mode");
    REG_SET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
    esp_rom_software_reset_system();
}

static void uart_init(void){
    uart_config_t cfg={
        .baud_rate=UART_BAUD,
        .data_bits=UART_DATA_8_BITS,
        .parity=UART_PARITY_DISABLE,
        .stop_bits=UART_STOP_BITS_1,
        .flow_ctrl=UART_HW_FLOWCTRL_DISABLE,
        .source_clk=UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, 2048, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, UART_TX, UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void app_main(void){
    ESP_LOGI(TAG, "ESP-DAP HID+CDC RST start");
    ESP_LOGI(TAG, "SWDIO=%d SWCLK=%d nRST=%d UART TX=%d RX=%d", PIN_SWDIO, PIN_SWCLK, PIN_nRESET, UART_TX, UART_RX);

    dap_init();
    uart_init();
    s_uart_ring = xRingbufferCreate(8*1024, RINGBUF_TYPE_BYTEBUF);
    assert(s_uart_ring);

    const tusb_desc_device_t *dev = (const tusb_desc_device_t*)desc_device;
    const char **strs = desc_strs;
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = dev;
    tusb_cfg.descriptor.string = strs;
    tusb_cfg.descriptor.string_count = 6;
    tusb_cfg.descriptor.full_speed_config = desc_cfg;

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    ESP_LOGI(TAG, "tinyusb installed, HID+CDC");

    tinyusb_config_cdcacm_t acm_cfg = {
        .callback_rx = cdc_rx_cb,
        .callback_line_coding_changed = cdc_line_coding_cb,
    };
    acm_cfg.cdc_port = TINYUSB_CDC_ACM_0;
    esp_err_t acm_err = tinyusb_cdcacm_init(&acm_cfg);
    if (acm_err != ESP_OK) ESP_LOGW(TAG, "cdc init err %d", acm_err);

    xTaskCreate(uart_rx_task, "uart_rx", 2048, NULL, 5, NULL);
    xTaskCreate(uart_to_usb_task, "uart2usb", 2048, NULL, 5, NULL);
    xTaskCreate(hid_task, "hid", 3072, NULL, 6, NULL);

    ESP_LOGI(TAG, "ready: HID DAP 64B + CDC %d->UART%d", CDC_ITF, UART_NUM);
    ESP_LOGI(TAG, "OpenOCD: openocd -f interface/cmsis-dap.cfg -f target/stm32f4x.cfg -c 'adapter speed 1000'");
    ESP_LOGI(TAG, "CDC: /dev/ttyACM0 or COMx, SWD wiring: S3 GND<->Target GND, %d->SWDIO, %d->SWCLK, %d->nRST(可选), UART %d/%d->Target TX/RX",
             PIN_SWDIO, PIN_SWCLK, PIN_nRESET, UART_TX, UART_RX);
}
