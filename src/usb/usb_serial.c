#include "usb_serial.h"

#include <stddef.h>
#include <string.h>

#include "chry_ringbuffer.h"
#include "dap/dap_usb.h"
#ifndef USBFS_PORT_BENCHMARK
#define USBFS_PORT_BENCHMARK 0
#endif
#if !USBFS_PORT_BENCHMARK
#include "uart_bridge.h"
#endif
#include "usbd_cdc_acm.h"
#include "usbd_core.h"

#define USB_SERIAL_MPS 64u
#define USB_SERIAL_USB_TO_UART_RING_SIZE 1024u
#define USB_SERIAL_PORT0_UART_TO_USB_RING_SIZE 1024u
#define USB_SERIAL_PORT1_UART_TO_USB_RING_SIZE 4096u
#define USB_SERIAL_ACTIVE_PORT_COUNT 1u
#define USB_SERIAL_CONFIG_DESC_SIZE (9u + 9u + 7u + 7u + CDC_ACM_DESCRIPTOR_LEN)
#define HIGHCODE __attribute__((section(".highcode"), noinline))

/*
 * Do not use WCH's VID for application firmware: macOS's WCH driver claims
 * the whole composite device before the class-compliant ACM driver can bind.
 * Replace these development IDs with an assigned VID/PID before release.
 */
#ifndef USB_SERIAL_VID
#define USB_SERIAL_VID 0x046Au
#endif
#ifndef USB_SERIAL_PID
#define USB_SERIAL_PID 0x4001u
#endif

_Static_assert(USB_SERIAL_PORT_COUNT >= 1u && USB_SERIAL_PORT_COUNT <= 2u, "CH32X035 USBFS supports at most two CDC ports");
_Static_assert((USB_SERIAL_USB_TO_UART_RING_SIZE & (USB_SERIAL_USB_TO_UART_RING_SIZE - 1u)) == 0u, "USB-to-UART ring size must be a power of two");
_Static_assert((USB_SERIAL_PORT0_UART_TO_USB_RING_SIZE & (USB_SERIAL_PORT0_UART_TO_USB_RING_SIZE - 1u)) == 0u, "CDC0 UART-to-USB ring size must be a power of two");
_Static_assert((USB_SERIAL_PORT1_UART_TO_USB_RING_SIZE & (USB_SERIAL_PORT1_UART_TO_USB_RING_SIZE - 1u)) == 0u, "CDC1 UART-to-USB ring size must be a power of two");

typedef struct {
    uint8_t in_ep;
    uint8_t out_ep;
    uint8_t int_ep;
    uint8_t control_intf;
    volatile bool configured;
    volatile bool dtr;
    volatile bool rts;
    volatile bool out_read_armed;
    volatile bool out_paused;
    volatile bool in_busy;
    volatile uint16_t out_pending_len;
    volatile uint8_t out_pending_idx;
    volatile uint8_t out_armed_idx;
    volatile uint32_t line_coding_version;
    struct cdc_line_coding line_coding;
    uint8_t out_packet[2u][USB_SERIAL_MPS] __attribute__((aligned(4)));
    uint8_t usb_to_uart_pool[USB_SERIAL_USB_TO_UART_RING_SIZE];
    uint8_t *uart_to_usb_pool;
    uint16_t uart_to_usb_pool_size;
    volatile uint32_t uart_to_usb_in;
    volatile uint32_t uart_to_usb_out;
    chry_ringbuffer_t usb_to_uart;
    struct usbd_endpoint out_endpoint;
    struct usbd_endpoint in_endpoint;
    struct usbd_interface control_interface;
    struct usbd_interface data_interface;
} usb_serial_port_t;

static uint8_t s_port0_uart_to_usb_pool[USB_SERIAL_PORT0_UART_TO_USB_RING_SIZE] __attribute__((aligned(4)));
#if USB_SERIAL_PORT_COUNT >= 2u
static uint8_t s_port1_uart_to_usb_pool[USB_SERIAL_PORT1_UART_TO_USB_RING_SIZE] __attribute__((aligned(4)));
#endif

