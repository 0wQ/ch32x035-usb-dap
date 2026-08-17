#include "dap_main.h"

#include <ch32x035.h>
#include <ch32x035_dma.h>
#include <ch32x035_tim.h>

// 半传输边界为 256 B，避免读取 DMA 正在写入的数据
#define CHERRYDAP_UART_RX_DMA_SIZE      512U
#define CHERRYDAP_UART_RX_DMA_HALF_SIZE (CHERRYDAP_UART_RX_DMA_SIZE / 2U)
#define CHERRYDAP_UART_RX_DMA_MASK      (CHERRYDAP_UART_RX_DMA_SIZE - 1U)
#define CHERRYDAP_UART_RX_IDLE_FLUSH_US 1000U

static uint8_t cherrydap_uart_rx_dma_buffer[CHERRYDAP_UART_RX_DMA_SIZE] __attribute__((aligned(4)));
static volatile uint32_t cherrydap_uart_rx_dma_wraps;
static volatile uint32_t cherrydap_uart_rx_dma_completed;
static uint32_t cherrydap_uart_rx_dma_read;
static uint16_t cherrydap_uart_rx_dma_last_remaining;
static uint16_t cherrydap_uart_rx_dma_last_activity;
static volatile uint16_t cherrydap_uart_tx_dma_len;
static volatile uint8_t cherrydap_uart_tx_dma_busy;

static void cherrydap_uart_tx_dma_stop(void) {
    USART_DMACmd(USART2, USART_DMAReq_Tx, DISABLE);
    DMA_Cmd(DMA1_Channel7, DISABLE);
    DMA_ClearITPendingBit(DMA1_IT_GL7);
    cherrydap_uart_tx_dma_len = 0U;
    cherrydap_uart_tx_dma_busy = 0U;
}

static void cherrydap_uart_rx_dma_stop(void) {
    USART_DMACmd(USART2, USART_DMAReq_Rx, DISABLE);
    DMA_Cmd(DMA1_Channel6, DISABLE);
    DMA_ClearITPendingBit(DMA1_IT_GL6);
    cherrydap_uart_rx_dma_wraps = 0U;
    cherrydap_uart_rx_dma_completed = 0U;
    cherrydap_uart_rx_dma_read = 0U;
    cherrydap_uart_rx_dma_last_remaining = CHERRYDAP_UART_RX_DMA_SIZE;
    cherrydap_uart_rx_dma_last_activity = TIM_GetCounter(TIM2);
}

static void cherrydap_uart_rx_dma_start(void) {
    DMA_InitTypeDef dma = {0};

    DMA_DeInit(DMA1_Channel6);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&USART2->DATAR;
    dma.DMA_MemoryBaseAddr = (uint32_t)cherrydap_uart_rx_dma_buffer;
    dma.DMA_DIR = DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize = CHERRYDAP_UART_RX_DMA_SIZE;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Circular;
    dma.DMA_Priority = DMA_Priority_VeryHigh;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel6, &dma);
    DMA_ClearITPendingBit(DMA1_IT_GL6);
    DMA_ITConfig(DMA1_Channel6, DMA_IT_HT | DMA_IT_TC, ENABLE);
    DMA_Cmd(DMA1_Channel6, ENABLE);
    USART_DMACmd(USART2, USART_DMAReq_Rx, ENABLE);
    cherrydap_uart_rx_dma_last_remaining = DMA_GetCurrDataCounter(DMA1_Channel6);
    cherrydap_uart_rx_dma_last_activity = TIM_GetCounter(TIM2);
}

