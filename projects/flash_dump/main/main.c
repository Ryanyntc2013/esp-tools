#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_err.h"
#include "esp_crc.h"
#include "esp_log.h"

#define FLASH_SPI_HOST       SPI2_HOST
#define FLASH_CS_GPIO        10
#define FLASH_MOSI_GPIO      11
#define FLASH_SCLK_GPIO      12
#define FLASH_MISO_GPIO      13
#define FLASH_SIZE_20H       (256U * 1024U)
#define FLASH_SIZE_40H       (512U * 1024U)
#define FLASH_CHUNK          4096U
#define FLASH_READ_CMD       0x03U
#define FLASH_JEDEC_CMD      0x9FU
#define FLASH_STATUS_CMD     0x05U
#define FLASH_WIP_MASK       0x01U
#define FRAME_MAGIC          "P25D"
#define FRAME_TYPE_DATA      0x01U

static spi_device_handle_t s_flash;
static uint32_t s_flash_size;
static uint8_t s_data[FLASH_CHUNK];
static uint8_t s_frame[19U + FLASH_CHUNK];

static void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
}

static void put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)value;
    dst[1] = (uint8_t)(value >> 8);
    dst[2] = (uint8_t)(value >> 16);
    dst[3] = (uint8_t)(value >> 24);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    return esp_crc32_le(crc, data, (uint32_t)len);
}

static esp_err_t flash_read(uint32_t offset, uint8_t *data, size_t len)
{
    spi_transaction_t transaction = {
        .cmd = FLASH_READ_CMD,
        .addr = offset,
        .length = len * 8U,
        .rxlength = len * 8U,
        .rx_buffer = data,
    };
    return spi_device_transmit(s_flash, &transaction);
}

static esp_err_t flash_read_id(uint8_t id[3])
{
    spi_transaction_t transaction = {
        .cmd = FLASH_JEDEC_CMD,
        .length = 24,
        .rxlength = 24,
        .rx_buffer = id,
    };
    return spi_device_transmit(s_flash, &transaction);
}

static esp_err_t flash_read_status(uint8_t *status)
{
    spi_transaction_t transaction = {
        .cmd = FLASH_STATUS_CMD,
        .length = 8,
        .rxlength = 8,
        .rx_buffer = status,
    };
    return spi_device_transmit(s_flash, &transaction);
}

static esp_err_t flash_init(void)
{
    spi_bus_config_t bus_config = {
        .mosi_io_num = FLASH_MOSI_GPIO,
        .miso_io_num = FLASH_MISO_GPIO,
        .sclk_io_num = FLASH_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FLASH_CHUNK,
    };
    spi_device_interface_config_t device_config = {
        .clock_speed_hz = 10 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = FLASH_CS_GPIO,
        .queue_size = 1,
        .command_bits = 8,
        .address_bits = 24,
    };

    esp_err_t err = spi_bus_initialize(FLASH_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }
    return spi_bus_add_device(FLASH_SPI_HOST, &device_config, &s_flash);
}

static bool read_line(char *line, size_t capacity)
{
    if (fgets(line, capacity, stdin) == NULL) {
        clearerr(stdin);
        vTaskDelay(pdMS_TO_TICKS(10));
        return false;
    }
    line[strcspn(line, "\r\n")] = '\0';
    return true;
}

static bool parse_get(const char *line, uint32_t *seq, uint32_t *offset, uint32_t *length)
{
    unsigned long parsed_seq;
    unsigned long parsed_offset;
    unsigned int parsed_length;
    char extra;

    if (sscanf(line, "GET %lu %lu %u %c", &parsed_seq, &parsed_offset, &parsed_length, &extra) != 3) {
        return false;
    }
    *seq = (uint32_t)parsed_seq;
    *offset = (uint32_t)parsed_offset;
    *length = parsed_length;
    return true;
}

static bool parse_seq_command(const char *line, const char *command, uint32_t *seq)
{
    unsigned long parsed_seq;
    char extra;
    size_t command_len = strlen(command);

    if (strncmp(line, command, command_len) != 0 ||
        sscanf(line + command_len, " %lu %c", &parsed_seq, &extra) != 1) {
        return false;
    }
    *seq = (uint32_t)parsed_seq;
    return true;
}

