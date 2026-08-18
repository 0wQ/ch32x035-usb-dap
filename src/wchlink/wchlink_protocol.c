#include "wchlink_protocol.h"

#include "rvswd_gpio.h"
#include "wchlink_flash_loader.h"

#define WCHLINK_COMMAND_PREFIX 0x81u
#define WCHLINK_REPLY_PREFIX   0x82u

#define WCHLINK_FAMILY_RESET   0x0bu
#define WCHLINK_FAMILY_SPEED   0x0cu
#define WCHLINK_FAMILY_CONTROL 0x0du
#define WCHLINK_FAMILY_INFO    0x11u
#define WCHLINK_FAMILY_DMI     0x08u

#define WCHLINK_CONTROL_IDENTIFY  0x01u
#define WCHLINK_CONTROL_CONNECT   0x02u
#define WCHLINK_CONTROL_HOLD      0x03u
#define WCHLINK_CONTROL_RESET_LOW 0x13u
#define WCHLINK_CONTROL_STOP      0xffu

#define WCHLINK_FLASH_LOADER_ADDRESS 0x20000000u
#define WCHLINK_FLASH_DATA_ADDRESS   0x20001000u
#define WCHLINK_FLASH_PACKET_SIZE    256u
#define WCHLINK_FLASH_CHUNK_SIZE     4096u

static bool wchlink_connected;
static uint32_t wchlink_read_address;
static uint32_t wchlink_read_remaining;
static bool wchlink_read_active;
static uint32_t wchlink_write_address;
static uint32_t wchlink_write_remaining;
static uint8_t wchlink_write_mode;
static uint32_t wchlink_loader_received;
static uint32_t wchlink_flash_data_received;
static uint32_t wchlink_flash_chunk_length;
static bool wchlink_loader_ready;
static bool wchlink_data_reply_pending;
static uint8_t wchlink_data_reply_status;

static size_t wchlink_ack(uint8_t *response, size_t capacity, uint8_t family) {
    if (capacity < 4u) {
        return 0u;
    }
    response[0] = WCHLINK_REPLY_PREFIX;
    response[1] = family;
    response[2] = 1u;
    response[3] = 0u;
    return 4u;
}

static size_t wchlink_unsupported(uint8_t *response, size_t capacity, uint8_t family) {
    if (capacity < 4u) {
        return 0u;
    }
    response[0] = WCHLINK_COMMAND_PREFIX;
    response[1] = family;
    response[2] = 1u;
    response[3] = 2u;
    return 4u;
}

static size_t wchlink_identity(uint8_t *response, size_t capacity) {
    if (capacity < 7u) {
        return 0u;
    }
    response[0] = WCHLINK_REPLY_PREFIX;
    response[1] = WCHLINK_FAMILY_CONTROL;
    response[2] = 4u;
    response[3] = 2u;
    response[4] = 8u;
    response[5] = 0x12u;
    response[6] = 0u;
    return 7u;
}

static size_t wchlink_connect_reply(uint8_t *response, size_t capacity, bool connected) {
    if (!connected) {
        if (capacity < 4u) {
            return 0u;
        }
        response[0] = WCHLINK_COMMAND_PREFIX;
        response[1] = 0x55u;
        response[2] = 1u;
        response[3] = 1u;
        return 4u;
    }
    if (capacity < 8u) {
        return 0u;
    }
    response[0] = WCHLINK_REPLY_PREFIX;
    response[1] = WCHLINK_FAMILY_CONTROL;
    response[2] = 5u;
    response[3] = 0x06u;
    response[4] = 0x30u;
    response[5] = 0x70u;
    response[6] = 0x05u;
    response[7] = 0x28u;
    return 8u;
}

