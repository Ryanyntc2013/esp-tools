#pragma once
#include <stdint.h>
#include <stddef.h>

// 处理一条 64B HID 报告，生成 64B 响应
// req[64] -> resp[64]，返回响应有效长度 (固定64)
int dap_process(const uint8_t *req, uint8_t *resp);

void dap_init(void);
