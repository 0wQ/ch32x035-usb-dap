#include <libusb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARTIAL_WRITE_DATA_LENGTH 4u

static int hex_nibble(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static bool parse_partial_write_data(const char *text,
                                     unsigned char data[PARTIAL_WRITE_DATA_LENGTH]) {
    if (strlen(text) != PARTIAL_WRITE_DATA_LENGTH * 2u) {
        return false;
    }
    for (size_t index = 0u; index < PARTIAL_WRITE_DATA_LENGTH; ++index) {
        int high = hex_nibble(text[index * 2u]);
        int low = hex_nibble(text[index * 2u + 1u]);

        if (high < 0 || low < 0) {
            return false;
        }
        data[index] = (unsigned char)((high << 4u) | low);
    }
    return true;
}

static int run_query(const char *serial, const char *operation,
                     unsigned char partial_write_family,
                     const unsigned char partial_write_data[PARTIAL_WRITE_DATA_LENGTH]) {
    libusb_context *context = NULL;
    libusb_device **devices = NULL;
    libusb_device_handle *handle = NULL;
    unsigned char identify_request[] = {0x81, 0x0d, 0x01, 0x01};
    unsigned char connect_request[] = {0x81, 0x0d, 0x01, 0x02};
    unsigned char stop_request[] = {0x81, 0x0d, 0x01, 0xff};
    unsigned char soft_reset_request[] = {0x81, 0x0b, 0x01, 0x01};
    unsigned char debugger_mode_request[] = {0x81, 0x0f, 0x01, 0x02};
    unsigned char iap_mode_request[] = {0x81, 0x0f, 0x01, 0x01};
    unsigned char chip_info_request[] = {0x81, 0x11, 0x01, 0x0b};
    unsigned char speed_request[] = {0x81, 0x0c, 0x02, 0x0b, 0x03};
    unsigned char partial_write_speed_request[] = {0x81, 0x0c, 0x02, 0x07, 0x03};
    unsigned char erase_request[] = {0x81, 0x0d, 0x02, 0x08, 0x0b};
    unsigned char set_chip_type_request[] = {0x81, 0x0d, 0x01, 0x04};
    unsigned char read_protection_request[] = {0x81, 0x06, 0x01, 0x01};
    unsigned char basic_erase_request[] = {0x81, 0x02, 0x01, 0x01};
    unsigned char partial_write_request[] = {0x81, 0x0a, 0x05, 0x00, 0x00, 0x0a, 0xac, 0x04};
    unsigned char *requests[7] = {chip_info_request};
    int request_lengths[7] = {sizeof(chip_info_request)};
    size_t request_count = 1u;
    bool read_only = false;
    bool identify_loop = false;
    bool mrs_connect_read = false;
    bool mrs_clear_sequence = false;
    bool partial_write = strcmp(operation, "partial-write") == 0;
    bool no_response = strcmp(operation, "iap-mode") == 0;
    unsigned char response[64] = {0};
    unsigned int timeout = 3000u;
    int transferred = 0;
    int result = 1;
    ssize_t count;

    // 0x0A 测试必须先按实际目标族选择 RVSWD 传输格式，默认保留 CH582 的 long-frame 提示
    partial_write_speed_request[3] = partial_write_family;

    if (strcmp(operation, "identify") == 0) {
        requests[0] = identify_request;
        request_lengths[0] = sizeof(identify_request);
    } else if (strcmp(operation, "soft-reset") == 0) {
        requests[0] = soft_reset_request;
        request_lengths[0] = sizeof(soft_reset_request);
    } else if (strcmp(operation, "connect-soft-reset") == 0) {
        requests[0] = connect_request;
        requests[1] = soft_reset_request;
        request_lengths[0] = sizeof(connect_request);
        request_lengths[1] = sizeof(soft_reset_request);
        request_count = 2u;
    } else if (strcmp(operation, "debugger-mode") == 0) {
        requests[0] = debugger_mode_request;
        request_lengths[0] = sizeof(debugger_mode_request);
    } else if (strcmp(operation, "iap-mode") == 0) {
        requests[0] = iap_mode_request;
        request_lengths[0] = sizeof(iap_mode_request);
    } else if (strcmp(operation, "read-only") == 0) {
        read_only = true;
        request_count = 0u;
    } else if (strcmp(operation, "identify-three") == 0) {
        requests[0] = identify_request;
        requests[1] = identify_request;
        requests[2] = identify_request;
        request_lengths[0] = sizeof(identify_request);
        request_lengths[1] = sizeof(identify_request);
        request_lengths[2] = sizeof(identify_request);
        request_count = 3u;
    } else if (strcmp(operation, "identify-loop") == 0) {
        requests[0] = identify_request;
        request_lengths[0] = sizeof(identify_request);
        request_count = 100u;
        identify_loop = true;
    } else if (strcmp(operation, "stale-identify") == 0) {
        requests[0] = stop_request;
        requests[1] = identify_request;
        request_lengths[0] = sizeof(stop_request);
        request_lengths[1] = sizeof(identify_request);
        request_count = 2u;
    } else if (strcmp(operation, "erase") == 0) {
        requests[0] = erase_request;
        request_lengths[0] = sizeof(erase_request);
        timeout = 15000u;
    } else if (strcmp(operation, "mrs-erase") == 0) {
        requests[0] = set_chip_type_request;
        requests[1] = read_protection_request;
        requests[2] = basic_erase_request;
        request_lengths[0] = sizeof(set_chip_type_request);
        request_lengths[1] = sizeof(read_protection_request);
        request_lengths[2] = sizeof(basic_erase_request);
        request_count = 3u;
        timeout = 15000u;
    } else if (strcmp(operation, "set-chip-type") == 0) {
        requests[0] = set_chip_type_request;
        request_lengths[0] = sizeof(set_chip_type_request);
        timeout = 3000u;
    } else if (strcmp(operation, "mrs-sequence") == 0) {
        requests[0] = identify_request;
        requests[1] = speed_request;
        requests[2] = connect_request;
        requests[3] = chip_info_request;
        request_lengths[0] = sizeof(identify_request);
        request_lengths[1] = sizeof(speed_request);
        request_lengths[2] = sizeof(connect_request);
        request_lengths[3] = sizeof(chip_info_request);
        request_count = 4u;
    } else if (strcmp(operation, "mrs-connect-20") == 0) {
        requests[0] = identify_request;
        requests[1] = speed_request;
        requests[2] = connect_request;
        request_lengths[0] = sizeof(identify_request);
        request_lengths[1] = sizeof(speed_request);
        request_lengths[2] = sizeof(connect_request);
        request_count = 3u;
        mrs_connect_read = true;
        request_lengths[0] = sizeof(identify_request);
    } else if (strcmp(operation, "mrs-clear-sequence") == 0) {
        requests[0] = identify_request;
        requests[1] = speed_request;
        requests[2] = connect_request;
        requests[3] = chip_info_request;
        requests[4] = basic_erase_request;
        requests[5] = connect_request;
        requests[6] = stop_request;
        request_lengths[0] = sizeof(identify_request);
        request_lengths[1] = sizeof(speed_request);
        request_lengths[2] = sizeof(connect_request);
        request_lengths[3] = sizeof(chip_info_request);
        request_lengths[4] = sizeof(basic_erase_request);
        request_lengths[5] = sizeof(connect_request);
        request_lengths[6] = sizeof(stop_request);
        request_count = 7u;
        mrs_clear_sequence = true;
    } else if (partial_write) {
        requests[0] = identify_request;
        requests[1] = partial_write_speed_request;
        requests[2] = connect_request;
        requests[3] = partial_write_request;
        request_lengths[0] = sizeof(identify_request);
        request_lengths[1] = sizeof(speed_request);
        request_lengths[2] = sizeof(connect_request);
        request_lengths[3] = sizeof(partial_write_request);
        request_count = 4u;
    } else if (strcmp(operation, "chip-info") != 0) {
        fprintf(stderr, "unknown operation: %s\n", operation);
        return 2;
    }
    if (libusb_init(&context) != 0) {
        return 1;
    }
    count = libusb_get_device_list(context, &devices);
    for (ssize_t index = 0; index < count; ++index) {
        struct libusb_device_descriptor descriptor;
        unsigned char device_serial[256] = {0};
        if (libusb_get_device_descriptor(devices[index], &descriptor) != 0 ||
            descriptor.idVendor != 0x1a86u || descriptor.idProduct != 0x8010u ||
            descriptor.iSerialNumber == 0u ||
            libusb_open(devices[index], &handle) != 0) {
            continue;
        }
        if (libusb_get_string_descriptor_ascii(handle, descriptor.iSerialNumber,
                                               device_serial, sizeof(device_serial)) > 0 &&
            strcmp((const char *)device_serial, serial) == 0) {
            break;
        }
        libusb_close(handle);
        handle = NULL;
    }
    if (handle == NULL) {
        fprintf(stderr, "WCH-Link not found: %s\n", serial);
        goto finish;
    }
    if (libusb_claim_interface(handle, 0) != 0) {
        fprintf(stderr, "cannot claim interface\n");
        goto finish;
    }
    if (read_only) {
        int transfer_result = libusb_bulk_transfer(handle, 0x81u, response, sizeof(response),
                                                   &transferred, timeout);

        if (transfer_result != 0) {
            fprintf(stderr, "response transfer failed: result=%d transferred=%d\n",
                    transfer_result, transferred);
            goto release;
        }
        printf("response_length=%d\nresponse=", transferred);
        for (int index = 0; index < transferred; ++index) {
            printf("%02x%s", response[index], index + 1 == transferred ? "\n" : " ");
        }
        result = 0;
        goto release;
    }
    for (size_t request_index = 0u; request_index < request_count; ++request_index) {
        size_t request_slot = identify_loop ? 0u : request_index;
        int transfer_result = libusb_bulk_transfer(
            handle, 0x01u, requests[request_slot], request_lengths[request_slot],
            &transferred, timeout);
        if (transfer_result != 0 || transferred != request_lengths[request_slot]) {
            fprintf(stderr, "command transfer failed: iteration=%zu result=%d transferred=%d\n",
                    request_index + 1u, transfer_result, transferred);
            goto release;
        }
        if (no_response) {
            printf("request=81 0f 01 01\nresponse=not-read\n");
            result = 0;
            goto release;
        }
        if (strcmp(operation, "stale-identify") == 0 && request_index == 0u) {
            printf("request=");
            for (int index = 0; index < request_lengths[request_index]; ++index) {
                printf("%02x%s", requests[request_index][index],
                       index + 1 == request_lengths[request_index] ? "\n" : " ");
            }
            printf("response=not-read\n");
            continue;
        }
        int response_capacity = sizeof(response);
        if (mrs_connect_read) {
            response_capacity = request_index == 0u ? 10u
                                : request_index == 2u ? 20u
                                : 4u;
        } else if (mrs_clear_sequence) {
            static const int mrs_response_capacities[] = {10, 4, 8, 20, 4, 8, 4};
            response_capacity = mrs_response_capacities[request_index];
        }
        transfer_result = libusb_bulk_transfer(handle, 0x81u, response,
                                               response_capacity,
                                               &transferred, timeout);
        if (transfer_result != 0) {
            fprintf(stderr, "response transfer failed: result=%d transferred=%d\n",
                    transfer_result, transferred);
            goto release;
        }
        if (!identify_loop) {
            printf("request=");
            for (int index = 0; index < request_lengths[request_slot]; ++index) {
                printf("%02x%s", requests[request_slot][index],
                       index + 1 == request_lengths[request_slot] ? "\n" : " ");
            }
            printf("response_length=%d\nresponse=", transferred);
            for (int index = 0; index < transferred; ++index) {
                printf("%02x%s", response[index],
                       index + 1 == transferred ? "\n" : " ");
            }
        }
        if (partial_write && request_index == request_count - 1u) {
            transfer_result = libusb_bulk_transfer(
                handle, 0x02u, (unsigned char *)partial_write_data,
                PARTIAL_WRITE_DATA_LENGTH,
                &transferred, timeout);
            printf("partial_data_result=%d transferred=%d\n", transfer_result,
                   transferred);
            transfer_result = libusb_bulk_transfer(
                handle, 0x82u, response, sizeof(response), &transferred,
                timeout);
            printf("partial_response_result=%d transferred=%d\n",
                   transfer_result, transferred);
            if (transfer_result == 0) {
                printf("partial_response=");
                for (int index = 0; index < transferred; ++index) {
                    printf("%02x%s", response[index],
                           index + 1 == transferred ? "\n" : " ");
                }
            }
            result = transfer_result == 0 ? 0 : 1;
            goto release;
        }
    }
    if (identify_loop) {
        printf("identify_responses=%zu\n", request_count);
    }
    result = 0;

release:
    libusb_release_interface(handle, 0);
finish:
    if (handle != NULL) {
        libusb_close(handle);
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    return result;
}

static void print_usage(const char *program) {
    fprintf(stderr,
            "usage: %s SERIAL [identify|identify-three|identify-loop|soft-reset|connect-soft-reset|debugger-mode|iap-mode|read-only|partial-write|stale-identify|chip-info|erase|mrs-erase|set-chip-type|mrs-sequence|mrs-connect-20|mrs-clear-sequence] [partial-family] [partial-data-hex]\n",
            program);
}

int main(int argc, char **argv) {
    const char *operation;
    unsigned long family = 0x07u;
    unsigned char partial_write_data[PARTIAL_WRITE_DATA_LENGTH] = {0x73, 0x00, 0x10, 0x00};
    char *end = NULL;

    if (argc == 2 && (strcmp(argv[1], "--help") == 0 ||
                      strcmp(argv[1], "-h") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc < 2 || argc > 5) {
        print_usage(argv[0]);
        return 2;
    }
    operation = argc >= 3 ? argv[2] : "chip-info";
    if (argc >= 4) {
        family = strtoul(argv[3], &end, 0);
        if (end == argv[3] || *end != '\0' || family > 0xffu) {
            fprintf(stderr, "invalid partial-family: %s\n", argv[3]);
            return 2;
        }
        if (strcmp(operation, "partial-write") != 0) {
            fprintf(stderr, "partial-family only applies to partial-write operations\n");
            return 2;
        }
    }
    if (argc == 5 && !parse_partial_write_data(argv[4], partial_write_data)) {
        fprintf(stderr, "partial-data-hex must contain exactly 8 hexadecimal digits\n");
        return 2;
    }
    return run_query(argv[1], operation, (unsigned char)family, partial_write_data);
}
