#include "uart_bridge.h"

#include "usb/usb_serial.h"

#include <stdbool.h>
#include <stdint.h>

#include <ch32x035.h>
#include <ch32x035_dma.h>

#define UART_BRIDGE_PORT_COUNT       USB_SERIAL_PORT_COUNT
#define UART_BRIDGE_TX_RING_SIZE     512u
#define UART_BRIDGE_TX_RING_MASK     (UART_BRIDGE_TX_RING_SIZE - 1u)
#define UART_BRIDGE_RX_DMA_RING_SIZE 2048u
#define UART_BRIDGE_RX_DMA_RING_MASK (UART_BRIDGE_RX_DMA_RING_SIZE - 1u)
#define UART_BRIDGE_CHUNK_SIZE       256u
#define UART_BRIDGE_MAX_BAUD         3000000u

_Static_assert(UART_BRIDGE_PORT_COUNT == 2u, "UART bridge pin map provides two ports");
_Static_assert((UART_BRIDGE_TX_RING_SIZE & UART_BRIDGE_TX_RING_MASK) == 0u, "UART TX ring size must be a power of two");
_Static_assert((UART_BRIDGE_RX_DMA_RING_SIZE & UART_BRIDGE_RX_DMA_RING_MASK) == 0u, "UART RX DMA ring size must be a power of two");

typedef struct {
    USART_TypeDef *usart;
    volatile uint16_t tx_head;
    volatile uint16_t tx_tail;
    volatile uint32_t rx_head;
    volatile uint32_t rx_tail;
    volatile uint32_t rx_dma_overruns;
    volatile uint32_t rx_usart_errors;
    volatile uint32_t rx_dma_wraps;
    volatile uint32_t rx_dma_wraps_total;
    volatile uint32_t rx_dma_errors;
    uint32_t rx_dma_absolute_head;
    DMA_Channel_TypeDef *rx_dma_channel;
    DMA_Channel_TypeDef *tx_dma_channel;
    volatile uint16_t tx_dma_len;
    volatile bool tx_dma_busy;
    volatile bool tx_dma_complete_pending;
    volatile uint32_t tx_dma_errors;
    uint32_t line_version;
    usb_serial_line_config_t active_line_config;
    uint8_t tx_ring[UART_BRIDGE_TX_RING_SIZE];
    uint8_t rx_dma_ring[UART_BRIDGE_RX_DMA_RING_SIZE] __attribute__((aligned(4)));
} uart_bridge_port_t;

static uart_bridge_port_t s_uart_ports[UART_BRIDGE_PORT_COUNT] = {
    {.usart = USART2, .rx_dma_channel = DMA1_Channel6, .tx_dma_channel = DMA1_Channel7},
    {.usart = USART3, .rx_dma_channel = DMA1_Channel3, .tx_dma_channel = DMA1_Channel2},
};

static uint16_t uart_ring_used(uint16_t head, uint16_t tail) {
    return (uint16_t)(head - tail);
}

static uint16_t uart_tx_free(const uart_bridge_port_t *port) {
    return (uint16_t)(UART_BRIDGE_TX_RING_SIZE - uart_ring_used(port->tx_head, port->tx_tail));
}

static bool uart_line_config_supported(const usb_serial_line_config_t *config) {
    if (config->baud_rate < 300u || config->baud_rate > UART_BRIDGE_MAX_BAUD) {
        return false;
    }
    if (config->stop_bits > 2u || config->parity > 2u) {
        return false;
    }
    if (config->data_bits == 8u) {
        return true;
    }
    return config->data_bits == 7u && config->parity != 0u;
}

static bool uart_line_config_equal(const usb_serial_line_config_t *left, const usb_serial_line_config_t *right) {
    return left->baud_rate == right->baud_rate &&
           left->stop_bits == right->stop_bits &&
           left->parity == right->parity &&
           left->data_bits == right->data_bits;
}

