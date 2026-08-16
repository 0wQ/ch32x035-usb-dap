#include "dap_main.h"

#include <ch32x035.h>
#include <string.h>

#define CHERRYDAP_BOS_MAX_SIZE 64U

extern struct usb_bos_descriptor bos_desc;

static uint8_t bos_without_landing_page[CHERRYDAP_BOS_MAX_SIZE];

static void cherrydap_disable_webusb_landing_page(void)
{
    static const uint8_t webusb_platform_uuid[16] = {
        0x38, 0xB6, 0x08, 0x34, 0xA9, 0x09, 0xA0, 0x47,
        0x8B, 0xFD, 0xA0, 0x76, 0x88, 0x15, 0xB6, 0x65,
    };
    const uint8_t *source = bos_desc.string;
    uint16_t source_len = bos_desc.string_len;
    uint16_t source_offset = 5U;

    if (source == NULL || source_len < 5U || source_len > sizeof(bos_without_landing_page)) {
        return;
    }

    memcpy(bos_without_landing_page, source, source_len);
    while (source_offset < source_len) {
        uint8_t capability_len = source[source_offset];
        bool is_webusb;

        if (capability_len == 0U || source_offset + capability_len > source_len) {
            return;
        }

        is_webusb = capability_len >= 24U &&
                    source[source_offset + 1U] == USB_DESCRIPTOR_TYPE_DEVICE_CAPABILITY &&
                    source[source_offset + 2U] == USB_DEVICE_CAPABILITY_PLATFORM &&
                    memcmp(&source[source_offset + 4U], webusb_platform_uuid,
                           sizeof(webusb_platform_uuid)) == 0;
        if (is_webusb) {
            bos_without_landing_page[source_offset + 23U] = 0U;
            bos_desc.string = bos_without_landing_page;
            return;
        }
        source_offset += capability_len;
    }
}

static void cherrydap_uart_apply(const struct cdc_line_coding *line_coding)
{
    USART_InitTypeDef init = {0};

    init.USART_BaudRate = line_coding->dwDTERate != 0U ? line_coding->dwDTERate : 115200U;
    init.USART_WordLength = line_coding->bDataBits == 9U ? USART_WordLength_9b : USART_WordLength_8b;
    init.USART_StopBits = line_coding->bCharFormat == 2U ? USART_StopBits_2 : USART_StopBits_1;
    init.USART_Parity = line_coding->bParityType == 1U ? USART_Parity_Odd :
                        line_coding->bParityType == 2U ? USART_Parity_Even : USART_Parity_No;
    init.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    init.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART2, &init);
    USART_Cmd(USART2, ENABLE);
}

void cherrydap_port_init(void)
{
    GPIO_InitTypeDef gpio = {0};
    struct cdc_line_coding line_coding = {
        .dwDTERate = 115200U,
        .bCharFormat = 0U,
        .bParityType = 0U,
        .bDataBits = 8U,
    };

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin = GPIO_Pin_2;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_3;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);
    cherrydap_uart_apply(&line_coding);
    cherrydap_disable_webusb_landing_page();
}

void cherrydap_port_process(void)
{
    while (USART_GetFlagStatus(USART2, USART_FLAG_RXNE) != RESET) {
        uint8_t byte = (uint8_t)USART_ReceiveData(USART2);
        (void)chry_ringbuffer_write(&g_uartrx, &byte, 1U);
    }
}

void chry_dap_usb2uart_uart_config_callback(struct cdc_line_coding *line_coding)
{
    if (line_coding != NULL) {
        cherrydap_uart_apply(line_coding);
    }
}

void chry_dap_usb2uart_uart_send_bydma(uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0U; i < len; ++i) {
        while (USART_GetFlagStatus(USART2, USART_FLAG_TXE) == RESET) {}
        USART_SendData(USART2, data[i]);
    }
    chry_dap_usb2uart_uart_send_complete(len);
}
