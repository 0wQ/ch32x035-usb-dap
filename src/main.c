#include "bsp/bsp_delay.h"
#include "drv/drv_dp_pullup.h"
#include "drv/drv_power_switch.h"
#include "drv/drv_uart_mux.h"
#include "drv/drv_ws2816c.h"
#include "dap_main.h"

#include <ch32x035.h>

void cherrydap_port_init(void);
void cherrydap_port_process(void);

static void __attribute__((section(".highcode"), noinline, optimize("O2"))) cherrydap_process(void) {
    cherrydap_port_process();
    chry_dap_handle();
    chry_dap_usb2uart_handle();
}

#define WS2816C_DIM_LEVEL 0x0100u
#define WS2816C_EFFECT_STEP_MS 100u

static void ws2816c_show_startup_effect(void) {
    static const drv_ws2816c_pixel_t colors[] = {
        {.red = WS2816C_DIM_LEVEL, .green = 0u, .blue = 0u},
        {.red = 0u, .green = WS2816C_DIM_LEVEL, .blue = 0u},
        {.red = 0u, .green = 0u, .blue = WS2816C_DIM_LEVEL},
        {.red = WS2816C_DIM_LEVEL, .green = 0u, .blue = WS2816C_DIM_LEVEL},
    };

    for (size_t index = 0u; index < sizeof(colors) / sizeof(colors[0]); ++index) {
        uint64_t deadline;

        drv_ws2816c_write(&colors[index], 1u);
        deadline = bsp_time_ms() + WS2816C_EFFECT_STEP_MS;
        // 等待颜色切换期间继续处理 USB 请求
        while (bsp_time_ms() < deadline) {
        }
    }
}

int main(void) {
    SystemInit();
    bsp_delay_init();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);

    drv_dp_pullup_init();
    drv_power_switch_init();
    drv_uart_mux_init();
    bsp_delay_init();
    bsp_delay_ms(10u);
    drv_power_switch_set_enabled(true);
    drv_ws2816c_init();
    ws2816c_show_startup_effect();

    cherrydap_port_init();
    chry_dap_init(0u, 0u);
    for (;;) {
        cherrydap_process();
    }
}