static void cherrydap_uart_rx_dma_drain(void) {
    uint16_t remaining = DMA_GetCurrDataCounter(DMA1_Channel6);
    uint16_t now = TIM_GetCounter(TIM2);
    uint32_t head = cherrydap_uart_rx_dma_completed;

    if (remaining != cherrydap_uart_rx_dma_last_remaining) {
        cherrydap_uart_rx_dma_last_remaining = remaining;
        cherrydap_uart_rx_dma_last_activity = now;
    } else if ((uint16_t)(now - cherrydap_uart_rx_dma_last_activity) >= CHERRYDAP_UART_RX_IDLE_FLUSH_US &&
               remaining != CHERRYDAP_UART_RX_DMA_SIZE) {
        uint32_t wraps = cherrydap_uart_rx_dma_wraps;
        uint32_t position = (CHERRYDAP_UART_RX_DMA_SIZE - remaining) & CHERRYDAP_UART_RX_DMA_MASK;
        uint32_t idle_head = wraps * CHERRYDAP_UART_RX_DMA_SIZE + position;

        if (idle_head < head) {
            idle_head += CHERRYDAP_UART_RX_DMA_SIZE;
        }
        if (idle_head > head) {
            cherrydap_uart_rx_dma_completed = idle_head;
            head = idle_head;
        }
    }

    while (cherrydap_uart_rx_dma_read != head) {
        uint32_t len = head - cherrydap_uart_rx_dma_read;
        uint32_t free = chry_ringbuffer_get_free(&g_uartrx);
        uint32_t contiguous = CHERRYDAP_UART_RX_DMA_SIZE -
                              (cherrydap_uart_rx_dma_read & CHERRYDAP_UART_RX_DMA_MASK);

        if (free == 0U) {
            break;
        }
        if (len > contiguous) {
            len = contiguous;
        }
        if (len > free) {
            len = free;
        }

        // CDC 发送队列满时保留 DMA 读指针，等待主循环继续排空
        len = chry_ringbuffer_write(&g_uartrx,
                                    &cherrydap_uart_rx_dma_buffer[cherrydap_uart_rx_dma_read & CHERRYDAP_UART_RX_DMA_MASK],
                                    len);
        cherrydap_uart_rx_dma_read += len;
        if (len == 0U) {
            break;
        }
    }
}

static void cherrydap_uart_tx_dma_start(uint8_t *data, uint16_t len) {
    DMA_InitTypeDef dma = {0};

    if (data == NULL || len == 0U || cherrydap_uart_tx_dma_busy != 0U) {
        return;
    }

    DMA_DeInit(DMA1_Channel7);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&USART2->DATAR;
    dma.DMA_MemoryBaseAddr = (uint32_t)data;
    dma.DMA_DIR = DMA_DIR_PeripheralDST;
    dma.DMA_BufferSize = len;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Normal;
    dma.DMA_Priority = DMA_Priority_VeryHigh;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel7, &dma);
    DMA_ClearITPendingBit(DMA1_IT_GL7);
    DMA_ITConfig(DMA1_Channel7, DMA_IT_TC | DMA_IT_TE, ENABLE);
    cherrydap_uart_tx_dma_len = len;
    cherrydap_uart_tx_dma_busy = 1U;
    DMA_Cmd(DMA1_Channel7, ENABLE);
    USART_DMACmd(USART2, USART_DMAReq_Tx, ENABLE);
}

static void cherrydap_uart_apply(const struct cdc_line_coding *line_coding) {
    USART_InitTypeDef init = {0};

    cherrydap_uart_tx_dma_stop();
    cherrydap_uart_rx_dma_stop();
    init.USART_BaudRate = line_coding->dwDTERate != 0U ? line_coding->dwDTERate : 115200U;
    init.USART_WordLength = line_coding->bDataBits == 9U ? USART_WordLength_9b : USART_WordLength_8b;
    init.USART_StopBits = line_coding->bCharFormat == 2U ? USART_StopBits_2 : USART_StopBits_1;
    init.USART_Parity = line_coding->bParityType == 1U ? USART_Parity_Odd : line_coding->bParityType == 2U ? USART_Parity_Even
                                                                                                           : USART_Parity_No;
    init.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    init.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART2, &init);
    USART_Cmd(USART2, ENABLE);
    USART_ITConfig(USART2, USART_IT_ERR, ENABLE);
    cherrydap_uart_rx_dma_start();
}

