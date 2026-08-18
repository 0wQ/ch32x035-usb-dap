#pragma once

#include <stdbool.h>
#include <stdint.h>

void rvswd_gpio_init(void);
void rvswd_gpio_disconnect(void);
bool rvswd_gpio_connect(void);
bool rvswd_gpio_read_dmi(uint8_t address, uint32_t *value);
bool rvswd_gpio_write_dmi(uint8_t address, uint32_t value);
bool rvswd_gpio_read_memory32(uint32_t address, uint32_t *value);
bool rvswd_gpio_write_memory32(uint32_t address, uint32_t value);
bool rvswd_gpio_write_memory(uint32_t address, const uint8_t *data, uint32_t length);
bool rvswd_gpio_write_register(uint16_t regno, uint32_t value);
bool rvswd_gpio_read_register(uint16_t regno, uint32_t *value);
bool rvswd_gpio_halt(void);
bool rvswd_gpio_execute(uint32_t entry, uint32_t mode, uint32_t address,
                        uint32_t length, uint32_t data_address, uint32_t *result);
bool rvswd_gpio_flash_erase_all(void);
uint32_t rvswd_gpio_flash_last_error(void);
bool rvswd_gpio_flash_program_page(uint32_t address, const uint8_t *data, uint32_t length);
