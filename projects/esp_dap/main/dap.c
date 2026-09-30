#include "dap.h"
#include "swd.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "dap";

// DAP commands
#define ID_DAP_Info                0x00
#define ID_DAP_HostStatus          0x01
#define ID_DAP_Connect             0x02
#define ID_DAP_Disconnect          0x03
#define ID_DAP_TransferConfigure   0x04
#define ID_DAP_Transfer            0x05
#define ID_DAP_TransferBlock       0x06
#define ID_DAP_TransferAbort       0x07
#define ID_DAP_WriteABORT          0x08
#define ID_DAP_Delay               0x09
#define ID_DAP_ResetTarget         0x0A
#define ID_DAP_SWD_Pins            0x10
#define ID_DAP_SWD_Configure       0x13
#define ID_DAP_SWD_Sequence        0x1D
#define ID_DAP_JTAG_Sequence       0x14
#define ID_DAP_JTAG_Configure      0x15
#define ID_DAP_JTAG_IDCODE         0x16

#define DAP_OK    0x00
#define DAP_ERROR 0xFF

#define DAP_PORT_AUTODETECT 0
#define DAP_PORT_SWD        1
#define DAP_PORT_JTAG       2
#define DAP_PORT_DISABLED   0

// Info IDs
#define DAP_INFO_VENDOR          0x01
#define DAP_INFO_PRODUCT         0x02
#define DAP_INFO_SER_NUM         0x03
#define DAP_INFO_FW_VER          0x04
#define DAP_INFO_DEVICE_VENDOR   0x05
#define DAP_INFO_DEVICE_NAME     0x06
#define DAP_INFO_CAPS            0xF0
#define DAP_INFO_SWO_BUFFER_SIZE 0xFD
#define DAP_INFO_MAX_PACKET_COUNT 0xFE
#define DAP_INFO_MAX_PACKET_SIZE 0xFF
#define DAP_INFO_PACKET_COUNT    0xFE
#define DAP_INFO_PACKET_SIZE     0xFF

static uint8_t s_port = DAP_PORT_DISABLED;
static uint8_t s_idle_cycles = 0;
static uint16_t s_wait_retry = 100;
static uint16_t s_match_retry = 0;
static bool s_connected = false;

void dap_init(void) {
    s_port = DAP_PORT_DISABLED;
    s_connected = false;
    swd_init();
}

static int dap_info(const uint8_t *req, uint8_t *resp) {
    uint8_t id = req[1];
    resp[0] = ID_DAP_Info;
    switch (id) {
        case DAP_INFO_VENDOR:
            resp[1] = 13;
            memcpy(&resp[2], "Espressif", 10); // len 10? pad
            memcpy(&resp[2], "Espressif", 9);
            resp[1] = 9;
            return 2+9;
        case DAP_INFO_PRODUCT:
            resp[1] = 7;
            memcpy(&resp[2], "ESP-DAP", 7);
            return 2+7;
        case DAP_INFO_SER_NUM:
            resp[1] = 6;
            memcpy(&resp[2], "000001", 6);
            return 2+6;
        case DAP_INFO_FW_VER:
            resp[1] = 6;
            memcpy(&resp[2], "1.0.0", 5);
            resp[1]=5;
            return 2+5;
        case DAP_INFO_DEVICE_VENDOR:
            resp[1]=0; return 2;
        case DAP_INFO_DEVICE_NAME:
            resp[1]=0; return 2;
        case DAP_INFO_CAPS: // bit0 SWD, bit1 JTAG
            resp[1]=1; resp[2]=0x01; return 3;
        case DAP_INFO_SWO_BUFFER_SIZE:
            resp[1]=2; resp[2]=0; resp[3]=0; return 4;
        case DAP_INFO_PACKET_COUNT:
            resp[1]=1; resp[2]=1; return 3;
        case DAP_INFO_PACKET_SIZE:
            resp[1]=2; resp[2]=64; resp[3]=0; return 4;
        default:
            resp[1]=0; return 2;
    }
}

static int handle_connect(const uint8_t *req, uint8_t *resp) {
    uint8_t port = req[1];
    if (port == DAP_PORT_AUTODETECT) port = DAP_PORT_SWD;
    if (port == DAP_PORT_SWD) {
        s_port = DAP_PORT_SWD;
        swd_line_reset();
        s_connected = true;
        resp[0]=ID_DAP_Connect; resp[1]=DAP_PORT_SWD;
        return 2;
    }
    resp[0]=ID_DAP_Connect; resp[1]=DAP_PORT_DISABLED;
    return 2;
}

