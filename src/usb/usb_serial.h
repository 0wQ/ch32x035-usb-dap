#pragma once

#include <stdbool.h>
#include <stdint.h>

#define USB_SERIAL_PORT_COUNT 2u

typedef struct {
    uint32_t baud_rate;
    uint8_t stop_bits;
    uint8_t parity;
    uint8_t data_bits;
} usb_serial_line_config_t;

void usb_serial_init(void);
bool usb_serial_is_open(uint8_t port);
uint16_t usb_serial_rx_available(uint8_t port);
uint16_t usb_serial_tx_free(uint8_t port);
uint16_t usb_serial_read(uint8_t port, uint8_t *data, uint16_t len);
uint16_t usb_serial_write(uint8_t port, const uint8_t *data, uint16_t len);
bool usb_serial_get_line_config(uint8_t port, usb_serial_line_config_t *config, uint32_t *version);
void usb_serial_process(void);
