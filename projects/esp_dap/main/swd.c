#include "swd.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "swd";
static uint8_t s_turnaround = 1;
static bool s_data_phase = false;

static inline void swd_delay_half(void) {
    // ~0.5us @240MHz: esp_rom_delay_us 最小1us, 用NOP近似 600kHz~2MHz 可调
    // 用 80 个NOP ~ 0.3us
    // 这里用1us做保守，后续可通过 menuconfig 调 SCLK
    esp_rom_delay_us(1);
}

void swd_init(void) {
    gpio_config_t io = {};
    // SWCLK 始终输出
    io.pin_bit_mask = 1ULL << PIN_SWCLK;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = 0;
    io.pull_down_en = 0;
    gpio_config(&io);
    gpio_set_level(PIN_SWCLK, 0);

    // SWDIO 初始输出高
    io.pin_bit_mask = 1ULL << PIN_SWDIO;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    gpio_set_level(PIN_SWDIO, 1);

    // nRESET 输出高(不复位), 开漏风格用推挽+外部上拉亦可
    io.pin_bit_mask = 1ULL << PIN_nRESET;
    io.mode = GPIO_MODE_OUTPUT_OD; // 开漏，释放为高阻
    gpio_config(&io);
    gpio_set_level(PIN_nRESET, 1); // 释放

    ESP_LOGI(TAG, "SWDIO=%d SWCLK=%d nRST=%d turnaround=%d", PIN_SWDIO, PIN_SWCLK, PIN_nRESET, s_turnaround);
}

void swd_deinit(void) {
    gpio_reset_pin(PIN_SWCLK);
    gpio_reset_pin(PIN_SWDIO);
    gpio_reset_pin(PIN_nRESET);
}

static inline void dio_out(bool v) { gpio_set_level(PIN_SWDIO, v); }
static inline void clk_high(void) { gpio_set_level(PIN_SWCLK, 1); swd_delay_half(); }
static inline void clk_low(void)  { gpio_set_level(PIN_SWCLK, 0); swd_delay_half(); }

static void dio_set_output(void) {
    gpio_set_direction(PIN_SWDIO, GPIO_MODE_OUTPUT);
}
static void dio_set_input(void) {
    gpio_set_direction(PIN_SWDIO, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_SWDIO, GPIO_PULLUP_ONLY);
}

void swd_write_bit(int bit) {
    dio_out(bit ? 1 : 0);
    clk_high();
    clk_low();
}

int swd_read_bit(void) {
    clk_high();
    int v = gpio_get_level(PIN_SWDIO);
    clk_low();
    return v;
}

void swd_write_bits(uint32_t v, int n) {
    for (int i = 0; i < n; i++) {
        swd_write_bit((v >> i) & 1);
    }
}

uint32_t swd_read_bits(int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        int b = swd_read_bit();
        if (b) v |= (1u << i);
    }
    return v;
}

void swd_delay_us(uint32_t us) { esp_rom_delay_us(us); }

void swd_set_config(uint8_t turnaround, bool data_phase) {
    s_turnaround = turnaround ? turnaround : 1;
    if (s_turnaround > 4) s_turnaround = 4;
    s_data_phase = data_phase;
    ESP_LOGI(TAG, "cfg turnaround=%d data_phase=%d", s_turnaround, data_phase);
}

void swd_line_reset(void) {
    dio_set_output();
    dio_out(1);
    for (int i = 0; i < 64; i++) { clk_high(); clk_low(); }
}

void swd_jtag_to_swd(void) {
    // 16bit JTAG-to-SWD sequence 0xE79E LSB first
    dio_set_output();
    uint16_t seq = 0xE79E;
    for (int i = 0; i < 16; i++) {
        dio_out((seq >> i) & 1);
        clk_high(); clk_low();
    }
    // line reset already does 50+ clocks high, done elsewhere
}

