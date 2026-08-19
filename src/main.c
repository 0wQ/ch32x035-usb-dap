#include "bsp/bsp_delay.h"
#include "drv/drv_dp_pullup.h"
#include "drv/drv_power_switch.h"
#include "drv/drv_uart_mux.h"
#include "wchlink/wchlink_usb.h"

#include <system_ch32x035.h>

int main(void) {
    SystemInit();
    drv_dp_pullup_init();
    drv_power_switch_init();
    drv_uart_mux_init();
    bsp_delay_init();
    bsp_delay_ms(10u);
    drv_power_switch_set_enabled(true);
    wchlink_usb_init();
    for (;;) {
        wchlink_usb_process();
    }
}