/*
 * Two independent CDC functions. EP4 aliases the EP0 DMA register and is not
 * usable. Keep bulk IN, bulk OUT, and interrupt IN on distinct endpoint
 * numbers because X035 USBFS does not reliably run both directions on one DMA
 * endpoint under sustained full-duplex traffic.
 */
static usb_serial_port_t s_ports[USB_SERIAL_PORT_COUNT] = {
    { .in_ep = 0x85u, .out_ep = 0x06u, .int_ep = 0x87u, .control_intf = 1u },
#if USB_SERIAL_PORT_COUNT >= 2u
    { .in_ep = 0x85u, .out_ep = 0x06u, .int_ep = 0x87u, .control_intf = 2u },
#endif
};

static const uint8_t s_config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_SERIAL_CONFIG_DESC_SIZE, 3u, 0x01, USB_CONFIG_BUS_POWERED, 100),
    USB_INTERFACE_DESCRIPTOR_INIT(0u, 0u, 2u, 0xffu, 0u, 0u, 0u),
    USB_ENDPOINT_DESCRIPTOR_INIT(0x02u, USB_ENDPOINT_TYPE_BULK, USB_SERIAL_MPS, 0u),
    USB_ENDPOINT_DESCRIPTOR_INIT(0x81u, USB_ENDPOINT_TYPE_BULK, USB_SERIAL_MPS, 0u),
    CDC_ACM_DESCRIPTOR_INIT(1u, 0x87u, 0x06u, 0x85u, USB_SERIAL_MPS, 0u),
};

static const uint8_t s_device_descriptor[] = {
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USB_SERIAL_VID, USB_SERIAL_PID, 0x0100, 0x01),
};
static const char s_langid[] = { 0x09, 0x04 };
static const char *s_strings[] = { s_langid, "WCH", "CH32X035 Multi CDC", "0001" };

static usb_serial_port_t *usb_serial_port_from_ep(uint8_t ep)
{
    for (uint8_t i = 0; i < USB_SERIAL_ACTIVE_PORT_COUNT; ++i) {
        if (s_ports[i].in_ep == ep || s_ports[i].out_ep == ep) {
            return &s_ports[i];
        }
    }
    return NULL;
}

static usb_serial_port_t *usb_serial_port_from_intf(uint8_t intf)
{
    for (uint8_t i = 0; i < USB_SERIAL_ACTIVE_PORT_COUNT; ++i) {
        if (s_ports[i].control_intf == intf) {
            return &s_ports[i];
        }
    }
    return NULL;
}

#if !USBFS_PORT_BENCHMARK
static uint8_t usb_serial_port_index(const usb_serial_port_t *port)
{
    return (uint8_t)(port - s_ports);
}
#endif

static void usb_serial_control_line_changed(usb_serial_port_t *port)
{
#if USBFS_PORT_BENCHMARK
    (void)port;
#else
    uart_bridge_control_line_changed(usb_serial_port_index(port), port->dtr, port->rts);
#endif
}

static void usb_serial_reset_port(usb_serial_port_t *port)
{
    port->dtr = false;
    port->rts = false;
    port->out_read_armed = false;
    port->out_paused = false;
    port->in_busy = false;
    port->out_pending_len = 0u;
    port->out_pending_idx = 0u;
    port->out_armed_idx = 0u;
    chry_ringbuffer_reset(&port->usb_to_uart);
    port->uart_to_usb_in = 0u;
    port->uart_to_usb_out = 0u;
}

static uint32_t usb_serial_uart_to_usb_used(const usb_serial_port_t *port)
{
    return port->uart_to_usb_in - port->uart_to_usb_out;
}