static size_t wchlink_chip_info(uint8_t *response, size_t capacity) {
    if (capacity < 12u) {
        return 0u;
    }

    // wlink 的芯片信息查询使用无帧头的 12 字节回复
    response[0] = 0xffu;
    response[1] = 0xffu;
    response[2] = 0x01u;
    response[3] = 0x20u;
    response[4] = 0x5bu;
    response[5] = 0xa8u;
    response[6] = 0x0du;
    response[7] = 0x10u;
    response[8] = 0x53u;
    response[9] = 0x5cu;
    response[10] = 0xbbu;
    response[11] = 0x14u;
    return 12u;
}

static size_t wchlink_dmi(const uint8_t *request, uint8_t *response, size_t capacity) {
    uint8_t address;
    uint32_t data;
    bool success;

    if (capacity < 9u) {
        return 0u;
    }
    address = request[3];
    data = ((uint32_t)request[4] << 24u) | ((uint32_t)request[5] << 16u) |
           ((uint32_t)request[6] << 8u) | request[7];
    if (request[8] == 1u) {
        success = rvswd_gpio_read_dmi(address, &data);
    } else if (request[8] == 2u) {
        success = rvswd_gpio_write_dmi(address, data);
    } else {
        success = false;
    }

    response[0] = WCHLINK_REPLY_PREFIX;
    response[1] = WCHLINK_FAMILY_DMI;
    response[2] = 6u;
    response[3] = address;
    response[4] = (uint8_t)(data >> 24u);
    response[5] = (uint8_t)(data >> 16u);
    response[6] = (uint8_t)(data >> 8u);
    response[7] = (uint8_t)data;
    response[8] = success ? 0u : 2u;
    return 9u;
}

void wchlink_protocol_reset(void) {
    if (wchlink_connected) {
        rvswd_gpio_disconnect();
    }
    wchlink_connected = false;
    wchlink_read_address = 0u;
    wchlink_read_remaining = 0u;
    wchlink_read_active = false;
    wchlink_write_address = 0u;
    wchlink_write_remaining = 0u;
    wchlink_write_mode = 0u;
    wchlink_loader_received = 0u;
    wchlink_flash_data_received = 0u;
    wchlink_flash_chunk_length = 0u;
    wchlink_loader_ready = false;
    wchlink_data_reply_pending = false;
}

bool wchlink_protocol_is_connected(void) {
    return wchlink_connected;
}

void wchlink_protocol_begin_data_read(void) {
    if (wchlink_connected && wchlink_read_remaining != 0u) {
        wchlink_read_active = true;
    }
}

bool wchlink_protocol_data_read_active(void) {
    return wchlink_read_active;
}

bool wchlink_protocol_data_write_active(void) {
    return wchlink_write_mode != 0u;
}

void wchlink_protocol_write_data(const uint8_t *data, size_t length) {
    if (data == NULL || length == 0u || wchlink_write_mode == 0u) {
        return;
    }

    if (wchlink_write_mode == 1u) {
        if (length > WCHLINK_FLASH_PACKET_SIZE ||
            wchlink_loader_received + length > 512u ||
            !rvswd_gpio_write_memory(WCHLINK_FLASH_LOADER_ADDRESS + wchlink_loader_received,
                                     data, (uint32_t)length)) {
            wchlink_write_mode = 0u;
            return;
        }
        wchlink_loader_received += (uint32_t)length;
        if (wchlink_loader_received >= 512u) {
            wchlink_write_mode = 0u;
            wchlink_loader_ready = true;
        }
        return;
    }

    if (wchlink_write_mode == 2u && length == WCHLINK_FLASH_PACKET_SIZE &&
        wchlink_write_remaining != 0u &&
        wchlink_flash_data_received + length <= WCHLINK_FLASH_CHUNK_SIZE) {
        if (!rvswd_gpio_write_memory(WCHLINK_FLASH_DATA_ADDRESS + wchlink_flash_data_received,
                                     data, WCHLINK_FLASH_PACKET_SIZE)) {
            wchlink_write_mode = 0u;
            wchlink_data_reply_status = 0x05u;
            wchlink_data_reply_pending = true;
            return;
        }
        wchlink_flash_data_received += WCHLINK_FLASH_PACKET_SIZE;
        if (wchlink_flash_data_received >= wchlink_flash_chunk_length) {
            uint32_t result = 0xffffffffu;
            bool success = rvswd_gpio_execute(
                WCHLINK_FLASH_LOADER_ADDRESS, 0x0cu, wchlink_write_address,
                wchlink_flash_chunk_length, WCHLINK_FLASH_DATA_ADDRESS, &result);

            wchlink_data_reply_status = success && (result == 0u || result == 8u) ? 0x04u : 0x05u;
            wchlink_data_reply_pending = true;
            if (success && wchlink_write_remaining > wchlink_flash_chunk_length) {
                wchlink_write_address += wchlink_flash_chunk_length;
                wchlink_write_remaining -= wchlink_flash_chunk_length;
                wchlink_flash_data_received = 0u;
                wchlink_flash_chunk_length = wchlink_write_remaining > WCHLINK_FLASH_CHUNK_SIZE
                                                  ? WCHLINK_FLASH_CHUNK_SIZE
                                                  : wchlink_write_remaining;
            } else {
                wchlink_write_address = 0u;
                wchlink_write_remaining = 0u;
                wchlink_write_mode = 0u;
            }
        }
    }
}

