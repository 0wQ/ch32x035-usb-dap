#include <libusb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_query(const char *serial, const char *operation) {
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
    unsigned char erase_request[] = {0x81, 0x0d, 0x02, 0x08, 0x0b};
    unsigned char set_chip_type_request[] = {0x81, 0x0d, 0x01, 0x04};
    unsigned char read_protection_request[] = {0x81, 0x06, 0x01, 0x01};
    unsigned char basic_erase_request[] = {0x81, 0x02, 0x01, 0x01};
    unsigned char *requests[7] = {chip_info_request};
    int request_lengths[7] = {sizeof(chip_info_request)};
    size_t request_count = 1u;
    bool read_only = false;
    bool identify_loop = false;
    bool mrs_connect_read = false;
    bool mrs_clear_sequence = false;
    bool no_response = strcmp(operation, "iap-mode") == 0;
    unsigned char response[64] = {0};
    unsigned int timeout = 3000u;
    int transferred = 0;
    int result = 1;
    ssize_t count;

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

int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s SERIAL [identify|identify-three|identify-loop|soft-reset|connect-soft-reset|debugger-mode|iap-mode|read-only|stale-identify|chip-info|erase|mrs-erase|set-chip-type|mrs-sequence|mrs-connect-20|mrs-clear-sequence]\n", argv[0]);
        return 2;
    }
    return run_query(argv[1], argc == 3 ? argv[2] : "chip-info");
}