static uint32_t usb_serial_uart_to_usb_free(const usb_serial_port_t *port)
{
    return port->uart_to_usb_pool_size - usb_serial_uart_to_usb_used(port);
}

static void HIGHCODE usb_serial_start_read(usb_serial_port_t *port)
{
    uint8_t idx;

    if (port == NULL || !port->configured || port->out_read_armed || port->out_pending_len != 0u) {
        return;
    }

    if (chry_ringbuffer_get_free(&port->usb_to_uart) < USB_SERIAL_MPS) {
        port->out_paused = true;
        return;
    }

    idx = port->out_armed_idx ^ 1u;
    /*
     * usbd_ep_start_read() ACKs the hardware endpoint.  Mark it armed before
     * that write so an immediate USB IRQ cannot clear the flag and then have
     * this caller set a stale true value after the callback returns.
     */
    port->out_read_armed = true;
    port->out_paused = false;
    port->out_armed_idx = idx;
    if (usbd_ep_start_read(0, port->out_ep, port->out_packet[idx], sizeof(port->out_packet[idx])) != 0) {
        port->out_read_armed = false;
        port->out_paused = true;
    }
}

static void HIGHCODE usb_serial_out_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    usb_serial_port_t *port;

    (void)busid;
    port = usb_serial_port_from_ep(ep);
    if (port == NULL || !port->configured) {
        return;
    }

    port->out_read_armed = false;
    if (nbytes > USB_SERIAL_MPS) {
        nbytes = USB_SERIAL_MPS;
    }
    if (nbytes != 0u) {
        /*
         * Do not touch the ring from IRQ context.  The X035 USBFS IRQ can
         * preempt the main-loop ring read at any instruction, and the
         * IRQ-context ring write corrupts the ring state on this target.
         * Stash the completed ping-pong buffer instead; the main loop moves
         * it into the ring.  If the previous stash is still pending the main
         * loop is behind: keep the endpoint NAK'd (no re-arm) until the drain
         * catches up, so a pending buffer is never overwritten.
         */
        if (port->out_pending_len != 0u) {
            /* An OUT transfer is never re-armed while a packet is pending. */
            return;
        }
        port->out_pending_idx = port->out_armed_idx;
        port->out_pending_len = (uint16_t)nbytes;
    }
}

static void HIGHCODE usb_serial_start_in(usb_serial_port_t *port)
{
    uint32_t in;
    uint32_t out;
    uint32_t size;
    uint32_t offset;
    uint8_t *data;

    if (port == NULL || !port->configured || port->in_busy) {
        return;
    }

    in = port->uart_to_usb_in;
    out = port->uart_to_usb_out;
    __asm__ volatile("fence rw, rw" ::: "memory");
    size = in - out;
    if (size == 0u) {
        return;
    }

    offset = out & (port->uart_to_usb_pool_size - 1u);
    if (size > port->uart_to_usb_pool_size - offset) {
        size = port->uart_to_usb_pool_size - offset;
    }
    data = port->uart_to_usb_pool + offset;
    port->in_busy = true;
    if (usbd_ep_start_write(0, port->in_ep, data, size) != 0) {
        port->in_busy = false;
    }
}

static void HIGHCODE usb_serial_in_callback(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    usb_serial_port_t *port;

    (void)busid;
    port = usb_serial_port_from_ep(ep);
    if (port == NULL || !port->in_busy) {
        return;
    }

    if (nbytes > usb_serial_uart_to_usb_used(port)) {
        nbytes = usb_serial_uart_to_usb_used(port);
    }
    port->uart_to_usb_out += nbytes;
    port->in_busy = false;
    usb_serial_start_in(port);
}

static const uint8_t *usb_serial_device_descriptor(uint8_t speed)
{
    (void)speed;
    return s_device_descriptor;
}

static const uint8_t *usb_serial_config_descriptor(uint8_t speed)
{
    (void)speed;
    return s_config_descriptor;
}

