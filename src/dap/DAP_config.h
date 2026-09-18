#pragma once

#include <stdint.h>

#include <ch32x035.h>

#include "status/status_led.h"

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
#define DAP_JTAG_DEV_CNT        1U
#define DAP_DEFAULT_PORT        1U
#define DAP_DEFAULT_SWJ_CLOCK   1000000U
#define DAP_PACKET_SIZE         64U
#define DAP_PACKET_COUNT        8U
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

__STATIC_INLINE uint8_t DAP_GetVendorString(char *str) {
    (void)str;
    return 0U;
}
__STATIC_INLINE uint8_t DAP_GetProductString(char *str) {
    (void)str;
    return 0U;
}
__STATIC_INLINE uint8_t DAP_GetSerNumString(char *str) {
    (void)str;
    return 0U;
}
__STATIC_INLINE uint8_t DAP_GetTargetDeviceVendorString(char *str) {
    (void)str;
    return 0U;
}
__STATIC_INLINE uint8_t DAP_GetTargetDeviceNameString(char *str) {
    (void)str;
    return 0U;
}
__STATIC_INLINE uint8_t DAP_GetTargetBoardVendorString(char *str) {
    (void)str;
    return 0U;
}
__STATIC_INLINE uint8_t DAP_GetTargetBoardNameString(char *str) {
    (void)str;
    return 0U;
}
__STATIC_INLINE uint8_t DAP_GetProductFirmwareVersionString(char *str) {
    (void)str;
    return 0U;
}

// PA2 为 SWCLK，PA3 为 SWDIO，本板没有 nRESET
// BSHR 低 16 位将输出置高，BCR 将输出拉低
__STATIC_FORCEINLINE uint32_t PIN_SWCLK_TCK_IN(void) { return (GPIOA->INDR & GPIO_Pin_2) != 0U; }
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_SET(void) { GPIOA->BSHR = GPIO_Pin_2; }
__STATIC_FORCEINLINE void PIN_SWCLK_TCK_CLR(void) { GPIOA->BCR = GPIO_Pin_2; }
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_TMS_IN(void) { return (GPIOA->INDR & GPIO_Pin_3) != 0U; }
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_SET(void) { GPIOA->BSHR = GPIO_Pin_3; }
__STATIC_FORCEINLINE void PIN_SWDIO_TMS_CLR(void) { GPIOA->BCR = GPIO_Pin_3; }
__STATIC_FORCEINLINE uint32_t PIN_SWDIO_IN(void) { return (GPIOA->INDR & GPIO_Pin_3) != 0U; }
__STATIC_FORCEINLINE void PIN_SWDIO_OUT(uint32_t bit) {
    if ((bit & 1U) != 0U)
        GPIOA->BSHR = GPIO_Pin_3;
    else
        GPIOA->BCR = GPIO_Pin_3;
}
// 设置 SWDIO 数据，并将 SWCLK 原子拉低，为下一个 SWD 数据位准备
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_SWCLK_CLR(uint32_t bit) {
    // BSHR 高 16 位复位对应引脚，本操作同时置高 PA3 并拉低 PA2
    if ((bit & 1U) != 0U)
        GPIOA->BSHR = GPIO_Pin_3 | (GPIO_Pin_2 << 16);
    else
        GPIOA->BCR = GPIO_Pin_3 | GPIO_Pin_2;
}
// CFGLR 为 PA0 至 PA7 每个引脚分配 4 位，0x1 为推挽输出，0x4 为浮空输入
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_ENABLE(void) {
    // PA3 的 12 至 15 位，从浮空输入切换为推挽输出
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFU << 12)) | (0x1U << 12);
}
__STATIC_FORCEINLINE void PIN_SWDIO_OUT_DISABLE(void) {
    // PA3 的 12 至 15 位，释放 SWDIO 供目标驱动
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xFU << 12)) | (0x4U << 12);
}
__STATIC_FORCEINLINE uint32_t PIN_TDI_IN(void) { return 0U; }
__STATIC_FORCEINLINE void PIN_TDI_OUT(uint32_t bit) { (void)bit; }
__STATIC_FORCEINLINE uint32_t PIN_TDO_IN(void) { return 0U; }
__STATIC_FORCEINLINE uint32_t PIN_nTRST_IN(void) { return 1U; }
__STATIC_FORCEINLINE void PIN_nTRST_OUT(uint32_t bit) { (void)bit; }
// 本板没有 nRESET 引脚，用电平影子回读最后一次写入的值
// SWJ_Pins 在 wait 模式下靠读回值匹配请求值退出自旋，返回固定电平会让主机请求取反时死锁
extern uint32_t pin_nreset_shadow;
__STATIC_FORCEINLINE uint32_t PIN_nRESET_IN(void) { return pin_nreset_shadow; }
__STATIC_FORCEINLINE void PIN_nRESET_OUT(uint32_t bit) { pin_nreset_shadow = bit & 1U; }

__STATIC_INLINE void PORT_JTAG_SETUP(void) {}
__STATIC_INLINE void PORT_SWD_SETUP(void) {
    // PA2 为 SWCLK 输出，PA3 为 SWDIO 输出（读时由传输函数切输入）
    // 每个 PA0 至 PA7 的 CFGLR 字段占用 4 位，PA2 在 8 至 11 位，PA3 在 12 至 15 位
    GPIOA->BSHR = GPIO_Pin_2 | GPIO_Pin_3;
    GPIOA->CFGLR = (GPIOA->CFGLR & ~((0xFU << 8) | (0xFU << 12))) |
                   (0x1U << 8) | (0x1U << 12);
}
__STATIC_INLINE void PORT_OFF(void) {
    // DAP 端口关闭时，仅将 PA2、PA3 释放为浮空输入
    GPIOA->CFGLR = (GPIOA->CFGLR & ~((0xFU << 8) | (0xFU << 12))) |
                   (0x4U << 8) | (0x4U << 12);
}
// DAP_HostStatus 在主循环上下文调用，此处只写状态，出帧由 status_led_process 统一处理
__STATIC_INLINE void LED_CONNECTED_OUT(uint32_t bit) { status_led_set_connected(bit != 0u); }
__STATIC_INLINE void LED_RUNNING_OUT(uint32_t bit) { status_led_set_running(bit != 0u); }
__STATIC_INLINE uint32_t TIMESTAMP_GET(void) { return 0U; }
__STATIC_INLINE void DAP_SETUP(void) {
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    PORT_OFF();
}
// 本板没有 nRESET 引脚，返回 0 表示复位未执行，主机据此回退到 SWD 软复位
__STATIC_INLINE uint8_t RESET_TARGET(void) {
    return 0U;
}