static esp_err_t send_data_frame(uint32_t seq, uint32_t offset, uint16_t length)
{
    const size_t header_len = 15U;
    const size_t crc_data_len = 11U + length;
    const size_t frame_len = 19U + length;

    memcpy(s_frame, FRAME_MAGIC, 4);
    s_frame[4] = FRAME_TYPE_DATA;
    put_u32_le(s_frame + 5, seq);
    put_u32_le(s_frame + 9, offset);
    put_u16_le(s_frame + 13, length);
    memcpy(s_frame + header_len, s_data, length);
    put_u32_le(s_frame + header_len + length,
               crc32_update(0, s_frame + 4, crc_data_len));

    if (fwrite(s_frame, 1, frame_len, stdout) != frame_len) {
        return ESP_FAIL;
    }
    fflush(stdout);
    return ESP_OK;
}

static void protocol_error(const char *message)
{
    printf("P25E ERROR %s\r\n", message);
    fflush(stdout);
}

void app_main(void)
{
    uint8_t id[3] = {0};
    uint8_t status = 0;
    char line[96];
    uint32_t next_offset = 0;
    uint32_t expected_seq = 0;
    uint32_t image_crc = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    usb_serial_jtag_driver_config_t usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    usb_config.tx_buffer_size = 8192;
    usb_config.rx_buffer_size = 512;
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
    usb_serial_jtag_vfs_use_driver();
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
    esp_log_level_set("*", ESP_LOG_NONE);
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_ERROR_CHECK(flash_init());
    ESP_ERROR_CHECK(flash_read_id(id));
    if (id[0] != 0x85 || id[1] != 0x60 || (id[2] != 0x12 && id[2] != 0x13)) {
        printf("P25D ERROR unexpected-id=%02X-%02X-%02X\r\n", id[0], id[1], id[2]);
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    s_flash_size = (id[2] == 0x13) ? FLASH_SIZE_40H : FLASH_SIZE_20H;

    for (int attempt = 0; attempt < 100; attempt++) {
        ESP_ERROR_CHECK(flash_read_status(&status));
        if ((status & FLASH_WIP_MASK) == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if ((status & FLASH_WIP_MASK) != 0) {
        protocol_error("flash-busy");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

        printf("P25D READY id=%02X-%02X-%02X size=%lu chunk=%u\r\n",
            id[0], id[1], id[2], (unsigned long)s_flash_size, FLASH_CHUNK);

    while (!read_line(line, sizeof(line)) || strcmp(line, "GO") != 0) {
    }

        printf("P25D BEGIN size=%lu chunk=%u\r\n",
            (unsigned long)s_flash_size, FLASH_CHUNK);

        while (next_offset < s_flash_size) {
        uint32_t seq;
        uint32_t offset;
        uint32_t length;

        if (!read_line(line, sizeof(line))) {
            continue;
        }
        if (strcmp(line, "ABORT") == 0) {
            protocol_error("aborted");
            return;
        }
        if (!parse_get(line, &seq, &offset, &length) ||
            seq != expected_seq || offset != next_offset ||
            length == 0 || length > FLASH_CHUNK || offset + length > s_flash_size) {
            protocol_error("invalid-get");
            continue;
        }

        ESP_ERROR_CHECK(flash_read(offset, s_data, length));
        ESP_ERROR_CHECK(send_data_frame(seq, offset, (uint16_t)length));

        for (;;) {
            uint32_t ack_seq;
            if (!read_line(line, sizeof(line))) {
                continue;
            }
            if (parse_seq_command(line, "ACK", &ack_seq) && ack_seq == seq) {
                image_crc = crc32_update(image_crc, s_data, length);
                next_offset += length;
                expected_seq++;
                break;
            }
            if (parse_seq_command(line, "NACK", &ack_seq) && ack_seq == seq) {
                break;
            }
            if (strcmp(line, "ABORT") == 0) {
                protocol_error("aborted");
                return;
            }
            protocol_error("invalid-ack");
        }
    }

    while (read_line(line, sizeof(line))) {
        if (strcmp(line, "FINISH") == 0) {
                 printf("P25D END size=%lu crc32=%08lX\r\n",
                     (unsigned long)s_flash_size, (unsigned long)image_crc);
            break;
        }
        if (strcmp(line, "ABORT") == 0) {
            protocol_error("aborted");
            return;
        }
        protocol_error("expected-finish");
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