static const char *usb_serial_string_descriptor(uint8_t speed, uint8_t index)
{
    (void)speed;
    return index < 4u ? s_strings[index] : NULL;
}

static const struct usb_descriptor s_descriptor = {
    .device_descriptor_callback = usb_serial_device_descriptor,
    .config_descriptor_callback = usb_serial_config_descriptor,
    .string_descriptor_callback = usb_serial_string_descriptor,
};

static void usb_serial_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;
    dap_usb_event(event);
    for (uint8_t i = 0; i < USB_SERIAL_ACTIVE_PORT_COUNT; ++i) {
        usb_serial_port_t *port = &s_ports[i];

        if (event == USBD_EVENT_RESET || event == USBD_EVENT_DISCONNECTED) {
            port->configured = false;
            usb_serial_reset_port(port);
            usb_serial_control_line_changed(port);
        } else if (event == USBD_EVENT_CONFIGURED) {
            port->configured = true;
            usb_serial_reset_port(port);
            usb_serial_control_line_changed(port);
            usb_serial_start_read(port);
        }
    }
}

void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr)
{
    usb_serial_port_t *port;

    (void)busid;
    port = usb_serial_port_from_intf(intf);
    if (port != NULL) {
        port->dtr = dtr;
        usb_serial_control_line_changed(port);
    }
}

void usbd_cdc_acm_set_rts(uint8_t busid, uint8_t intf, bool rts)
{
    usb_serial_port_t *port;

    (void)busid;
    port = usb_serial_port_from_intf(intf);
    if (port != NULL) {
        port->rts = rts;
        usb_serial_control_line_changed(port);
    }
}

void usbd_cdc_acm_set_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *line_coding)
{
    usb_serial_port_t *port;

    (void)busid;
    port = usb_serial_port_from_intf(intf);
    if (port != NULL && line_coding != NULL) {
        ++port->line_coding_version;
        memcpy(&port->line_coding, line_coding, sizeof(port->line_coding));
        ++port->line_coding_version;
    }
}

void usbd_cdc_acm_get_line_coding(uint8_t busid, uint8_t intf, struct cdc_line_coding *line_coding)
{
    usb_serial_port_t *port;

    (void)busid;
    port = usb_serial_port_from_intf(intf);
    if (port != NULL && line_coding != NULL) {
        memcpy(line_coding, &port->line_coding, sizeof(*line_coding));
    }
}

void usb_serial_init(void)
{
    usbd_desc_register(0, &s_descriptor);
    dap_usb_init();
    for (uint8_t i = 0; i < USB_SERIAL_ACTIVE_PORT_COUNT; ++i) {
        usb_serial_port_t *port = &s_ports[i];

        (void)chry_ringbuffer_init(&port->usb_to_uart, port->usb_to_uart_pool, sizeof(port->usb_to_uart_pool));
        if (i == 0u) {
            port->uart_to_usb_pool = s_port0_uart_to_usb_pool;
            port->uart_to_usb_pool_size = sizeof(s_port0_uart_to_usb_pool);
        }
#if USB_SERIAL_PORT_COUNT >= 2u
        else {
            port->uart_to_usb_pool = s_port1_uart_to_usb_pool;
            port->uart_to_usb_pool_size = sizeof(s_port1_uart_to_usb_pool);
        }
#endif
        port->uart_to_usb_in = 0u;
        port->uart_to_usb_out = 0u;
        port->line_coding.dwDTERate = 115200u;
        port->line_coding.bDataBits = 8u;
        port->line_coding.bParityType = 0u;
        port->line_coding.bCharFormat = 0u;
        port->line_coding_version = 2u;

        port->out_endpoint.ep_addr = port->out_ep;
        port->out_endpoint.ep_cb = usb_serial_out_callback;
        port->in_endpoint.ep_addr = port->in_ep;
        port->in_endpoint.ep_cb = usb_serial_in_callback;
        usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &port->control_interface));
        usbd_add_interface(0, usbd_cdc_acm_init_intf(0, &port->data_interface));
        usbd_add_endpoint(0, &port->out_endpoint);
        usbd_add_endpoint(0, &port->in_endpoint);
    }
    usbd_initialize(0, 0, usb_serial_event_handler);
}