static uint32_t uart_dma_flags(const uart_bridge_port_t *port) {
    return port->rx_dma_channel == DMA1_Channel6
               ? (DMA1_IT_GL6 | DMA1_IT_TC6 | DMA1_IT_HT6 | DMA1_IT_TE6)
               : (DMA1_IT_GL3 | DMA1_IT_TC3 | DMA1_IT_HT3 | DMA1_IT_TE3);
}

static uint32_t uart_dma_tx_flags(const uart_bridge_port_t *port) {
    return port->tx_dma_channel == DMA1_Channel7
               ? (DMA1_IT_GL7 | DMA1_IT_TC7 | DMA1_IT_HT7 | DMA1_IT_TE7)
               : (DMA1_IT_GL2 | DMA1_IT_TC2 | DMA1_IT_HT2 | DMA1_IT_TE2);
}

static void uart_dma_tx_stop(uart_bridge_port_t *port) {
    USART_DMACmd(port->usart, USART_DMAReq_Tx, DISABLE);
    DMA_Cmd(port->tx_dma_channel, DISABLE);
    DMA_ClearITPendingBit(uart_dma_tx_flags(port));
    port->tx_dma_busy = false;
    port->tx_dma_complete_pending = false;
    port->tx_dma_len = 0u;
}

static void uart_dma_tx_start(uart_bridge_port_t *port) __attribute__((section(".highcode"), noinline)) {
    DMA_InitTypeDef dma = {0};
    uint16_t head = port->tx_head;
    uint16_t tail = port->tx_tail;
    uint16_t used;
    uint16_t len;

    if (port->tx_dma_busy || port->tx_dma_complete_pending) {
        return;
    }
    used = uart_ring_used(head, tail);
    if (used == 0u) {
        return;
    }
    len = used;
    if (len > (uint16_t)(UART_BRIDGE_TX_RING_SIZE - (tail & UART_BRIDGE_TX_RING_MASK))) {
        len = (uint16_t)(UART_BRIDGE_TX_RING_SIZE - (tail & UART_BRIDGE_TX_RING_MASK));
    }

    DMA_DeInit(port->tx_dma_channel);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&port->usart->DATAR;
    dma.DMA_MemoryBaseAddr = (uint32_t)&port->tx_ring[tail & UART_BRIDGE_TX_RING_MASK];
    dma.DMA_DIR = DMA_DIR_PeripheralDST;
    dma.DMA_BufferSize = len;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Normal;
    dma.DMA_Priority = DMA_Priority_VeryHigh;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(port->tx_dma_channel, &dma);
    DMA_ClearITPendingBit(uart_dma_tx_flags(port));
    DMA_ITConfig(port->tx_dma_channel, DMA_IT_TC | DMA_IT_TE, ENABLE);
    port->tx_dma_len = len;
    port->tx_dma_busy = true;
    DMA_Cmd(port->tx_dma_channel, ENABLE);
    USART_DMACmd(port->usart, USART_DMAReq_Tx, ENABLE);
}

static void uart_dma_rx_stop(uart_bridge_port_t *port) {
    USART_DMACmd(port->usart, USART_DMAReq_Rx, DISABLE);
    DMA_Cmd(port->rx_dma_channel, DISABLE);
    DMA_ClearITPendingBit(uart_dma_flags(port));
    port->rx_head = 0u;
    port->rx_tail = 0u;
    port->rx_dma_wraps = 0u;
    port->rx_dma_errors = 0u;
    port->rx_dma_absolute_head = 0u;
}

