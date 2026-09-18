#include "bsp/bsp_delay.h"

#include <stdbool.h>

#include <ch32x035.h>

#define SYSTICK_CTLR (*(volatile uint32_t *)0xE000F000UL)
#define SYSTICK_SR   (*(volatile uint32_t *)0xE000F004UL)
#define SYSTICK_CNTL (*(volatile uint32_t *)0xE000F008UL)
#define SYSTICK_CNTH (*(volatile uint32_t *)0xE000F00CUL)
#define SYSTICK_CMPL (*(volatile uint32_t *)0xE000F010UL)
#define SYSTICK_CMPH (*(volatile uint32_t *)0xE000F014UL)

#define SYSTICK_CTLR_STCLK_HCLK (1UL << 2U)
#define SYSTICK_CTLR_ENABLE     (1UL << 0U)

static bool delay_initialized;
static uint32_t systick_ticks_per_us;
static uint32_t systick_ticks_per_ms;

static void systick_read_counter(uint32_t *high, uint32_t *low) {
    uint32_t high_before;
    uint32_t high_after;

    do {
        high_before = SYSTICK_CNTH;
        *low = SYSTICK_CNTL;
        high_after = SYSTICK_CNTH;
    } while (high_before != high_after);

    *high = high_after;
}

// 只计算 64 位计数值除以 divisor 后的低 32 位，避免 RV32 上的软件 64 位除法
static uint32_t systick_counter_to_units(uint32_t high, uint32_t low, uint32_t divisor) {
    uint32_t remainder = high % divisor;
    uint32_t result = 0u;

    for (uint32_t mask = 0x80000000u; mask != 0u; mask >>= 1u) {
        uint32_t next = (remainder << 1u) | ((low & mask) != 0u ? 1u : 0u);
        if (next >= divisor) {
            next -= divisor;
            result |= mask;
        }
        remainder = next;
    }
    return result;
}

static void systick_wait_ticks(uint32_t ticks) {
    uint32_t start = SYSTICK_CNTL;

    while ((uint32_t)(SYSTICK_CNTL - start) < ticks) {
    }
}

static void delay_units(uint32_t units, uint32_t ticks_per_unit) {
    if (!delay_initialized || units == 0u || ticks_per_unit == 0u) {
        return;
    }

    const uint32_t max_units = UINT32_MAX / ticks_per_unit;
    while (units != 0u) {
        const uint32_t chunk = units > max_units ? max_units : units;
        systick_wait_ticks(chunk * ticks_per_unit);
        units -= chunk;
    }
}

void bsp_delay_init(void) {
    if (delay_initialized) {
        return;
    }

    SystemCoreClockUpdate();
    SYSTICK_CTLR = 0u;
    SYSTICK_SR = 0u;
    SYSTICK_CNTL = 0u;
    SYSTICK_CNTH = 0u;
    SYSTICK_CMPL = UINT32_MAX;
    SYSTICK_CMPH = UINT32_MAX;

    systick_ticks_per_us = SystemCoreClock / 1000000u;
    systick_ticks_per_ms = SystemCoreClock / 1000u;
    if (systick_ticks_per_us == 0u || systick_ticks_per_ms == 0u) {
        return;
    }
    SYSTICK_CTLR = SYSTICK_CTLR_ENABLE | SYSTICK_CTLR_STCLK_HCLK;
    delay_initialized = true;
}

void bsp_delay_us(uint32_t us) {
    delay_units(us, systick_ticks_per_us);
}

void bsp_delay_ms(uint32_t ms) {
    delay_units(ms, systick_ticks_per_ms);
}

uint32_t bsp_time_us(void) {
    uint32_t high;
    uint32_t low;

    if (!delay_initialized) {
        return 0u;
    }
    systick_read_counter(&high, &low);
    return systick_counter_to_units(high, low, systick_ticks_per_us);
}

uint32_t bsp_time_ms(void) {
    uint32_t high;
    uint32_t low;

    if (!delay_initialized) {
        return 0u;
    }
    systick_read_counter(&high, &low);
    return systick_counter_to_units(high, low, systick_ticks_per_ms);
}
