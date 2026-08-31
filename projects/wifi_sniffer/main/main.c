#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "nvs_flash.h"

/* WiFi 信号嗅探器：混杂模式被动接收 2.4G 管理帧+数据帧（数据帧含 EAPOL 登录握手）。
 * 普通模式：信道跳频 + AP 信息表周期打印（人读）；串口收到 'p' 切入 PCAP。
 * PCAP 模式：串口输出 pcap(radiotap) 二进制流；收到 'q' 切回普通模式。
 * 信道控制（两种模式均可用）：串口发送 1-13+回车 锁定信道，'h' 恢复跳频。
 * PC 端用 tools/serial_pcap.py 抓流后交给 Wireshark 分析。 */

#define AP_TABLE_MAX     40
#define HOP_INTERVAL_MS  200
#define SUMMARY_PERIOD_S 10
#define CHAN_MIN         1
#define CHAN_MAX         13
#define BOOT_BTN_GPIO    GPIO_NUM_0

static const char *TAG = "sniffer";
static bool s_pcap_mode;
static TaskHandle_t s_report_task;

/* 模式经 NVS 持久化，串口字符 'p'/'q' 远程切换 */
#define NVS_MODE_KEY "mode"

typedef struct {
    uint8_t    bssid[6];
    char       ssid[33];
    uint8_t    channel;
    int8_t     best_rssi;
    uint32_t   frames;
    TickType_t last_seen;
} ap_entry_t;

static ap_entry_t s_aps[AP_TABLE_MAX];
static int        s_ap_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t   s_chan_pkts[CHAN_MAX + 1];
static uint32_t   s_total_pkts;

static void mac_to_str(const uint8_t *mac, char *out)
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---- PCAP 输出（小端，LINKTYPE_IEEE802_11_RADIOTAP=127）---- */
static void pcap_write(const void *data, size_t len)
{
    fwrite(data, 1, len, stdout);
}

static void pcap_emit_global_header(void)
{
    const uint8_t hdr[24] = {
        0xd4, 0xc3, 0xb2, 0xa1,       // magic (us, little-endian)
        0x02, 0x00, 0x04, 0x00,       // version 2.4
        0x00, 0x00, 0x00, 0x00,       // thiszone
        0x00, 0x00, 0x00, 0x00,       // sigfigs
        0xff, 0xff, 0x00, 0x00,       // snaplen
        0x7f, 0x00, 0x00, 0x00        // radiotap linktype
    };
    pcap_write(hdr, sizeof(hdr));
}

/* radiotap: present = CHANNEL(bit3) | DBM_ANT_SIGNAL(bit5) */
static void pcap_emit_packet(const uint8_t *payload, size_t len,
                             int8_t rssi, uint8_t channel, uint64_t ts_us)
{
    /* 单缓冲一次写入，保证记录原子性（16B记录头 + 13B radiotap + 帧） */
    static uint8_t rec[16 + 13 + 1520];
    if (len > 1520) {
        len = 1520;
    }
    uint8_t *rt = rec + 16;
    rt[0] = 0;                                  // rev
    rt[1] = 0;                                  // pad
    rt[2] = 13;                                 // header len
    rt[3] = 0;
    rt[4] = 0x28; rt[5] = 0x00; rt[6] = 0; rt[7] = 0;   // present mask
    uint16_t freq = 2407 + 5 * channel;
    uint16_t cflags = 0x00a0;                   // 2GHz CCK|OFDM
    memcpy(&rt[8], &freq, 2);
    memcpy(&rt[10], &cflags, 2);
    rt[12] = (uint8_t)rssi;

    uint32_t total = (uint32_t)(len + 13);
    uint32_t sec = (uint32_t)(ts_us / 1000000);
    uint32_t usec = (uint32_t)(ts_us % 1000000);
    memcpy(rec, &sec, 4);
    memcpy(rec + 4, &usec, 4);
    memcpy(rec + 8, &total, 4);
    memcpy(rec + 12, &total, 4);
    memcpy(rec + 29, payload, len);
    pcap_write(rec, 29 + len);
}

