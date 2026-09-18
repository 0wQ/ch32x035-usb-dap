#include "bsp/bsp_delay.h"
#include "bsp/bsp_system.h"
#include "dap_main.h"
#include "drv/drv_button.h"
#include "drv/drv_dp_pullup.h"
#include "drv/drv_power_switch.h"
#include "drv/drv_sbu_mux.h"
#include "drv/drv_ws2816c.h"
#include "status/status_led.h"

#include <ch32x035.h>

void cherrydap_port_init(void);
void cherrydap_port_process(void);

static void __attribute__((section(".highcode"), noinline, optimize("O2"))) cherrydap_process(void) {
    cherrydap_port_process();
    chry_dap_handle();
    chry_dap_usb2uart_handle();
    status_led_process();
}

#define BUTTON_DEBOUNCE_MS      20u
#define BUTTON_SCAN_INTERVAL_MS 1u

static void process_button(void) {
    static bool sampled_pressed;
    static uint32_t sample_changed_at;
    static uint32_t last_scan_ms;
    uint32_t now_ms = bsp_time_ms();

    // 限制 GPIO 采样频率，等待下次采样期间继续处理 DAP
    if ((uint32_t)(now_ms - last_scan_ms) < BUTTON_SCAN_INTERVAL_MS) {
        return;
    }
    last_scan_ms = now_ms;
    bool pressed = drv_button_is_pressed();

    if (pressed != sampled_pressed) {
        sampled_pressed = pressed;
        sample_changed_at = now_ms;
        return;
    }

    // 按下稳定 20 ms 后复位进入 ISP
    if (pressed && (uint32_t)(now_ms - sample_changed_at) >= BUTTON_DEBOUNCE_MS) {
        bsp_system_enter_isp();
    }
}

int main(void) {
    SystemInit();
    bsp_delay_init();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);

    drv_dp_pullup_init();
    drv_power_switch_init();
    drv_sbu_mux_init();
    bsp_delay_ms(10u);
    drv_power_switch_set_enabled(true);

    drv_ws2816c_init();
    status_led_init();

    drv_button_init();

    cherrydap_port_init();
    chry_dap_init(0u, 0u);

    for (;;) {
        process_button();
        cherrydap_process();
    }
}