bool wchlink_protocol_take_data_reply(uint8_t *data, size_t capacity) {
    if (data == NULL || capacity < 4u || !wchlink_data_reply_pending) {
        return false;
    }
    data[0] = 0x41u;
    data[1] = 0x01u;
    data[2] = 0x01u;
    data[3] = wchlink_data_reply_status;
    wchlink_data_reply_pending = false;
    return true;
}

size_t wchlink_protocol_read_data(uint8_t *data, size_t capacity) {
    size_t produced = 0u;

    if (data == NULL || capacity < 4u || !wchlink_read_active) {
        return 0u;
    }

    while (produced + 4u <= capacity && wchlink_read_remaining >= 4u) {
        uint32_t value;

        if (!rvswd_gpio_read_memory32(wchlink_read_address, &value)) {
            wchlink_read_active = false;
            wchlink_read_remaining = 0u;
            return 0u;
        }

        // WCH-Link 数据端点按大端字节发送，wlink 主机随后按字反转
        data[produced + 0u] = (uint8_t)(value >> 24u);
        data[produced + 1u] = (uint8_t)(value >> 16u);
        data[produced + 2u] = (uint8_t)(value >> 8u);
        data[produced + 3u] = (uint8_t)value;
        produced += 4u;
        wchlink_read_address += 4u;
        wchlink_read_remaining -= 4u;
    }

    if (wchlink_read_remaining == 0u) {
        wchlink_read_active = false;
    }
    return produced;
}

