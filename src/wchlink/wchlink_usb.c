#include "wchlink_usb.h"

#include "wchlink_protocol.h"

#include <stdbool.h>
#include <string.h>

#include <ch32x035.h>
#include <usbd_core.h>

#define WCHLINK_MPS 64u
#define WCHLINK_VID 0x1a86u
#define WCHLINK_PID 0x8010u

static const uint8_t wchlink_device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_1_1, 0xef, 0x02, 0x01,
                               WCHLINK_VID, WCHLINK_PID, 0x0100, 0x01),
};

static const uint8_t wchlink_config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(48u, 1u, 0x01, USB_CONFIG_BUS_POWERED, 50u),
    USB_INTERFACE_DESCRIPTOR_INIT(0u, 0u, 4u, 0xffu, 0u, 0u, 0u),
    USB_ENDPOINT_DESCRIPTOR_INIT(0x01u, USB_ENDPOINT_TYPE_BULK, WCHLINK_MPS, 0u),
    USB_ENDPOINT_DESCRIPTOR_INIT(0x81u, USB_ENDPOINT_TYPE_BULK, WCHLINK_MPS, 0u),
    USB_ENDPOINT_DESCRIPTOR_INIT(0x02u, USB_ENDPOINT_TYPE_BULK, WCHLINK_MPS, 0u),
    USB_ENDPOINT_DESCRIPTOR_INIT(0x82u, USB_ENDPOINT_TYPE_BULK, WCHLINK_MPS, 0u),
};

static const char wchlink_langid[] = {0x09, 0x04};
static const char *const wchlink_strings[] = {
    wchlink_langid,
    "Sora",
    "WCH-LinkE RVSWD",
    "0000000000000001",
};

static const uint8_t *wchlink_device_descriptor_callback(uint8_t speed) {
    (void)speed;
    return wchlink_device_descriptor;
}

static const uint8_t *wchlink_config_descriptor_callback(uint8_t speed) {
    (void)speed;
    return wchlink_config_descriptor;
}

static const char *wchlink_string_descriptor_callback(uint8_t speed, uint8_t index) {
    (void)speed;
    if (index >= (sizeof(wchlink_strings) / sizeof(wchlink_strings[0]))) {
        return NULL;
    }
    return wchlink_strings[index];
}

static const struct usb_descriptor wchlink_descriptor = {
    .device_descriptor_callback = wchlink_device_descriptor_callback,
    .config_descriptor_callback = wchlink_config_descriptor_callback,
    .device_quality_descriptor_callback = NULL,
    .other_speed_descriptor_callback = NULL,
    .string_descriptor_callback = wchlink_string_descriptor_callback,
};

static struct usbd_interface wchlink_interface;
static struct usbd_endpoint wchlink_out_endpoint;
static struct usbd_endpoint wchlink_in_endpoint;
static struct usbd_endpoint wchlink_data_out_endpoint;
static struct usbd_endpoint wchlink_data_in_endpoint;

static uint8_t wchlink_request[WCHLINK_MPS] __attribute__((aligned(4)));
static uint8_t wchlink_response[WCHLINK_MPS] __attribute__((aligned(4)));
static uint8_t wchlink_data_packet[WCHLINK_MPS] __attribute__((aligned(4)));
static uint8_t wchlink_data_out_buffer[256u] __attribute__((aligned(4)));
static volatile uint16_t wchlink_request_length;
static volatile bool wchlink_request_pending;
static volatile bool wchlink_response_pending;
static volatile bool wchlink_data_in_pending;
static volatile bool wchlink_data_out_active;
static volatile bool wchlink_data_out_pending;
static volatile uint16_t wchlink_data_out_length;
static volatile bool wchlink_configured;

static void wchlink_service_data_in(void);
static void wchlink_service_data_out(void);

static void wchlink_arm_request(void) {
    if (wchlink_configured && !wchlink_request_pending && !wchlink_response_pending) {
        (void)usbd_ep_start_read(0u, 0x01u, wchlink_request, sizeof(wchlink_request));
    }
}

static void wchlink_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)busid;
    (void)ep;
    if (nbytes > sizeof(wchlink_request)) {
        nbytes = sizeof(wchlink_request);
    }
    wchlink_request_length = (uint16_t)nbytes;
    wchlink_request_pending = true;
}

static void wchlink_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)busid;
    (void)ep;
    (void)nbytes;
    wchlink_response_pending = false;
    wchlink_arm_request();
}

static void wchlink_data_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)busid;
    (void)ep;
    (void)nbytes;
    wchlink_data_in_pending = false;
}