static void uart_dma_rx_start(uart_bridge_port_t *port) {
    DMA_InitTypeDef dma = {0};

    DMA_DeInit(port->rx_dma_channel);
    dma.DMA_PeripheralBaseAddr = (uint32_t)&port->usart->DATAR;
    dma.DMA_MemoryBaseAddr = (uint32_t)port->rx_dma_ring;
    dma.DMA_DIR = DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize = UART_BRIDGE_RX_DMA_RING_SIZE;
    dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode = DMA_Mode_Circular;
    dma.DMA_Priority = DMA_Priority_VeryHigh;
    dma.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(port->rx_dma_channel, &dma);
    DMA_ITConfig(port->rx_dma_channel, DMA_IT_TC | DMA_IT_TE, ENABLE);
    DMA_Cmd(port->rx_dma_channel, ENABLE);
    USART_DMACmd(port->usart, USART_DMAReq_Rx, ENABLE);
}

static void uart_dma_rx_sync(uart_bridge_port_t *port) {
    uint32_t wraps = port->rx_dma_wraps;
    uint16_t remaining = DMA_GetCurrDataCounter(port->rx_dma_channel);
    uint32_t position = (UART_BRIDGE_RX_DMA_RING_SIZE - remaining) & UART_BRIDGE_RX_DMA_RING_MASK;
    uint32_t absolute = wraps * UART_BRIDGE_RX_DMA_RING_SIZE + position;

    /* The DMA counter can wrap before its TC IRQ is serviced. */
    if (absolute < port->rx_dma_absolute_head) {
        absolute += UART_BRIDGE_RX_DMA_RING_SIZE;
    }
    if (absolute > port->rx_dma_absolute_head) {
        port->rx_dma_absolute_head = absolute;
        port->rx_head = absolute;
    }
    if (port->rx_head - port->rx_tail > UART_BRIDGE_RX_DMA_RING_SIZE) {
        port->rx_tail = port->rx_head - UART_BRIDGE_RX_DMA_RING_SIZE;
        ++port->rx_dma_overruns;
    }
}

static void uart_apply_line_config(uart_bridge_port_t *port, const usb_serial_line_config_t *config) {
    USART_InitTypeDef init = {0};

    init.USART_BaudRate = config->baud_rate;
    init.USART_StopBits = config->stop_bits == 0u   ? USART_StopBits_1
                          : config->stop_bits == 1u ? USART_StopBits_1_5
                                                    : USART_StopBits_2;
    init.USART_Parity = config->parity == 0u   ? USART_Parity_No
                        : config->parity == 1u ? USART_Parity_Odd
                                               : USART_Parity_Even;
    init.USART_WordLength = config->parity != 0u && config->data_bits == 8u
                                ? USART_WordLength_9b
                                : USART_WordLength_8b;
    init.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    init.USART_Mode = USART_Mode_Tx | USART_Mode_Rx;

    USART_Cmd(port->usart, DISABLE);
    uart_dma_tx_stop(port);
    uart_dma_rx_stop(port);
    USART_Init(port->usart, &init);
    USART_Cmd(port->usart, ENABLE);
    USART_ITConfig(port->usart, USART_IT_ERR, ENABLE);
    uart_dma_rx_start(port);
    uart_dma_tx_start(port);
    port->active_line_config = *config;
}

static uint16_t uart_tx_write(uart_bridge_port_t *port, const uint8_t *data, uint16_t len) {
    uint16_t head = port->tx_head;
    uint16_t free = (uint16_t)(UART_BRIDGE_TX_RING_SIZE - uart_ring_used(head, port->tx_tail));

    if (len > free) {
        len = free;
    }
    for (uint16_t i = 0u; i < len; ++i) {
        port->tx_ring[(head + i) & UART_BRIDGE_TX_RING_MASK] = data[i];
    }
    port->tx_head = (uint16_t)(head + len);
    uart_dma_tx_start(port);
    return len;
}

static uint16_t uart_rx_write_to_usb(uart_bridge_port_t *port, uint8_t usb_port, uint16_t len) {
    uint32_t tail = port->rx_tail;
    uint32_t used = port->rx_head - tail;
    uint16_t contiguous = (uint16_t)(UART_BRIDGE_RX_DMA_RING_SIZE - (tail & UART_BRIDGE_RX_DMA_RING_MASK));

    if (len > used) {
        len = used;
    }
    if (len > contiguous) {
        len = contiguous;
    }
    len = usb_serial_write(usb_port, &port->rx_dma_ring[tail & UART_BRIDGE_RX_DMA_RING_MASK], len);
    port->rx_tail = tail + len;
    return len;
}