uint8_t swd_transfer(uint8_t request, uint32_t *data) {
    // request bits: [0]start 1, [1]APnDP, [2]RnW, [3]A2, [4]A3, [5]parity, [6]stop 0, [7]park 1
    dio_set_output();
    swd_write_bits(request, 8);

    // turnaround host->target
    dio_set_input();
    for (int i = 0; i < s_turnaround; i++) { clk_high(); clk_low(); }

    uint32_t ack = swd_read_bits(3); // ACK LSB first
    uint8_t ack_val = ack & 0x07;
    if (ack_val != 1) { // not OK
        // turnaround target->host + idle
        for (int i = 0; i < s_turnaround; i++) { clk_high(); clk_low(); }
        dio_set_output();
        dio_out(0);
        clk_high(); clk_low(); // idle
        return ack_val;
    }

    if ((request >> 2) & 1) { // RnW=1 read
        uint32_t v = swd_read_bits(32);
        uint32_t par = swd_read_bit();
        // parity check
        uint32_t p = 0; for (int i=0;i<32;i++) p ^= (v>>i)&1;
        if (p != par) {
            ack_val = 0x07; // mark fault on parity
        } else if (data) *data = v;

        // turnaround + idle
        for (int i = 0; i < s_turnaround; i++) { clk_high(); clk_low(); }
        dio_set_output();
        dio_out(0);
        clk_high(); clk_low();
    } else { // write
        // turnaround target->host already done, now host->target
        for (int i = 0; i < s_turnaround; i++) { clk_high(); clk_low(); }
        dio_set_output();
        uint32_t v = data ? *data : 0;
        swd_write_bits(v, 32);
        uint32_t p = 0; for (int i=0;i<32;i++) p ^= (v>>i)&1;
        swd_write_bit(p);
        dio_out(0);
        clk_high(); clk_low(); // idle? keep low
    }
    // optional data_phase idle cycles
    if (s_data_phase) {
        for (int i=0;i<4;i++){ clk_high(); clk_low(); }
    }
    return ack_val;
}

void swd_sequence(uint32_t cnt, const uint8_t *data, bool out, uint8_t *in) {
    if (out) {
        dio_set_output();
        for (uint32_t i=0;i<cnt;i++) {
            int byte = i >> 3, bit = i & 7;
            int b = (data[byte] >> bit) & 1;
            dio_out(b);
            clk_high(); clk_low();
        }
    } else {
        dio_set_input();
        // optionally capture (for SWD_Sequence with read)
        for (uint32_t i=0;i<cnt;i++) {
            clk_high();
            int b = gpio_get_level(PIN_SWDIO);
            clk_low();
            if (in) {
                if (b) in[i>>3] |= (1u << (i&7));
                else   in[i>>3] &= ~(1u << (i&7));
            }
        }
    }
}

void swd_set_pins(uint8_t pin_out, uint8_t pin_select, uint8_t pin_wait_us) {
    // DAP_SWD_Pins: pin_out bits 0=SWCLK 1=SWDIO 2=nRESET
    // pin_select bits 0=SWCLK 1=SWDIO 2=nRESET (1=改变该脚)
    // pin_wait_us: 操作后延时
    if (pin_select & (1<<0)) { gpio_set_level(PIN_SWCLK, (pin_out>>0)&1); }
    if (pin_select & (1<<1)) {
        if (pin_select & (1<<1)) { // always set dio
            dio_set_output();
            dio_out((pin_out>>1)&1);
        }
    }
    if (pin_select & (1<<2)) {
        // nRESET active low, pin_out bit2=1 => 释放, 0=>拉低
        if ((pin_out>>2)&1) gpio_set_level(PIN_nRESET, 1);
        else gpio_set_level(PIN_nRESET, 0);
    }
    if (pin_wait_us) esp_rom_delay_us(pin_wait_us);
}

void swd_set_reset(bool assert_nrst) {
    if (assert_nrst) gpio_set_level(PIN_nRESET, 0);
    else gpio_set_level(PIN_nRESET, 1);
}
bool swd_get_reset(void) { return gpio_get_level(PIN_nRESET); }