size_t wchlink_protocol_process(const uint8_t *request, size_t request_length,
                                uint8_t *response, size_t response_capacity) {
    uint8_t family;
    size_t response_length;

    if (request == NULL || response == NULL || request_length < 2u || request[0] != WCHLINK_COMMAND_PREFIX) {
        return wchlink_ack(response, response_capacity, 0u);
    }

    family = request[1];
    if (family == WCHLINK_FAMILY_DMI && request_length >= 9u) {
        return wchlink_dmi(request, response, response_capacity);
    }
    if (family == 0x01u && request_length >= 11u) {
        wchlink_write_address = ((uint32_t)request[3] << 24u) |
                                ((uint32_t)request[4] << 16u) |
                                ((uint32_t)request[5] << 8u) | request[6];
        wchlink_write_remaining = ((uint32_t)request[7] << 24u) |
                                   ((uint32_t)request[8] << 16u) |
                                   ((uint32_t)request[9] << 8u) | request[10];
        wchlink_write_mode = 0u;
        wchlink_loader_received = 0u;
        wchlink_flash_data_received = 0u;
        wchlink_flash_chunk_length = wchlink_write_remaining > WCHLINK_FLASH_CHUNK_SIZE
                                          ? WCHLINK_FLASH_CHUNK_SIZE
                                          : wchlink_write_remaining;
        wchlink_loader_ready = false;
        return wchlink_ack(response, response_capacity, family);
    }
    if (family == 0x03u && request_length >= 11u) {
        wchlink_read_address = ((uint32_t)request[3] << 24u) |
                               ((uint32_t)request[4] << 16u) |
                               ((uint32_t)request[5] << 8u) | request[6];
        wchlink_read_remaining = ((uint32_t)request[7] << 24u) |
                                  ((uint32_t)request[8] << 16u) |
                                  ((uint32_t)request[9] << 8u) | request[10];
        wchlink_read_active = false;
        return wchlink_ack(response, response_capacity, family);
    }
    if (family == 0x02u && request_length >= 4u) {
        switch (request[3]) {
            case 0x01u:
                return wchlink_unsupported(response, response_capacity, family);
            case 0x05u:
                if (!wchlink_connected || wchlink_write_remaining == 0u) {
                    return wchlink_unsupported(response, response_capacity, family);
                }
                wchlink_write_mode = 1u;
                wchlink_loader_received = 0u;
                return wchlink_ack(response, response_capacity, family);
            case 0x07u:
                if (wchlink_loader_ready &&
                    rvswd_gpio_execute(WCHLINK_FLASH_LOADER_ADDRESS, 0x01u, 0u, 0u,
                                       WCHLINK_FLASH_DATA_ADDRESS, NULL) &&
                    response_capacity >= 4u) {
                    response[0] = WCHLINK_REPLY_PREFIX;
                    response[1] = family;
                    response[2] = 1u;
                    response[3] = 0x07u;
                    return 4u;
                }
                return wchlink_unsupported(response, response_capacity, family);
            case 0x02u:
                if (!wchlink_loader_ready || wchlink_write_remaining == 0u) {
                    return wchlink_unsupported(response, response_capacity, family);
                }
                wchlink_write_mode = 2u;
                wchlink_flash_data_received = 0u;
                return wchlink_ack(response, response_capacity, family);
            case 0x08u:
                wchlink_write_mode = 0u;
                return wchlink_ack(response, response_capacity, family);
            case 0x0cu:
                wchlink_protocol_begin_data_read();
                return wchlink_ack(response, response_capacity, family);
            default:
                return wchlink_ack(response, response_capacity, family);
        }
    }
    if (family == WCHLINK_FAMILY_INFO) {
        return wchlink_chip_info(response, response_capacity);
    }
    if (family == WCHLINK_FAMILY_SPEED) {
        response_length = wchlink_ack(response, response_capacity, family);
        if (response_length != 0u) {
            response[3] = 1u;
        }
        return response_length;
    }
    if (family == WCHLINK_FAMILY_RESET) {
        return wchlink_ack(response, response_capacity, family);
    }
    if (family != WCHLINK_FAMILY_CONTROL || request_length < 4u) {
        return wchlink_ack(response, response_capacity, family);
    }

    switch (request[3]) {
        case WCHLINK_CONTROL_IDENTIFY:
            wchlink_protocol_reset();
            return wchlink_identity(response, response_capacity);
        case WCHLINK_CONTROL_CONNECT:
            rvswd_gpio_init();
            (void)rvswd_gpio_connect();
            wchlink_connected = true;
            return wchlink_connect_reply(response, response_capacity, wchlink_connected);
        case WCHLINK_CONTROL_STOP:
            wchlink_protocol_reset();
            return wchlink_ack(response, response_capacity, family);
        case WCHLINK_CONTROL_HOLD:
        case WCHLINK_CONTROL_RESET_LOW:
            return wchlink_ack(response, response_capacity, family);
        default:
            return wchlink_ack(response, response_capacity, family);
    }
}
