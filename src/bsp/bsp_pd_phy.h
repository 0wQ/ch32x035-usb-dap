#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BSP_PD_SOP_TYPE_SOP0 = 0,  // SOP
    BSP_PD_SOP_TYPE_SOP1 = 1,  // SOP' (cable)
    BSP_PD_SOP_TYPE_SOP2 = 2,  // SOP'' (cable)
} bsp_pd_sop_type_t;

typedef enum {
    BSP_PD_RX_EVENT_FRAME,  // Received complete frame
    BSP_PD_RX_EVENT_HRST,   // Hard Reset
    BSP_PD_RX_EVENT_CRST,   // Cable Reset
} bsp_pd_rx_event_t;

typedef struct {
    bsp_pd_rx_event_t event;
    bsp_pd_sop_type_t sop;
    const uint8_t *data;  // Valid only during callback
    uint8_t len;          // 2..34 bytes
    uint32_t hw_status;   // Raw hardware status
} bsp_pd_rx_info_t;

typedef void (*bsp_pd_rx_callback_t)(const bsp_pd_rx_info_t *info, void *user);

typedef struct {
    bsp_pd_rx_callback_t on_rx;
    void *user_data;
} bsp_pd_phy_config_t;

void bsp_pd_phy_init(const bsp_pd_phy_config_t *cfg);
void bsp_pd_phy_send(bsp_pd_sop_type_t sop, const uint8_t *data, uint8_t len);
void bsp_pd_phy_send_hard_reset(void);

void bsp_pd_phy_irq_lock(void);
void bsp_pd_phy_irq_unlock(void);
