#include "bsp/bsp_pd_phy.h"

#include <ch32x035.h>
#include <ch32x035_usbpd.h>
#include <string.h>

#define USBPD_DATA_MAX_LEN       34u
#define BSP_USBPD_CC_CMP_DEFAULT CC_CMP_45

static bsp_pd_rx_callback_t s_rx_callback = NULL;
static void *s_user_data = NULL;

static uint8_t s_rx_dma_buf[USBPD_DATA_MAX_LEN] __attribute__((aligned(4)));
static uint8_t s_tx_dma_buf[USBPD_DATA_MAX_LEN] __attribute__((aligned(4)));

static inline uint16_t usbpd_get_rx_timer_cnt(void) {
    uint32_t clk = SystemCoreClock ? SystemCoreClock : 48000000UL;
    if (clk >= 48000000UL) return UPD_TMR_RX_48M;
    if (clk >= 24000000UL) return UPD_TMR_RX_24M;
    return UPD_TMR_RX_12M;
}

static inline uint16_t usbpd_get_tx_timer_cnt(void) {
    uint32_t clk = SystemCoreClock ? SystemCoreClock : 48000000UL;
    if (clk >= 48000000UL) return UPD_TMR_TX_48M;
    if (clk >= 24000000UL) return UPD_TMR_TX_24M;
    return UPD_TMR_TX_12M;
}

static inline uint16_t usbpd_tx_sel_for_sop(bsp_pd_sop_type_t sop) {
    switch (sop) {
        case BSP_PD_SOP_TYPE_SOP1: return UPD_SOP1;
        case BSP_PD_SOP_TYPE_SOP2: return UPD_SOP2;
        case BSP_PD_SOP_TYPE_SOP0:
        default: return UPD_SOP0;
    }
}

static inline bsp_pd_sop_type_t usbpd_sop_from_hw_status(uint32_t status) {
    uint32_t aux = status & BMC_AUX_Mask;
    if (aux == BMC_AUX_SOP0) return BSP_PD_SOP_TYPE_SOP0;
    if (aux == BMC_AUX_SOP1_HRST) return BSP_PD_SOP_TYPE_SOP1;
    if (aux == BMC_AUX_SOP2_CRST) return BSP_PD_SOP_TYPE_SOP2;
    return BSP_PD_SOP_TYPE_SOP0;
}

static void usbpd_rx_mode(void)
    __attribute__((section(".highcode")))
    __attribute__((noinline));
static void usbpd_rx_mode(void) {
    USBPD->CONFIG |= PD_ALL_CLR;
    USBPD->CONFIG &= ~PD_ALL_CLR;

    /* The revised Type-C schematic routes the receptacle CC1 net to MCU PC15/USBPD CC2. */
    USBPD->CONFIG |= CC_SEL;

    USBPD->CONFIG |= IE_RX_ACT | IE_RX_RESET | PD_DMA_EN | PD_FILT_ED;

    USBPD->DMA = (uint32_t)s_rx_dma_buf;
    USBPD->CONTROL &= ~PD_TX_EN;
    USBPD->BMC_CLK_CNT = usbpd_get_rx_timer_cnt();
    USBPD->CONTROL |= BMC_START;

    NVIC_EnableIRQ(USBPD_IRQn);
}

void USBPD_IRQHandler(void)
    __attribute__((section(".highcode")))
    __attribute__((noinline))
    __attribute__((interrupt("WCH-Interrupt-fast")))
    __attribute__((aligned(4)));
void USBPD_IRQHandler(void) {
    uint32_t status = USBPD->STATUS;

    if (status & IF_RX_ACT) {
        USBPD->STATUS |= IF_RX_ACT;
        uint8_t len = USBPD->BMC_BYTE_CNT;
        if (len >= 2u && len <= USBPD_DATA_MAX_LEN && s_rx_callback) {
            bsp_pd_rx_info_t info = {
                .event = BSP_PD_RX_EVENT_FRAME,
                .sop = usbpd_sop_from_hw_status(status),
                .data = (const uint8_t *)s_rx_dma_buf,
                .len = len,
                .hw_status = status,
            };
            s_rx_callback(&info, s_user_data);
        }
        usbpd_rx_mode();
    }

    if (status & IF_RX_RESET) {
        USBPD->STATUS |= IF_RX_RESET;
        if (s_rx_callback) {
            bsp_pd_rx_info_t info = {
                .event = ((status & BMC_AUX_Mask) == BMC_AUX_SOP2_CRST) ? BSP_PD_RX_EVENT_CRST : BSP_PD_RX_EVENT_HRST,
                .sop = usbpd_sop_from_hw_status(status),
                .data = NULL,
                .len = 0u,
                .hw_status = status,
            };
            s_rx_callback(&info, s_user_data);
        }
        usbpd_rx_mode();
    }

    if (status & BUF_ERR) {
        USBPD->STATUS |= BUF_ERR;
        usbpd_rx_mode();
    }
}