void cherrydap_port_init(void) {
    GPIO_InitTypeDef gpio = {0};
    NVIC_InitTypeDef nvic = {0};
    struct cdc_line_coding line_coding = {
        .dwDTERate = 115200U,
        .bCharFormat = 0U,
        .bParityType = 0U,
        .bDataBits = 8U,
    };

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    gpio.GPIO_Pin = GPIO_Pin_2;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_3;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOA, &gpio);

    nvic.NVIC_IRQChannel = DMA1_Channel6_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1U;
    nvic.NVIC_IRQChannelSubPriority = 0U;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);
    nvic.NVIC_IRQChannel = DMA1_Channel7_IRQn;
    NVIC_Init(&nvic);
    nvic.NVIC_IRQChannel = USART2_IRQn;
    NVIC_Init(&nvic);

    TIM_TimeBaseInitTypeDef timer = {
        .TIM_Prescaler = (uint16_t)(SystemCoreClock / 1000000U - 1U),
        .TIM_CounterMode = TIM_CounterMode_Up,
        .TIM_Period = 0xFFFFU,
        .TIM_ClockDivision = TIM_CKD_DIV1,
    };

    TIM_TimeBaseInit(TIM2, &timer);
    TIM_Cmd(TIM2, ENABLE);
    cherrydap_uart_apply(&line_coding);
}

void cherrydap_port_process(void) {
    cherrydap_uart_rx_dma_drain();
}

void DMA1_Channel6_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast"), section(".highcode"), noinline));
void DMA1_Channel6_IRQHandler(void) {
    uint32_t pending = DMA1->INTFR;

    DMA_ClearITPendingBit(DMA1_IT_GL6);
    if ((pending & DMA1_IT_HT6) != 0U) {
        cherrydap_uart_rx_dma_completed =
            cherrydap_uart_rx_dma_wraps * CHERRYDAP_UART_RX_DMA_SIZE + CHERRYDAP_UART_RX_DMA_HALF_SIZE;
    }
    if ((pending & DMA1_IT_TC6) != 0U) {
        cherrydap_uart_rx_dma_wraps++;
        cherrydap_uart_rx_dma_completed = cherrydap_uart_rx_dma_wraps * CHERRYDAP_UART_RX_DMA_SIZE;
    }
}

void USART2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast"), section(".highcode"), noinline));
void USART2_IRQHandler(void) {
    uint16_t status = USART2->STATR;
    uint16_t errors = status & (USART_STATR_ORE | USART_STATR_NE | USART_STATR_FE | USART_STATR_PE);

    if (errors != 0U) {
        (void)USART2->DATAR;
    }
}

void DMA1_Channel7_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast"), section(".highcode"), noinline));
void DMA1_Channel7_IRQHandler(void) {
    uint32_t pending = DMA1->INTFR;

    DMA_ClearITPendingBit(DMA1_IT_GL7);
    if ((pending & DMA1_IT_TC7) != 0U) {
        uint16_t completed = cherrydap_uart_tx_dma_len;
        uint16_t next_len;
        uint8_t *next_data;

        DMA_Cmd(DMA1_Channel7, DISABLE);
        USART_DMACmd(USART2, USART_DMAReq_Tx, DISABLE);
        cherrydap_uart_tx_dma_len = 0U;
        cherrydap_uart_tx_dma_busy = 0U;
        if (chry_dap_usb2uart_uart_take_next(completed, &next_data, &next_len)) {
            cherrydap_uart_tx_dma_start(next_data, next_len);
        }
    } else if ((pending & DMA1_IT_TE7) != 0U) {
        DMA_Cmd(DMA1_Channel7, DISABLE);
        USART_DMACmd(USART2, USART_DMAReq_Tx, DISABLE);
        cherrydap_uart_tx_dma_len = 0U;
        cherrydap_uart_tx_dma_busy = 0U;
    }
}

void chry_dap_usb2uart_uart_config_callback(struct cdc_line_coding *line_coding) {
    if (line_coding != NULL) {
        cherrydap_uart_apply(line_coding);
    }
}

void chry_dap_usb2uart_uart_send_bydma(uint8_t *data, uint16_t len) {
    cherrydap_uart_tx_dma_start(data, len);
}