static void uart_bridge_irq(uart_bridge_port_t *port) {
    uint16_t status = port->usart->STATR;
    uint16_t errors = status & (USART_STATR_ORE | USART_STATR_NE | USART_STATR_FE | USART_STATR_PE);

    if (errors != 0u) {
        (void)port->usart->DATAR;
        ++port->rx_usart_errors;
    }
}

void USART2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USART3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

void USART2_IRQHandler(void) {
    uart_bridge_irq(&s_uart_ports[0]);
}

void USART3_IRQHandler(void) {
    uart_bridge_irq(&s_uart_ports[1]);
}

void DMA1_Channel3_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void DMA1_Channel6_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));

void DMA1_Channel3_IRQHandler(void) {
    if (DMA_GetITStatus(DMA1_IT_TC3) != RESET) {
        ++s_uart_ports[1].rx_dma_wraps;
        ++s_uart_ports[1].rx_dma_wraps_total;
    }
    if (DMA_GetITStatus(DMA1_IT_TE3) != RESET) {
        ++s_uart_ports[1].rx_dma_errors;
    }
    DMA_ClearITPendingBit(DMA1_IT_GL3);
}

void DMA1_Channel6_IRQHandler(void) {
    if (DMA_GetITStatus(DMA1_IT_TC6) != RESET) {
        ++s_uart_ports[0].rx_dma_wraps;
        ++s_uart_ports[0].rx_dma_wraps_total;
    }
    if (DMA_GetITStatus(DMA1_IT_TE6) != RESET) {
        ++s_uart_ports[0].rx_dma_errors;
    }
    DMA_ClearITPendingBit(DMA1_IT_GL6);
}

static void uart_dma_tx_irq(uart_bridge_port_t *port, uint32_t flags) {
    uint32_t tc_flag = port->tx_dma_channel == DMA1_Channel7 ? DMA1_IT_TC7 : DMA1_IT_TC2;
    uint32_t te_flag = port->tx_dma_channel == DMA1_Channel7 ? DMA1_IT_TE7 : DMA1_IT_TE2;
    uint32_t pending = DMA1->INTFR;

    DMA_ClearITPendingBit(flags);
    if ((pending & tc_flag) != 0u) {
        if (port->tx_dma_busy) {
            port->tx_dma_busy = false;
            DMA_Cmd(port->tx_dma_channel, DISABLE);
            USART_DMACmd(port->usart, USART_DMAReq_Tx, DISABLE);
            port->tx_dma_complete_pending = true;
        }
    }
    if ((pending & te_flag) != 0u) {
        ++port->tx_dma_errors;
        DMA_Cmd(port->tx_dma_channel, DISABLE);
        USART_DMACmd(port->usart, USART_DMAReq_Tx, DISABLE);
        port->tx_dma_len = 0u;
        port->tx_dma_busy = false;
    }
}

void DMA1_Channel2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast"), section(".highcode"), noinline));
void DMA1_Channel7_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast"), section(".highcode"), noinline));

void DMA1_Channel2_IRQHandler(void) {
    uart_dma_tx_irq(&s_uart_ports[1], DMA1_IT_GL2 | DMA1_IT_TC2 | DMA1_IT_HT2 | DMA1_IT_TE2);
}

void DMA1_Channel7_IRQHandler(void) {
    uart_dma_tx_irq(&s_uart_ports[0], DMA1_IT_GL7 | DMA1_IT_TC7 | DMA1_IT_HT7 | DMA1_IT_TE7);
}