static int handle_disconnect(const uint8_t *req, uint8_t *resp) {
    (void)req;
    s_port = DAP_PORT_DISABLED;
    s_connected = false;
    resp[0]=ID_DAP_Disconnect; resp[1]=DAP_OK;
    return 2;
}

static uint8_t build_request(uint8_t dap_req) {
    // dap_req: bit0 APnDP, bit1 RnW, bits2-3 A2-A3, plus other flags ignored
    uint8_t ap = dap_req & 0x01;
    uint8_t rnw= (dap_req>>1)&1;
    uint8_t a2 = (dap_req>>2)&1;
    uint8_t a3 = (dap_req>>3)&1;
    uint8_t req = 0x81; // start 1, park 1 (bit7), stop already 0
    if (ap)  req |= (1<<1);
    if (rnw) req |= (1<<2);
    if (a2)  req |= (1<<3);
    if (a3)  req |= (1<<4);
    // parity
    uint8_t p = ap ^ rnw ^ a2 ^ a3;
    if (p) req |= (1<<5);
    // bit6 stop=0 already, bit7 park=1 already in 0x81 (but need ensure)
    // 0x81 = 1000 0001 -> bit7=1 park, bit0=1 start
    return req;
}

int dap_process(const uint8_t *req, uint8_t *resp) {
    if (!req || !resp) return 0;
    memset(resp, 0, 64);
    uint8_t cmd = req[0];
    switch (cmd) {
        case ID_DAP_Info: return dap_info(req, resp);
        case ID_DAP_HostStatus: {
            resp[0]=ID_DAP_HostStatus; resp[1]=DAP_OK; return 2;
        }
        case ID_DAP_Connect: return handle_connect(req, resp);
        case ID_DAP_Disconnect: return handle_disconnect(req, resp);
        case ID_DAP_TransferConfigure: {
            s_idle_cycles = req[1];
            s_wait_retry = req[2] | (req[3]<<8);
            s_match_retry = req[4] | (req[5]<<8);
            resp[0]=ID_DAP_TransferConfigure; resp[1]=DAP_OK;
            return 2;
        }
        case ID_DAP_SWD_Configure: {
            uint8_t turnaround = (req[1] & 0x03) + 1; // spec: turnaround 1..4
            bool data_phase = (req[1] & 0x04) !=0;
            swd_set_config(turnaround, data_phase);
            resp[0]=ID_DAP_SWD_Configure; resp[1]=DAP_OK;
            return 2;
        }
        case ID_DAP_SWD_Pins: {
            uint8_t out = req[1];
            uint8_t sel = req[2];
            uint8_t wait = req[3] | (req[4]<<8) | (req[5]<<24); // we use low byte only
            (void)req[5];
            swd_set_pins(out, sel, req[3]); // use first byte wait
            // read back pins
            uint8_t pins = 0;
            // SWCLK level, SWDIO level, nRESET level
            // we approximate: read current levels
            // nRESET is open-drain, read via gpio
            bool nrst = swd_get_reset();
            // for SWCLK/SWDIO we don't track exact, assume out
            pins |= (out & 0x03); // echo
            if (nrst) pins |= (1<<2);
            resp[0]=ID_DAP_SWD_Pins; resp[1]=pins; return 2;
        }
        case ID_DAP_ResetTarget: {
            // assert nRESET low 10ms then release
            swd_set_reset(true);
            swd_delay_us(10000);
            swd_set_reset(false);
            swd_delay_us(50000);
            resp[0]=ID_DAP_ResetTarget; resp[1]=DAP_OK; resp[2]=1; // 1=reset supported, executed
            return 3;
        }
        case ID_DAP_WriteABORT: {
            uint8_t idx = req[1];
            uint32_t data = req[2] | (req[3]<<8) | (req[4]<<16) | (req[5]<<24);
            // Write DP ABORT (address 0, APnDP=0, RnW=0, A2=0 A3=0)
            if (s_port == DAP_PORT_SWD) {
                uint8_t r = build_request(0x00); // DP write ABORT
                uint32_t v = data;
                swd_transfer(r, &v);
            }
            resp[0]=ID_DAP_WriteABORT; resp[1]=DAP_OK; return 2;
        }
        case ID_DAP_Delay: {
            uint16_t d = req[1] | (req[2]<<8);
            swd_delay_us(d);
            resp[0]=ID_DAP_Delay; resp[1]=DAP_OK; return 2;
        }
        case ID_DAP_SWD_Sequence: {
            uint8_t cnt = req[1];
            int off = 2;
            int resp_off = 2;
            // According spec, SWD_Sequence has multiple sequences; we iterate
            // But simple: if cnt >1, sequential parse
            resp[0]=ID_DAP_SWD_Sequence; resp[1]=DAP_OK;
            for (int s=0; s<cnt; s++) {
                if (off >= 64) break;
                uint8_t info = req[off++];
                uint8_t bits = info & 0x3F;
                if (bits==0) bits=64;
                bool out = (info & 0x80)==0; // 0=out,1=in per spec? Actually bit7=1 means DIN (target->host)
                // number of bytes = (bits+7)/8
                int bytes = (bits+7)/8;
                if (off + bytes > 64) break;
                if (out) {
                    swd_sequence(bits, &req[off], true, NULL);
                } else {
                    // input sequence: need to collect bits
                    uint8_t tmp[8]={0};
                    swd_sequence(bits, NULL, false, tmp);
                    memcpy(&resp[resp_off], tmp, bytes);
                    resp_off += bytes;
                }
                off += bytes;
            }
            return resp_off;
        }
        case ID_DAP_Transfer: {
            uint8_t idx = req[1]; (void)idx;
            uint8_t tcnt = req[2];
            int off = 3;
            resp[0]=ID_DAP_Transfer;
            resp[1]=0; // placeholder count
            resp[2]=0; // response
            int resp_pos = 3;
            uint8_t resp_ack = 1; // OK
            uint8_t executed = 0;
            for (int i=0;i<tcnt;i++) {
                if (off >= 64) { resp_ack=0x07; break; }
                uint8_t dreq = req[off++];
                uint8_t swd_req = build_request(dreq & 0x0F); // only APnDP/RnW/A2A3
                bool is_read = (dreq>>1)&1;
                bool is_ap   = dreq & 1;
                uint32_t data = 0;
                if (!is_read) {
                    if (off+4 > 64) { resp_ack=0x07; break; }
                    data = req[off] | (req[off+1]<<8) | (req[off+2]<<16) | (req[off+3]<<24);
                    off+=4;
                }
                // handle timestamp/match_mask flags ignored for now
                uint8_t ack = 1;
                if (s_port==DAP_PORT_SWD) {
                    // idle cycles before transfer
                    for (int k=0;k<s_idle_cycles;k++){ /* idle 0 clocks with SWDIO=0 */ }
                    ack = swd_transfer(swd_req, &data);
                } else {
                    ack = 4; // fault if not SWD
                }
                // retry on WAIT up to s_wait_retry
                int retry=0;
                while (ack==2 && retry < s_wait_retry) {
                    ack = swd_transfer(swd_req, &data);
                    retry++;
                }
                if (ack != 1) {
                    resp_ack = ack;
                    // still need to consume? but we already
                    executed++;
                    break;
                }
                if (is_read) {
                    if (resp_pos+4 > 64) { resp_ack=0x07; break; }
                    resp[resp_pos++] = data & 0xFF;
                    resp[resp_pos++] = (data>>8)&0xFF;
                    resp[resp_pos++] = (data>>16)&0xFF;
                    resp[resp_pos++] = (data>>24)&0xFF;
                }
                executed++;
                if (ack != 1) { resp_ack = ack; break; }
            }
            resp[1]=executed;
            resp[2]=resp_ack; // low3 ack, bit3 protocol error if needed
            if (resp_ack!=1) resp[2] |= (1<<3); // mark?
            // If simple case ack=1 we ensure bit3=0
            if (resp_ack==1) resp[2]=1;
            // post idle cycles
            return resp_pos;
        }
        case ID_DAP_TransferBlock:
        case ID_DAP_TransferAbort:
        case ID_DAP_JTAG_Sequence:
        case ID_DAP_JTAG_Configure:
        case ID_DAP_JTAG_IDCODE:
            resp[0]=cmd; resp[1]=DAP_ERROR; return 2;
        default:
            resp[0]=cmd; resp[1]=DAP_ERROR; return 2;
    }
    return 64;
}
