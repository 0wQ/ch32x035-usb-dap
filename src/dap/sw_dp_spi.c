/*
 * X035 SPI1-assisted SWD transport.
 *
 * SPI1 clocks the eight-bit SWD request and 32-bit write payload. GPIO handles
 * turnaround, ACK, read payloads, and parity so PA5/PA7 remain the only SWD
 * pins required.
 */

#include "DAP_config.h"
#include "DAP.h"
#include "ch32x035_spi.h"

#define SWD_SPI_ENABLE       0U
#define GPIO_CFG_OUT_PP      0x1U
#define GPIO_CFG_AF_PP       0x9U
#define GPIO_CFG_INPUT_FLOAT 0x4U
#define SWD_SPI_CODE

static uint16_t swd_spi_prescaler;
static uint8_t swd_spi_active;
static uint8_t swd_spi_clock_enabled;

static void swd_set_gpio_clock_delay(uint32_t clock)
{
    uint32_t delay;

    if (clock >= (CPU_CLOCK / (2U * IO_PORT_WRITE_CYCLES))) {
        DAP_Data.fast_clock = 1U;
        DAP_Data.clock_delay = 1U;
        return;
    }

    DAP_Data.fast_clock = 0U;
    delay = ((CPU_CLOCK / 2U) + (clock - 1U)) / clock;
    if (delay > IO_PORT_WRITE_CYCLES) {
        delay -= IO_PORT_WRITE_CYCLES;
        delay = (delay + (DELAY_SLOW_CYCLES - 1U)) / DELAY_SLOW_CYCLES;
    } else {
        delay = 1U;
    }
    DAP_Data.clock_delay = delay;
}

/* Override the weak CherryDAP clock hook. SPI divisors are selected so that
 * the generated data clock never exceeds the host's requested clock. */
void Set_Clock_Delay(uint32_t clock)
{
    if (clock >= 24000000U) {
        swd_spi_prescaler = SPI_BaudRatePrescaler_2;
        swd_spi_active = 1U;
        swd_set_gpio_clock_delay(24000000U);
    } else if (clock >= 12000000U) {
        swd_spi_prescaler = SPI_BaudRatePrescaler_4;
        swd_spi_active = 1U;
        swd_set_gpio_clock_delay(12000000U);
    } else if (clock >= 6000000U) {
        swd_spi_prescaler = SPI_BaudRatePrescaler_8;
        swd_spi_active = 1U;
        swd_set_gpio_clock_delay(6000000U);
    } else if (clock >= 3000000U) {
        swd_spi_prescaler = SPI_BaudRatePrescaler_16;
        swd_spi_active = 1U;
        swd_set_gpio_clock_delay(3000000U);
    } else if (clock >= 1500000U) {
        swd_spi_prescaler = SPI_BaudRatePrescaler_32;
        swd_spi_active = 1U;
        swd_set_gpio_clock_delay(1500000U);
    } else if (clock >= 750000U) {
        swd_spi_prescaler = SPI_BaudRatePrescaler_64;
        swd_spi_active = 1U;
        swd_set_gpio_clock_delay(750000U);
    } else if (clock >= 375000U) {
        swd_spi_prescaler = SPI_BaudRatePrescaler_128;
        swd_spi_active = 1U;
        swd_set_gpio_clock_delay(375000U);
    } else if (clock >= 187500U) {
        swd_spi_prescaler = SPI_BaudRatePrescaler_256;
        swd_spi_active = 1U;
        swd_set_gpio_clock_delay(187500U);
    } else {
        swd_spi_active = 0U;
        swd_set_gpio_clock_delay(clock);
    }
}

uint8_t SWD_SPI_Active(void)
{
    return SWD_SPI_ENABLE != 0U && swd_spi_active;
}

__STATIC_FORCEINLINE void swd_gpio_delay(void)
{
    if (DAP_Data.fast_clock != 0U) {
        PIN_DELAY_FAST();
    } else {
        PIN_DELAY_SLOW(DAP_Data.clock_delay);
    }
}

__STATIC_FORCEINLINE void swd_gpio_clock(void)
{
    PIN_SWCLK_TCK_CLR();
    swd_gpio_delay();
    PIN_SWCLK_TCK_SET();
    swd_gpio_delay();
}

__STATIC_FORCEINLINE void swd_gpio_write_bit(uint32_t bit)
{
    PIN_SWDIO_OUT(bit);
    swd_gpio_clock();
}

__STATIC_FORCEINLINE uint32_t swd_gpio_read_bit(void)
{
    uint32_t bit;

    PIN_SWCLK_TCK_CLR();
    swd_gpio_delay();
    bit = PIN_SWDIO_IN();
    PIN_SWCLK_TCK_SET();
    swd_gpio_delay();
    return bit;
}

static SWD_SPI_CODE void swd_gpio_mode(uint32_t swdio_output, uint32_t swdio_level)
{
    uint32_t config;

    SPI1->CTLR1 &= (uint16_t)~SPI_CTLR1_SPE;
    GPIOA->BCR = GPIO_Pin_5;
    if (swdio_level != 0U) {
        GPIOA->BSHR = GPIO_Pin_7;
    } else {
        GPIOA->BCR = GPIO_Pin_7;
    }

    config = GPIOA->CFGLR;
    config &= ~((0xFU << 20) | (0xFU << 28));
    config |= GPIO_CFG_OUT_PP << 20;
    config |= (swdio_output != 0U ? GPIO_CFG_OUT_PP : GPIO_CFG_INPUT_FLOAT) << 28;
    GPIOA->CFGLR = config;
}

