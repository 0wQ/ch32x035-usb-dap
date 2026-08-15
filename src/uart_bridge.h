#pragma once

#include <stdbool.h>
#include <stdint.h>

void uart_bridge_init(void);
void uart_bridge_process(void);
void uart_bridge_control_line_changed(uint8_t port, bool dtr, bool rts);
