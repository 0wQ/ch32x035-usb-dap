#include <ch32x035.h>

#include "dap_main.h"

void cherrydap_port_init(void);
void cherrydap_port_process(void);

int main(void)
{
    SystemInit();
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_1);
    cherrydap_port_init();
    chry_dap_init(0u, 0u);
    for (;;) {
        cherrydap_port_process();
        chry_dap_handle();
        chry_dap_usb2uart_handle();
    }
}