void uart_bridge_init(void) {
    GPIO_InitTypeDef gpio = {0};
    NVIC_InitTypeDef nvic = {0};
    usb_serial_line_config_t config = {
        .baud_rate = 115200u,
        .stop_bits = 0u,
        .parity = 0u,
        .data_bits = 8u,
    };

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2 | RCC_APB1Periph_USART3, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);

    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Pin = GPIO_Pin_2;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_3;
    GPIO_Init(GPIOB, &gpio);

    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    gpio.GPIO_Pin = GPIO_Pin_3;
    GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_4;
    GPIO_Init(GPIOB, &gpio);

    nvic.NVIC_IRQChannelPreemptionPriority = 0u;
    nvic.NVIC_IRQChannelSubPriority = 2u;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    nvic.NVIC_IRQChannel = USART2_IRQn;
    NVIC_Init(&nvic);
    nvic.NVIC_IRQChannelSubPriority = 3u;
    nvic.NVIC_IRQChannel = USART3_IRQn;
    NVIC_Init(&nvic);
    nvic.NVIC_IRQChannel = DMA1_Channel6_IRQn;
    nvic.NVIC_IRQChannelSubPriority = 0u;
    NVIC_Init(&nvic);
    nvic.NVIC_IRQChannel = DMA1_Channel3_IRQn;
    nvic.NVIC_IRQChannelSubPriority = 1u;
    NVIC_Init(&nvic);
    nvic.NVIC_IRQChannelPreemptionPriority = 1u;
    nvic.NVIC_IRQChannel = DMA1_Channel7_IRQn;
    nvic.NVIC_IRQChannelSubPriority = 1u;
    NVIC_Init(&nvic);
    nvic.NVIC_IRQChannel = DMA1_Channel2_IRQn;
    nvic.NVIC_IRQChannelSubPriority = 2u;
    NVIC_Init(&nvic);

    uart_apply_line_config(&s_uart_ports[0], &config);
    uart_apply_line_config(&s_uart_ports[1], &config);
}

void uart_bridge_process(void) __attribute__((section(".highcode"), noinline)) {
    uint8_t chunk[UART_BRIDGE_PORT_COUNT][UART_BRIDGE_CHUNK_SIZE];

    for (uint8_t i = 0u; i < UART_BRIDGE_PORT_COUNT; ++i) {
        uart_bridge_port_t *port = &s_uart_ports[i];
        usb_serial_line_config_t config;
        uint32_t version;
        uint16_t count;
        uint16_t tx_free;

        if (usb_serial_get_line_config(i, &config, &version) && version != port->line_version) {
            if (uart_line_config_supported(&config) &&
                !uart_line_config_equal(&config, &port->active_line_config)) {
                uart_apply_line_config(port, &config);
            }
            port->line_version = version;
        }

        if (port->tx_dma_complete_pending) {
            uint16_t completed_len = port->tx_dma_len;

            port->tx_dma_complete_pending = false;
            port->tx_dma_len = 0u;
            port->tx_tail = (uint16_t)(port->tx_tail + completed_len);
            uart_dma_tx_start(port);
        }

        count = usb_serial_rx_available(i);
        if (count > UART_BRIDGE_CHUNK_SIZE) {
            count = UART_BRIDGE_CHUNK_SIZE;
        }
        tx_free = uart_tx_free(port);
        if (count > tx_free) {
            count = tx_free;
        }
        if (count != 0u) {
            count = usb_serial_read(i, chunk[i], count);
            (void)uart_tx_write(port, chunk[i], count);
        }

        uart_dma_rx_sync(port);
        count = (uint16_t)(port->rx_head - port->rx_tail);
        if (count > UART_BRIDGE_CHUNK_SIZE) {
            count = UART_BRIDGE_CHUNK_SIZE;
        }
        if (count > usb_serial_tx_free(i)) {
            count = usb_serial_tx_free(i);
        }
        if (count != 0u) {
            (void)uart_rx_write_to_usb(port, i, count);
        }
    }
}
