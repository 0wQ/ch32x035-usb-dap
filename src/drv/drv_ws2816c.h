#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t red;
    uint16_t green;
    uint16_t blue;
} drv_ws2816c_pixel_t;

// 初始化 SPI1 和 PA7，PA7 是 WS2816C 的 DIN 输出
// 发送出错后可直接重复调用以恢复
void drv_ws2816c_init(void);

// 主循环同步发送 1 至 8 个像素，包含复位时间，不可从中断调用
// 参数非法、上一帧未完成或 DMA 错误时返回 false
bool drv_ws2816c_write(const drv_ws2816c_pixel_t *pixels, size_t pixel_count);

// 编码并启动一帧后立即返回，上一帧未完成时返回 false，不等待也不拆外设
bool drv_ws2816c_write_async(const drv_ws2816c_pixel_t *pixels, size_t pixel_count);

// 主循环轮询：回收完成帧，检测 DMA 错误或超时并回到未初始化状态
void drv_ws2816c_process(void);

// 报告外设是否可用，为 false 时需重新 init
bool drv_ws2816c_ready(void);