/* 在 tagged parameters 区域查找指定 IE，返回值指针，长度经 out 参数带出 */
static const uint8_t *find_ie(const uint8_t *ies, size_t len, uint8_t tag, size_t *ie_len)
{
    size_t i = 0;
    while (i + 2 <= len) {
        size_t elen = ies[i + 1];
        if (i + 2 + elen > len) {
            break; // malformed frame, stop parsing IEs
        }
        if (ies[i] == tag) {
            *ie_len = elen;
            return &ies[i + 2];
        }
        i += 2 + elen;
    }
    return NULL;
}

static ap_entry_t *table_find_or_add(const uint8_t *bssid, TickType_t now)
{
    int oldest = 0;
    for (int i = 0; i < s_ap_count; i++) {
        if (memcmp(s_aps[i].bssid, bssid, 6) == 0) {
            return &s_aps[i];
        }
        if (s_aps[i].last_seen < s_aps[oldest].last_seen) {
            oldest = i;
        }
    }
    if (s_ap_count < AP_TABLE_MAX) {
        ap_entry_t *e = &s_aps[s_ap_count++];
        memset(e, 0, sizeof(*e));
        memcpy(e->bssid, bssid, 6);
        e->best_rssi = INT8_MIN; // RSSI 为负值，须从最小值起比较
        e->last_seen = now;
        return e;
    }
    // 表满，淘汰最久未见的条目复用槽位
    ap_entry_t *e = &s_aps[oldest];
    memset(e, 0, sizeof(*e));
    memcpy(e->bssid, bssid, 6);
    e->best_rssi = INT8_MIN;
    e->last_seen = now;
    return e;
}