bool usb_serial_is_open(uint8_t port)
{
    return port < USB_SERIAL_PORT_COUNT && s_ports[port].configured && s_ports[port].dtr;
}

uint16_t usb_serial_rx_available(uint8_t port)
{
    if (port >= USB_SERIAL_PORT_COUNT) {
        return 0u;
    }
    return (uint16_t)chry_ringbuffer_get_used(&s_ports[port].usb_to_uart);
}

uint16_t usb_serial_tx_free(uint8_t port)
{
    if (port >= USB_SERIAL_PORT_COUNT) {
        return 0u;
    }
    return (uint16_t)usb_serial_uart_to_usb_free(&s_ports[port]);
}

uint16_t usb_serial_read(uint8_t port, uint8_t *data, uint16_t len)
{
    if (port >= USB_SERIAL_PORT_COUNT || data == NULL || len == 0u) {
        return 0u;
    }
    return (uint16_t)chry_ringbuffer_read(&s_ports[port].usb_to_uart, data, len);
}

uint16_t usb_serial_write(uint8_t port, const uint8_t *data, uint16_t len)
{
    if (port >= USB_SERIAL_PORT_COUNT || data == NULL || len == 0u) {
        return 0u;
    }
    usb_serial_port_t *serial_port = &s_ports[port];
    uint32_t in = serial_port->uart_to_usb_in;
    uint32_t free = usb_serial_uart_to_usb_free(serial_port);
    uint32_t offset;
    uint32_t first;

    if (len > free) {
        len = (uint16_t)free;
    }
    offset = in & (serial_port->uart_to_usb_pool_size - 1u);
    first = serial_port->uart_to_usb_pool_size - offset;
    if (first > len) {
        first = len;
    }
    memcpy(serial_port->uart_to_usb_pool + offset, data, first);
    memcpy(serial_port->uart_to_usb_pool, data + first, len - first);
    __asm__ volatile("fence rw, rw" ::: "memory");
    serial_port->uart_to_usb_in = in + len;
    return len;
}

bool usb_serial_get_line_config(uint8_t port, usb_serial_line_config_t *config, uint32_t *version)
{
    usb_serial_port_t *serial_port;
    uint32_t before;
    uint32_t after;

    if (port >= USB_SERIAL_PORT_COUNT || config == NULL) {
        return false;
    }

    serial_port = &s_ports[port];
    for (;;) {
        before = serial_port->line_coding_version;
        if ((before & 1u) != 0u) {
            continue;
        }
        config->baud_rate = serial_port->line_coding.dwDTERate;
        config->stop_bits = serial_port->line_coding.bCharFormat;
        config->parity = serial_port->line_coding.bParityType;
        config->data_bits = serial_port->line_coding.bDataBits;
        after = serial_port->line_coding_version;
        if (before == after) {
            break;
        }
    }

    if (version != NULL) {
        *version = after;
    }
    return true;
}

void HIGHCODE usb_serial_process(void)
{
    dap_usb_process();
    for (uint8_t i = 0; i < USB_SERIAL_ACTIVE_PORT_COUNT; ++i) {
        usb_serial_port_t *port = &s_ports[i];

        /* Move the IRQ-stashed OUT packet in main-loop context before re-arming OUT. */
        if (port->out_pending_len != 0u) {
            uint16_t len = port->out_pending_len;
            uint8_t idx = port->out_pending_idx;

            (void)chry_ringbuffer_write(&port->usb_to_uart, port->out_packet[idx], len);
            port->out_pending_len = 0u;
        }

        usb_serial_start_read(port);
        usb_serial_start_in(port);
    }
}