static SWD_SPI_CODE void swd_spi_pin_mode(void)
{
    uint32_t config = GPIOA->CFGLR;

    config &= ~((0xFU << 20) | (0xFU << 28));
    config |= (GPIO_CFG_AF_PP << 20) | (GPIO_CFG_AF_PP << 28);
    GPIOA->CFGLR = config;
}

static SWD_SPI_CODE void swd_spi_begin(uint32_t data_size)
{
    if (swd_spi_clock_enabled == 0U) {
        RCC_APB2PeriphClockCmd(RCC_APB2Periph_SPI1, ENABLE);
        swd_spi_clock_enabled = 1U;
    }

    SPI1->CTLR1 = 0U;
    SPI1->CTLR2 = 0U;
    GPIOA->BCR = GPIO_Pin_5;
    swd_spi_pin_mode();
    SPI1->CTLR1 = SPI_Mode_Master | SPI_NSS_Soft | SPI_NSSInternalSoft_Set |
                  SPI_FirstBit_LSB | SPI_CPOL_Low | SPI_CPHA_1Edge |
                  SPI_Direction_1Line_Tx | swd_spi_prescaler | data_size;
    SPI1->CTLR1 |= SPI_CTLR1_SPE;
}

static SWD_SPI_CODE void swd_spi_wait_idle(void)
{
    while ((SPI1->STATR & SPI_STATR_BSY) != 0U) {}
}

static SWD_SPI_CODE void swd_spi_tx8(uint8_t value)
{
    swd_spi_begin(SPI_DataSize_8b);
    SPI1->DATAR = value;
    swd_spi_wait_idle();
    if ((SPI1->STATR & SPI_STATR_RXNE) != 0U) {
        (void)SPI1->DATAR;
    }
}

static SWD_SPI_CODE void swd_spi_tx32(uint32_t value)
{
    swd_spi_begin(SPI_DataSize_16b);
    SPI1->DATAR = (uint16_t)value;
    while ((SPI1->STATR & SPI_STATR_TXE) == 0U) {}
    SPI1->DATAR = (uint16_t)(value >> 16);
    swd_spi_wait_idle();
}

static SWD_SPI_CODE uint32_t swd_parity32(uint32_t value)
{
    value ^= value >> 16;
    value ^= value >> 8;
    value ^= value >> 4;
    value ^= value >> 2;
    value ^= value >> 1;
    return value & 1U;
}

static SWD_SPI_CODE uint32_t swd_request_byte(uint32_t request)
{
    uint32_t header = 0x81U | ((request & 0x0FU) << 1);

    return header | (swd_parity32(request & 0x0FU) << 5);
}

static SWD_SPI_CODE uint32_t swd_read_ack(void)
{
    uint32_t ack;

    for (uint32_t n = DAP_Data.swd_conf.turnaround; n != 0U; --n) {
        swd_gpio_clock();
    }
    ack = swd_gpio_read_bit();
    ack |= swd_gpio_read_bit() << 1;
    ack |= swd_gpio_read_bit() << 2;
    return ack;
}

static SWD_SPI_CODE void swd_idle_cycles(void)
{
    uint32_t n = DAP_Data.transfer.idle_cycles;

    if (n != 0U) {
        PIN_SWDIO_TMS_CLR();
        while (n-- != 0U) {
            swd_gpio_clock();
        }
    }
    PIN_SWDIO_TMS_SET();
}

uint8_t SWD_Transfer_SPI(uint32_t request, uint32_t *data)
{
    uint32_t ack;
    uint32_t value;

    swd_spi_tx8((uint8_t)swd_request_byte(request));
    swd_gpio_mode(0U, 1U);
    ack = swd_read_ack();

    if (ack == DAP_TRANSFER_OK) {
        if ((request & DAP_TRANSFER_RnW) != 0U) {
            value = 0U;
            for (uint32_t n = 0U; n < 32U; ++n) {
                value |= swd_gpio_read_bit() << n;
            }
            if ((swd_parity32(value) ^ swd_gpio_read_bit()) != 0U) {
                ack = DAP_TRANSFER_ERROR;
            }
            if (data != NULL) {
                *data = value;
            }
            for (uint32_t n = DAP_Data.swd_conf.turnaround; n != 0U; --n) {
                swd_gpio_clock();
            }
            swd_gpio_mode(1U, 1U);
        } else {
            for (uint32_t n = DAP_Data.swd_conf.turnaround; n != 0U; --n) {
                swd_gpio_clock();
            }
            value = *data;
            swd_spi_tx32(value);
            swd_gpio_mode(1U, 1U);
            swd_gpio_write_bit(swd_parity32(value));
        }
        if ((request & DAP_TRANSFER_TIMESTAMP) != 0U) {
            DAP_Data.timestamp = TIMESTAMP_GET();
        }
        swd_idle_cycles();
        return (uint8_t)ack;
    }

    if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {
        if (DAP_Data.swd_conf.data_phase != 0U && (request & DAP_TRANSFER_RnW) != 0U) {
            for (uint32_t n = 33U; n != 0U; --n) {
                swd_gpio_clock();
            }
        }
        for (uint32_t n = DAP_Data.swd_conf.turnaround; n != 0U; --n) {
            swd_gpio_clock();
        }
        swd_gpio_mode(1U, 0U);
        if (DAP_Data.swd_conf.data_phase != 0U && (request & DAP_TRANSFER_RnW) == 0U) {
            for (uint32_t n = 33U; n != 0U; --n) {
                swd_gpio_write_bit(0U);
            }
        }
        PIN_SWDIO_TMS_SET();
        return (uint8_t)ack;
    }

    for (uint32_t n = DAP_Data.swd_conf.turnaround + 33U; n != 0U; --n) {
        swd_gpio_clock();
    }
    swd_gpio_mode(1U, 1U);
    return (uint8_t)ack;
}