/* 原地切换模式：不重启，避免重启横幅与日志文本混入二进制流 */
static void switch_mode(bool pcap)
{
    nvs_handle_t h;
    if (nvs_open("sniff", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_MODE_KEY, pcap ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }

    if (pcap) {
        vTaskSuspend(s_report_task);            // 先停汇总打印再转二进制
        portENTER_CRITICAL(&s_lock);
        s_total_pkts = 0;
        memset(s_chan_pkts, 0, sizeof(s_chan_pkts));
        s_pcap_mode = true;
        portEXIT_CRITICAL(&s_lock);
        esp_log_level_set("*", ESP_LOG_NONE);   // 静音后等在途日志写完
        vTaskDelay(pdMS_TO_TICKS(150));
        setvbuf(stdout, NULL, _IONBF, 0);
        pcap_emit_global_header();
    } else {
        fflush(stdout);
        setvbuf(stdout, NULL, _IOLBF, 1024);
        portENTER_CRITICAL(&s_lock);
        s_pcap_mode = false;
        portEXIT_CRITICAL(&s_lock);
        esp_log_level_set("*", ESP_LOG_INFO);
        ESP_LOGI(TAG, "log mode; send 'p' for PCAP");
        vTaskResume(s_report_task);
    }
}

/* stdin 轮询：log 模式收 'p'，pcap 模式收 'q'；
 * 数字+回车锁信道，'h' 恢复跳频（PCAP 模式下不回显，避免污染二进制流）。
 * 注意不能用 O_NONBLOCK：IDF v6.2 USJ vfs 非阻塞路径恒返回 EOF（available 恒 0），
 * 数据会积压在 hw FIFO；fgetc 阻塞路径会直接读 hw FIFO，无数据立即返回 EOF */
static volatile int s_locked_ch;    // 0=跳频, 1-13=锁定

static void stdin_poll_task(void *arg)
{
    char numbuf[4] = {0};
    size_t nlen = 0;
    for (;;) {
        int c = fgetc(stdin);
        if (c != EOF) {
            if (c == 'p' && !s_pcap_mode) {
                switch_mode(true);
            } else if (c == 'q' && s_pcap_mode) {
                switch_mode(false);
            } else if (c == 'h') {
                s_locked_ch = 0;
                if (!s_pcap_mode) {
                    ESP_LOGI(TAG, "channel hopping resumed");
                }
            } else if (c >= '0' && c <= '9' && nlen < sizeof(numbuf) - 1) {
                numbuf[nlen++] = (char)c;
            } else if (c == '\n' || c == '\r') {
                if (nlen > 0) {
                    int ch = atoi(numbuf);
                    if (ch >= CHAN_MIN && ch <= CHAN_MAX) {
                        s_locked_ch = ch;
                        if (!s_pcap_mode) {
                            ESP_LOGI(TAG, "channel locked to %d", ch);
                        }
                    } else if (!s_pcap_mode) {
                        ESP_LOGI(TAG, "invalid channel %d (1-13)", ch);
                    }
                }
                nlen = 0;
                numbuf[0] = '\0';
            } else {
                nlen = 0;
                numbuf[0] = '\0';
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void sniffer_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    /* 管理帧(auth/assoc/beacon) + 数据帧(EAPOL 登录握手)都收 */
    if (buf == NULL || (type != WIFI_PKT_MGMT && type != WIFI_PKT_DATA)) {
        return;
    }
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    size_t len = pkt->rx_ctrl.sig_len;

    if (len < 24) {
        return;
    }
    uint16_t fc = payload[0] | (payload[1] << 8);
    bool is_mgmt = ((fc >> 2) & 0x3) == 0;
    if (!is_mgmt && ((fc >> 2) & 0x3) != 2) { // 控制帧丢弃
        return;
    }
    uint8_t subtype = (fc >> 4) & 0xF;

    portENTER_CRITICAL(&s_lock);
    s_total_pkts++;
    uint8_t ch = pkt->rx_ctrl.channel;
    if (ch >= CHAN_MIN && ch <= CHAN_MAX) {
        s_chan_pkts[ch]++;
    }

    if (s_pcap_mode) {
        // PCAP 模式：管理帧全量转发为二进制流（锁外写，先拷贝元数据）
        size_t plen = len;
        int8_t rssi = pkt->rx_ctrl.rssi;
        uint64_t ts = pkt->rx_ctrl.timestamp;
        portEXIT_CRITICAL(&s_lock);
        pcap_emit_packet(payload, plen, rssi, ch, ts);
        return;
    }

    if (!is_mgmt) { // log 模式下数据帧只计数，不进 AP 表
        portEXIT_CRITICAL(&s_lock);
        return;
    }

    // beacon(8)/probe-resp(5)/probe-req(4)，发送方均在 addr2
    if (subtype != 8 && subtype != 5 && subtype != 4) {
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    const uint8_t *src = &payload[10];

    size_t ie_off = (subtype == 4) ? 24 : 36;
    char ssid[33] = {0};
    bool has_ssid = false;
    if (len > ie_off) {
        size_t elen = 0;
        const uint8_t *ie = find_ie(payload + ie_off, len - ie_off, 0 /* SSID IE */, &elen);
        if (ie && elen <= 32) {
            memcpy(ssid, ie, elen);
            has_ssid = true;
        }
    }

    TickType_t now = xTaskGetTickCount();
    ap_entry_t *e = table_find_or_add(src, now);
    bool is_new = (e->frames == 0);
    if (has_ssid || (is_new && !has_ssid)) {
        strlcpy(e->ssid, has_ssid ? ssid : "", sizeof(e->ssid));
    }
    e->channel = ch;
    if (pkt->rx_ctrl.rssi > e->best_rssi) {
        e->best_rssi = pkt->rx_ctrl.rssi;
    }
    e->frames++;

    if (is_new) {
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "NEW AP %02x:%02x:%02x:%02x:%02x:%02x ch=%d rssi=%d ssid=\"%s\"",
                 src[0], src[1], src[2], src[3], src[4], src[5],
                 pkt->rx_ctrl.channel, pkt->rx_ctrl.rssi,
                 has_ssid ? ssid : "<hidden>");
        return;
    }
    portEXIT_CRITICAL(&s_lock);
}

static void hop_task(void *arg)
{
    for (;;) {
        int locked = s_locked_ch;
        if (locked >= CHAN_MIN && locked <= CHAN_MAX) {
            esp_wifi_set_channel(locked, WIFI_SECOND_CHAN_NONE);
        } else {
            static uint8_t ch = CHAN_MIN;
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            ch = (ch == CHAN_MAX) ? CHAN_MIN : ch + 1;
        }
        vTaskDelay(pdMS_TO_TICKS(HOP_INTERVAL_MS));
    }
}

static void report_task(void *arg)
{
    static ap_entry_t snap[AP_TABLE_MAX];
    int snap_count;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SUMMARY_PERIOD_S * 1000));
        ESP_LOGI(TAG, "==== summary: total=%lu, APs=%d ====",
                 (unsigned long)s_total_pkts, s_ap_count);

        printf("CH:");
        for (int c = CHAN_MIN; c <= CHAN_MAX; c++) {
            printf(" %d=%lu", c, (unsigned long)s_chan_pkts[c]);
        }
        printf("\n");

        // 锁内只做快照拷贝，打印放锁外（UART 阻塞不能进临界区）
        portENTER_CRITICAL(&s_lock);
        snap_count = s_ap_count;
        memcpy(snap, s_aps, sizeof(ap_entry_t) * snap_count);
        portEXIT_CRITICAL(&s_lock);

        printf("%-17s %-3s %-5s %-8s %s\n", "BSSID", "CH", "RSSI", "FRAMES", "SSID");
        for (int i = 0; i < snap_count; i++) {
            char mac[18];
            mac_to_str(snap[i].bssid, mac);
            printf("%-17s %-3d %-5d %-8lu %s\n",
                   mac, snap[i].channel, snap[i].best_rssi,
                   (unsigned long)snap[i].frames,
                   snap[i].ssid[0] ? snap[i].ssid : "<hidden>");
        }
        fflush(stdout);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 模式来源：NVS 标志（串口 'p'/'q' 切换）或启动时按住 BOOT(GPIO0)
    uint8_t saved = 0;
    nvs_handle_t h;
    if (nvs_open("sniff", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, NVS_MODE_KEY, &saved);
        nvs_close(h);
    }
    gpio_reset_pin(BOOT_BTN_GPIO);
    gpio_set_direction(BOOT_BTN_GPIO, GPIO_MODE_INPUT);
    gpio_pullup_en(BOOT_BTN_GPIO);
    vTaskDelay(pdMS_TO_TICKS(30));
    s_pcap_mode = (saved == 1) || (gpio_get_level(BOOT_BTN_GPIO) == 0);

    if (s_pcap_mode) {
        esp_log_level_set("*", ESP_LOG_NONE); // 二进制流中不能混入日志文本
        setvbuf(stdout, NULL, _IONBF, 0);
        pcap_emit_global_header();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT | WIFI_PROMIS_FILTER_MASK_DATA,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(sniffer_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

    xTaskCreate(stdin_poll_task, "stdin", 3072, NULL, 3, NULL);
    xTaskCreate(report_task, "report", 6144, NULL, 4, &s_report_task);

    if (!s_pcap_mode) {
        ESP_LOGI(TAG, "WiFi sniffer started (log mode). Send 'p' to switch to PCAP mode.");
        ESP_LOGI(TAG, "Hopping ch%d-ch%d every %dms", CHAN_MIN, CHAN_MAX, HOP_INTERVAL_MS);
    } else {
        vTaskSuspend(s_report_task);   // PCAP 启动时停汇总打印，'q' 可恢复
    }

    xTaskCreate(hop_task, "hop", 2048, NULL, 5, NULL);
}
