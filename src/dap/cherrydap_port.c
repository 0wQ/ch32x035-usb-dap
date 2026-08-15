#include "dap_main.h"

#include <ch32x035.h>

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

