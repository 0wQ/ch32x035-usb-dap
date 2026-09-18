#pragma once
#include <stdbool.h>

void status_led_init(void);
// 主循环唯一出帧点，内部限帧并对重复内容去重
void status_led_process(void);
// 由 DAP_HostStatus 钩子在主循环上下文调用，只记录状态，不碰硬件
void status_led_set_connected(bool connected);
void status_led_set_running(bool running);