static void wchlink_data_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes) {
    (void)busid;
    (void)ep;
    if (nbytes > sizeof(wchlink_data_out_buffer)) {
        nbytes = sizeof(wchlink_data_out_buffer);
    }
    wchlink_data_out_length = (uint16_t)nbytes;
    wchlink_data_out_active = false;
    wchlink_data_out_pending = true;
}

static void wchlink_event_handler(uint8_t busid, uint8_t event) {
    (void)busid;
    switch (event) {
        case USBD_EVENT_RESET:
        case USBD_EVENT_DISCONNECTED:
        case USBD_EVENT_SUSPEND:
            wchlink_configured = false;
            wchlink_request_pending = false;
            wchlink_response_pending = false;
            wchlink_data_in_pending = false;
            wchlink_data_out_active = false;
            wchlink_data_out_pending = false;
            wchlink_protocol_reset();
            break;
        case USBD_EVENT_CONFIGURED:
            wchlink_configured = true;
            wchlink_request_pending = false;
            wchlink_response_pending = false;
            wchlink_data_in_pending = false;
            wchlink_data_out_active = false;
            wchlink_data_out_pending = false;
            wchlink_arm_request();
            break;
        case USBD_EVENT_RESUME:
            break;
        default:
            break;
    }
}

void wchlink_usb_init(void) {
    wchlink_out_endpoint.ep_addr = 0x01u;
    wchlink_out_endpoint.ep_cb = wchlink_out_callback;
    wchlink_in_endpoint.ep_addr = 0x81u;
    wchlink_in_endpoint.ep_cb = wchlink_in_callback;
    wchlink_data_out_endpoint.ep_addr = 0x02u;
    wchlink_data_out_endpoint.ep_cb = wchlink_data_out_callback;
    wchlink_data_in_endpoint.ep_addr = 0x82u;
    wchlink_data_in_endpoint.ep_cb = wchlink_data_in_callback;

    usbd_desc_register(0u, &wchlink_descriptor);
    usbd_add_interface(0u, &wchlink_interface);
    usbd_add_endpoint(0u, &wchlink_out_endpoint);
    usbd_add_endpoint(0u, &wchlink_in_endpoint);
    usbd_add_endpoint(0u, &wchlink_data_out_endpoint);
    usbd_add_endpoint(0u, &wchlink_data_in_endpoint);
    usbd_initialize(0u, 0u, wchlink_event_handler);
}

static void wchlink_service_data_in(void) {
    size_t data_length;

    if (!wchlink_configured || wchlink_data_in_pending) {
        return;
    }

    if (wchlink_protocol_take_data_reply(wchlink_data_packet,
                                         sizeof(wchlink_data_packet))) {
        data_length = 4u;
    } else {
        if (!wchlink_protocol_data_read_active()) {
            return;
        }
        data_length = wchlink_protocol_read_data(wchlink_data_packet,
                                                 sizeof(wchlink_data_packet));
    }
    if (data_length == 0u) {
        return;
    }

    wchlink_data_in_pending = true;
    if (usbd_ep_start_write(0u, 0x82u, wchlink_data_packet,
                            (uint32_t)data_length) != 0) {
        wchlink_data_in_pending = false;
    }
}

static void wchlink_service_data_out(void) {
    uint16_t data_length;

    if (!wchlink_configured) {
        return;
    }

    if (wchlink_data_out_pending) {
        __disable_irq();
        data_length = wchlink_data_out_length;
        wchlink_data_out_pending = false;
        __enable_irq();
        wchlink_protocol_write_data(wchlink_data_out_buffer, data_length);
    }

    if (wchlink_protocol_data_write_active() && !wchlink_data_out_active &&
        !wchlink_data_out_pending) {
        wchlink_data_out_active = true;
        if (usbd_ep_start_read(0u, 0x02u, wchlink_data_out_buffer,
                               sizeof(wchlink_data_out_buffer)) != 0) {
            wchlink_data_out_active = false;
        }
    }
}

void wchlink_usb_process(void) {
    uint16_t request_length;
    size_t response_length;

    if (!wchlink_configured) {
        return;
    }

    wchlink_service_data_out();
    wchlink_service_data_in();
    if (!wchlink_request_pending || wchlink_response_pending) {
        return;
    }

    __disable_irq();
    request_length = wchlink_request_length;
    wchlink_request_pending = false;
    __enable_irq();

    response_length = wchlink_protocol_process(wchlink_request, request_length,
                                                wchlink_response, sizeof(wchlink_response));
    if (response_length == 0u) {
        response_length = 4u;
        memset(wchlink_response, 0, response_length);
    }
    wchlink_response_pending = true;
    if (usbd_ep_start_write(0u, 0x81u, wchlink_response, (uint32_t)response_length) != 0) {
        wchlink_response_pending = false;
        wchlink_arm_request();
    }
    wchlink_service_data_in();
    wchlink_service_data_out();
}
