#pragma once

#include <stdint.h>

#include "ch32x035.h"

#ifndef __STATIC_INLINE
#define __STATIC_INLINE static inline
#endif
#ifndef __STATIC_FORCEINLINE
#define __STATIC_FORCEINLINE __attribute__((always_inline)) static inline
#endif
#ifndef __WEAK
#define __WEAK __attribute__((weak))
#endif

#define CPU_CLOCK               48000000U
#define IO_PORT_WRITE_CYCLES    2U
#define DAP_SWD                 1
#define DAP_JTAG                0
#define USE_PIOC_ACC             0
#define DAP_JTAG_DEV_CNT        1U
#define DAP_DEFAULT_PORT        1U
#define DAP_DEFAULT_SWJ_CLOCK   1000000U
#define DAP_PACKET_SIZE         64U
#define DAP_PACKET_COUNT        4U
#define SWO_UART                0
#define SWO_MANCHESTER          0
#define SWO_STREAM              0
#define SWO_BUFFER_SIZE         256U
#define TIMESTAMP_CLOCK         0U
#define DAP_UART                0
#define DAP_UART_USB_COM_PORT   1
#define DAP_UART_DRIVER         0
#define DAP_UART_RX_BUFFER_SIZE 64U
#define DAP_UART_TX_BUFFER_SIZE 64U
#define DAP_FW_VER              "0.1.0"
#define CONFIG_UARTRX_RINGBUF_SIZE 2048U
#define CONFIG_USBRX_RINGBUF_SIZE  2048U

__STATIC_INLINE uint8_t DAP_GetVendorString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetProductString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetSerNumString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetDeviceVendorString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetDeviceNameString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetBoardVendorString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetTargetBoardNameString(char *str) { (void)str; return 0U; }
__STATIC_INLINE uint8_t DAP_GetProductFirmwareVersionString(char *str) { (void)str; return 0U; }

/* PA4 = nRESET, PA5 = SWCLK, PA7 = SWDIO. PA6 is unused. */
__STATIC_FORCEINLINE uint32_t PIN_SWCLK_TCK_IN(void) { return (GPIOA->INDR & GPIO_Pin_5) != 0U; }
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_SET(void) { GPIOA->BSHR = GPIO_Pin_5; }
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_CLR(void) { GPIOA->BCR = GPIO_Pin_5; }
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_TMS_IN(void) { return (GPIOA->INDR & GPIO_Pin_7) != 0U; }
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_SET(void) { GPIOA->BSHR = GPIO_Pin_7; }
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_CLR(void) { GPIOA->BCR = GPIO_Pin_7; }
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_IN(void) { return (GPIOA->INDR & GPIO_Pin_7) != 0U; }
__STATIC_FORCEINLINE void PIN_SWDIO_OUT(uint32_t bit)
{
    if ((bit & 1U) != 0U) GPIOA->BSHR = GPIO_Pin_7;
    else GPIOA->BCR = GPIO_Pin_7;
}
/* Set SWDIO and pull SWCLK low atomically for the next SWD data bit. */
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_SWCLK_CLR(uint32_t bit)
{
    if ((bit & 1U) != 0U) GPIOA->BSHR = GPIO_Pin_7 | (GPIO_Pin_5 << 16);
    else GPIOA->BCR = GPIO_Pin_7 | GPIO_Pin_5;
}
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_ENABLE(void)
{
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFU << 28)) | (0x1U << 28);
}
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_DISABLE(void)
{
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFU << 28)) | (0x4U << 28);
}
__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void) { return 0U; }
__STATIC_FORCEINLINE void PIN_TDI_OUT(uint32_t bit) { (void)bit; }
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void) { return 0U; }
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN(void) { return 1U; }
__STATIC_FORCEINLINE void PIN_nTRST_OUT(uint32_t bit) { (void)bit; }
__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void) { return (GPIOA->INDR & GPIO_Pin_4) != 0U; }
__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit)
{
    if (bit != 0U) GPIOA->BSHR = GPIO_Pin_4;
    else GPIOA->BCR = GPIO_Pin_4;
}

__STATIC_INLINE void PORT_JTAG_SETUP(void) {}
__STATIC_INLINE void PORT_SWD_SETUP(void)
{
    GPIOA->BSHR = GPIO_Pin_4 | GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
    GPIOA->CFGLR = (GPIOA->CFGLR & ~((0xFU << 16) | (0xFU << 20) | (0xFU << 24) | (0xFU << 28))) |
                   (0x1U << 16) | (0x1U << 20) | (0x4U << 24) | (0x1U << 28);
}
__STATIC_INLINE void PORT_OFF(void)
{
    GPIOA->CFGLR = (GPIOA->CFGLR & ~((0xFU << 16) | (0xFU << 20) | (0xFU << 24) | (0xFU << 28))) |
                   (0x4U << 16) | (0x4U << 20) | (0x4U << 24) | (0x4U << 28);
}
__STATIC_INLINE void LED_CONNECTED_OUT(uint32_t bit) { (void)bit; }
__STATIC_INLINE void LED_RUNNING_OUT(uint32_t bit) { (void)bit; }
__STATIC_INLINE uint32_t TIMESTAMP_GET(void) { return 0U; }
__STATIC_INLINE void DAP_SETUP(void)
{
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    PORT_OFF();
}
__STATIC_INLINE uint8_t RESET_TARGET(void)
{
    PIN_nRESET_OUT(0U);
    for (volatile uint32_t i = 0U; i < (CPU_CLOCK / 1000U); ++i) {
        __asm__ volatile("nop");
    }
    PIN_nRESET_OUT(1U);
    return 1U;
}
