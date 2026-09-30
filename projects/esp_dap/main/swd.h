#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PIN_SWDIO  4
#define PIN_SWCLK  5
#define PIN_nRESET 6
// UART for CDC (透传到目标串口)
#define PIN_UART_TX 17
#define PIN_UART_RX 18

void swd_init(void);
void swd_deinit(void);

// 线级操作 (bit-bang)
void swd_write_bit(int bit);
int  swd_read_bit(void);
void swd_write_bits(uint32_t v, int n);   // LSB first
uint32_t swd_read_bits(int n);

// SWD 协议
// request: 0x81.. 含 APnDP/RnW/A2A3, data: 写时为32bit, 读时忽略
// return ACK: 1=OK 2=WAIT 4=FAULT
uint8_t swd_transfer(uint8_t request, uint32_t *data);
void    swd_line_reset(void);             // 50+ clocks with SWDIO=1
void    swd_jtag_to_swd(void);            // JTAG-to-SWD 切换序列
void    swd_sequence(uint32_t cnt, const uint8_t *data, bool out, uint8_t *ack_bits);

// Pins & Reset
void swd_set_pins(uint8_t pin_out, uint8_t pin_select, uint8_t pin_wait_us);
void swd_set_reset(bool assert_nrst);    // true=拉低复位
bool swd_get_reset(void);
void swd_set_config(uint8_t turnaround, bool data_phase);

void swd_delay_us(uint32_t us);