void bsp_pd_phy_init(const bsp_pd_phy_config_t *cfg) {
    if (cfg) {
        s_rx_callback = cfg->on_rx;
        s_user_data = cfg->user_data;
    } else {
        s_rx_callback = NULL;
        s_user_data = NULL;
    }

    GPIO_InitTypeDef gpio = {0};
    NVIC_InitTypeDef nvic = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_AFIO, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_USBPD, ENABLE);

    nvic.NVIC_IRQChannel = USBPD_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 0;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    gpio.GPIO_Pin = GPIO_Pin_15;
    gpio.GPIO_Speed = GPIO_Speed_50MHz;
    gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING;
    GPIO_Init(GPIOC, &gpio);

    AFIO->CTLR |= AFIO_CTLR_USBPD_IN_HVT;

    USBPD->CONFIG = PD_DMA_EN | PD_FILT_ED;
    USBPD->STATUS = BUF_ERR | IF_RX_BIT | IF_RX_BYTE | IF_RX_ACT | IF_RX_RESET | IF_TX_END;

    USBPD->PORT_CC2 = (uint8_t)((USBPD->PORT_CC2 & (~CC_CMP_Mask)) | BSP_USBPD_CC_CMP_DEFAULT);
    USBPD->PORT_CC2 &= ~CC_PD;

    // External Rd is fixed in hardware for this project.
    USBPD->CONFIG |= CC_SEL;
    usbpd_rx_mode();

}

void bsp_pd_phy_send(bsp_pd_sop_type_t sop, const uint8_t *data, uint8_t len)
    __attribute__((section(".highcode")))
    __attribute__((noinline));
void bsp_pd_phy_send(bsp_pd_sop_type_t sop, const uint8_t *data, uint8_t len) {
    if (len > USBPD_DATA_MAX_LEN) len = USBPD_DATA_MAX_LEN;
    if (len > 0u && data) {
        memcpy((void *)s_tx_dma_buf, data, len);
    }

    USBPD->PORT_CC2 |= CC_LVE;

    USBPD->BMC_CLK_CNT = usbpd_get_tx_timer_cnt();
    USBPD->DMA = (uint32_t)s_tx_dma_buf;
    USBPD->TX_SEL = usbpd_tx_sel_for_sop(sop);
    USBPD->BMC_TX_SZ = len;

    USBPD->CONTROL |= PD_TX_EN;
    USBPD->STATUS &= BMC_AUX_INVALID;
    USBPD->CONTROL |= BMC_START;

    while ((USBPD->STATUS & IF_TX_END) == 0u) {
    }
    USBPD->STATUS |= IF_TX_END;

    USBPD->PORT_CC2 &= ~CC_LVE;

    usbpd_rx_mode();
}

void bsp_pd_phy_send_hard_reset(void) {
    USBPD->PORT_CC2 |= CC_LVE;

    USBPD->BMC_CLK_CNT = usbpd_get_tx_timer_cnt();
    USBPD->DMA = (uint32_t)s_tx_dma_buf;
    USBPD->TX_SEL = UPD_HARD_RESET;
    USBPD->BMC_TX_SZ = 0u;

    USBPD->CONTROL |= PD_TX_EN;
    USBPD->STATUS &= BMC_AUX_INVALID;
    USBPD->CONTROL |= BMC_START;

    while ((USBPD->STATUS & IF_TX_END) == 0u) {
    }
    USBPD->STATUS |= IF_TX_END;

    USBPD->PORT_CC2 &= ~CC_LVE;

    usbpd_rx_mode();
}

void bsp_pd_phy_irq_lock(void) {
    NVIC_DisableIRQ(USBPD_IRQn);
}

void bsp_pd_phy_irq_unlock(void) {
    NVIC_EnableIRQ(USBPD_IRQn);
}
