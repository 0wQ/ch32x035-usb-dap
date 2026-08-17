#include "bsp/bsp_delay.h"
#include "dap_main.h"

#include <ch32x035.h>

void cherrydap_port_init(void);
void cherrydap_port_process(void);

static void __attribute__((section(".highcode"), noinline, optimize("O2"))) cherrydap_process(void) {
    cherrydap_port_process();
    chry_dap_handle();
    chry_dap_usb2uart_handle();
}

int main(void) {
    SystemInit();
    bsp_delay_init();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    cherrydap_port_init();
    chry_dap_init(0u, 0u);
    for (;;) {
        cherrydap_process();
    }
}
