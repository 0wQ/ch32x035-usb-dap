#include "bsp/bsp_delay.h"
#include "wchlink/wchlink_usb.h"

#include <system_ch32x035.h>

int main(void) {
    SystemInit();
    bsp_delay_init();
    wchlink_usb_init();
    for (;;) {
        wchlink_usb_process();
    }
}
