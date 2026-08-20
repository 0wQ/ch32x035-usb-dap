#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>
#include <libusb.h>

typedef int (*open_device_fn)(void);
typedef int (*close_device_fn)(void);
typedef int (*get_device_version_fn)(int *, int *);
typedef int (*set_target_chip_fn)(int, int);
typedef int (*set_two_line_speed_fn)(int, int);
typedef int (*set_chip_type_fn)(int, uint8_t *, bool);
typedef int (*reset_fn)(void);
typedef int (*download_fn)(int, int, int, const char *);
typedef int (*flash_operation_fn)(int, int, int, int, const char *);
typedef int (*flash_operation_ex_fn)(int, int, int, int, const char *);
typedef int (*clear_code_flash_fn)(int, int, int);
typedef int (*query_protection_fn)(int, int);
typedef int (*enable_protection_fn)(int, int);
typedef int (*disable_protection_fn)(int, int);
typedef int (*get_mem_type_fn)(int, int);
typedef int (*set_mem_type_fn)(int, int, int);
typedef int (*disable_debug_fn)(int, int);
typedef int (*get_chip_id_fn)(void);
typedef void (*set_location_fn)(const char *);

struct mrs_api {
    void *library;
    open_device_fn open_device;
    close_device_fn close_device;
    get_device_version_fn get_device_version;
    set_target_chip_fn set_target_chip;
    set_two_line_speed_fn set_two_line_speed;
    set_chip_type_fn set_chip_type;
    reset_fn reset;
    download_fn download;
    flash_operation_fn flash_operation;
    flash_operation_ex_fn flash_operation_ex;
    clear_code_flash_fn clear_code_flash;
    query_protection_fn query_protection;
    enable_protection_fn enable_protection;
    disable_protection_fn disable_protection;
    get_mem_type_fn get_mem_type;
    set_mem_type_fn set_mem_type;
    disable_debug_fn disable_debug;
    get_chip_id_fn get_chip_id;
    set_location_fn set_location;
};

static const char *default_library_path =
    "/Volumes/apfs/app/MounRiver Studio 2.app/Contents/Resources/app/resources/"
    "darwin/components/WCH/Others/CommunicationLib/default/libmcuupdate.dylib";
static const char *default_location = NULL;
static const char *default_serial = "035";
static const int default_family = 6;
static const int default_speed = 3;
static const int default_address = 0x08000000;

static void print_usage(const char *program) {
    fprintf(stderr,
            "usage:\n"
            "  %s check [--library PATH] [--serial SERIAL] [--location BUS-PORT]\n"
            "           [--family N] [--debug-mode N] [--speed N]\n"
            "  %s flash --file PATH [--flags N] [--address N] [--library PATH]\n"
            "           [--serial SERIAL] [--location BUS-PORT] [--family N]\n"
            "           [--debug-mode N] [--speed N]\n"
            "  %s download --file PATH [--address N] [--library PATH]\n"
            "             [--serial SERIAL] [--location BUS-PORT] [--family N]\n"
            "             [--debug-mode N] [--speed N]\n"
            "  %s erase [--clear-type N] [--library PATH] [--location BUS-PORT]\n"
            "           [--serial SERIAL] [--family N] [--debug-mode N] [--speed N]\n"
            "  %s verify --file PATH [--address N] [--library PATH]\n"
            "            [--serial SERIAL] [--location BUS-PORT] [--family N]\n"
            "            [--debug-mode N] [--speed N]\n"
            "  %s reset [--library PATH] [--serial SERIAL] [--location BUS-PORT]\n"
            "           [--family N] [--debug-mode N] [--speed N]\n"
            "  %s protect-enable|protect-disable [--library PATH] [--location BUS-PORT]\n"
            "            [--serial SERIAL] [--family N] [--debug-mode N] [--speed N]\n"
            "\n"
            "default serial: %s\n"
            "default flash flags: 0x06 (program + verify)\n",
            program, program, program, program, program, program, program, default_serial);
}

static void print_symbol_error(const char *name) {
    const char *error = dlerror();
    fprintf(stderr, "missing symbol %s: %s\n", name, error != NULL ? error : "unknown error");
}

static bool load_symbol(void *library, const char *name, void *symbol_storage,
                        size_t symbol_size) {
    void *symbol = dlsym(library, name);
    if (symbol == NULL) {
        print_symbol_error(name);
        return false;
    }
    memcpy(symbol_storage, &symbol,
           symbol_size < sizeof(symbol) ? symbol_size : sizeof(symbol));
    return true;
}

static bool load_api(struct mrs_api *api, const char *path) {
    memset(api, 0, sizeof(*api));
    api->library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (api->library == NULL) {
        fprintf(stderr, "dlopen %s: %s\n", path, dlerror());
        return false;
    }

#define LOAD(name, field)                                                    \
    if (!load_symbol(api->library, name, &api->field, sizeof(api->field))) { \
        dlclose(api->library);                                               \
        api->library = NULL;                                                 \
        return false;                                                        \
    }
    LOAD("McuCompiler_OpenDevice", open_device)
    LOAD("McuCompiler_CloseDevice", close_device)
    LOAD("McuCompiler_GetDeviceVersion", get_device_version)
    LOAD("McuCompiler_SetTargetChip", set_target_chip)
    LOAD("McuCompiler_SetTwolineLowSpeed", set_two_line_speed)
    LOAD("McuCompiler_SetChipType", set_chip_type)
    LOAD("McuCompiler_Reset", reset)
    LOAD("McuCompiler_Download", download)
    LOAD("MRSFunc_FlashOperation", flash_operation)
    LOAD("MRSFunc_FlashOperationExB", flash_operation_ex)
    LOAD("MRSFunc_ClearCodeFlash", clear_code_flash)
    LOAD("MRSFunc_QueryRProtect", query_protection)
    LOAD("MRSFunc_EnableRProtect", enable_protection)
    LOAD("MRSFunc_DisableRProtect", disable_protection)
    LOAD("MRSFunc_GetMemType", get_mem_type)
    LOAD("MRSFunc_SetMemType", set_mem_type)
    LOAD("MRSFunc_DisableDbgInterface", disable_debug)
    LOAD("MRSFunc_GetLinkedMCUID", get_chip_id)
#undef LOAD

    api->set_location = (set_location_fn)dlsym(api->library, "jtag_usb_set_location");
    return true;
}

static void unload_api(struct mrs_api *api) {
    if (api->library != NULL) {
        dlclose(api->library);
        api->library = NULL;
    }
}

static int parse_number(const char *value, int *result) {
    char *end = NULL;
    long parsed = strtol(value, &end, 0);
    if (end == value || *end != '\0' || parsed < INT32_MIN || parsed > INT32_MAX) {
        return -1;
    }
    *result = (int)parsed;
    return 0;
}

static bool serial_matches(const char *serial, const char *device_serial) {
    if (serial == NULL || device_serial == NULL) {
        return false;
    }
    if (strcmp(serial, "035") == 0) {
        return strncmp(device_serial, "035", 3u) == 0 &&
               strlen(device_serial) == 12u;
    }
    return strcmp(device_serial, serial) == 0;
}

static bool resolve_location(const char *serial, char *location, size_t capacity) {
    libusb_context *context = NULL;
    libusb_device **devices = NULL;
    ssize_t device_count;
    bool found = false;

    if (serial == NULL || location == NULL || capacity == 0u ||
        libusb_init(&context) != 0) {
        return false;
    }

    device_count = libusb_get_device_list(context, &devices);
    for (ssize_t index = 0; index < device_count && !found; ++index) {
        struct libusb_device_descriptor descriptor;
        libusb_device_handle *handle = NULL;
        unsigned char device_serial[256] = {0};
        uint8_t ports[8] = {0};
        int port_count;
        int serial_length;
        int written;

        if (libusb_get_device_descriptor(devices[index], &descriptor) != 0 ||
            descriptor.idVendor != 0x1a86u || descriptor.idProduct != 0x8010u ||
            descriptor.iSerialNumber == 0u ||
            libusb_open(devices[index], &handle) != 0) {
            continue;
        }

        serial_length = libusb_get_string_descriptor_ascii(
            handle, descriptor.iSerialNumber, device_serial, sizeof(device_serial));
        if (serial_length <= 0 ||
            !serial_matches(serial, (const char *)device_serial)) {
            libusb_close(handle);
            continue;
        }

        port_count = libusb_get_port_numbers(devices[index], ports, sizeof(ports));
        written = snprintf(location, capacity, "%u", libusb_get_bus_number(devices[index]));
        for (int port = 0; port < port_count && written >= 0 &&
                           (size_t)written < capacity;
             ++port) {
            written += snprintf(location + written, capacity - (size_t)written,
                                "%c%u", port == 0 ? '-' : '.', ports[port]);
        }
        found = written >= 0 && (size_t)written < capacity;
        libusb_close(handle);
    }

    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    return found;
}

static size_t count_wchlink_devices(void) {
    libusb_context *context = NULL;
    libusb_device **devices = NULL;
    ssize_t device_count;
    size_t matching_count = 0u;

    if (libusb_init(&context) != 0) {
        return 0u;
    }
    device_count = libusb_get_device_list(context, &devices);
    for (ssize_t index = 0; index < device_count; ++index) {
        struct libusb_device_descriptor descriptor;

        if (libusb_get_device_descriptor(devices[index], &descriptor) == 0 &&
            descriptor.idVendor == 0x1a86u && descriptor.idProduct == 0x8010u) {
            matching_count++;
        }
    }
    libusb_free_device_list(devices, 1);
    libusb_exit(context);
    return matching_count;
}

static bool parse_common_option(int *index, int argc, char **argv, const char **library_path,
                                const char **location, const char **serial, int *family,
                                int *debug_mode, int *speed, int *address) {
    const char *argument = argv[*index];
    const char *value = NULL;

    if (strcmp(argument, "--library") == 0 || strcmp(argument, "--location") == 0 ||
        strcmp(argument, "--serial") == 0 ||
        strcmp(argument, "--family") == 0 || strcmp(argument, "--debug-mode") == 0 ||
        strcmp(argument, "--speed") == 0 ||
        strcmp(argument, "--address") == 0) {
        if (*index + 1 >= argc) {
            fprintf(stderr, "%s requires a value\n", argument);
            return false;
        }
        value = argv[++*index];
    } else if (strncmp(argument, "--library=", 10) == 0) {
        value = argument + 10;
        argument = "--library";
    } else if (strncmp(argument, "--location=", 11) == 0) {
        value = argument + 11;
        argument = "--location";
    } else if (strncmp(argument, "--serial=", 9) == 0) {
        value = argument + 9;
        argument = "--serial";
    } else if (strncmp(argument, "--family=", 9) == 0) {
        value = argument + 9;
        argument = "--family";
    } else if (strncmp(argument, "--debug-mode=", 13) == 0) {
        value = argument + 13;
        argument = "--debug-mode";
    } else if (strncmp(argument, "--speed=", 8) == 0) {
        value = argument + 8;
        argument = "--speed";
    } else if (strncmp(argument, "--address=", 10) == 0) {
        value = argument + 10;
        argument = "--address";
    } else {
        return false;
    }

    if (strcmp(argument, "--library") == 0) {
        *library_path = value;
    } else if (strcmp(argument, "--location") == 0) {
        *location = value;
    } else if (strcmp(argument, "--serial") == 0) {
        *serial = value;
    } else if (strcmp(argument, "--family") == 0) {
        return parse_number(value, family) == 0;
    } else if (strcmp(argument, "--debug-mode") == 0) {
        return parse_number(value, debug_mode) == 0;
    } else if (strcmp(argument, "--speed") == 0) {
        return parse_number(value, speed) == 0;
    } else {
        return parse_number(value, address) == 0;
    }
    return true;
}

static int open_and_configure(struct mrs_api *api, int family, int debug_mode, int speed,
                              uint8_t *subtype) {
    int link_type = 0;
    int link_mode = 0;
    int result = api->set_target_chip(family, debug_mode);
    printf("set_target_chip(%d,%d)=%d\n", family, debug_mode, result);
    if (result != 0) {
        return result;
    }
    result = api->open_device();
    printf("open=%d\n", result);
    if (result != 0) {
        return result;
    }
    result = api->get_device_version(&link_type, &link_mode);
    printf("get_device_version=%d type=%d mode=%d\n", result, link_type, link_mode);
    result = api->set_two_line_speed(family, speed);
    printf("set_two_line_speed(%d,%d)=%d\n", family, speed, result);
    result = api->set_chip_type(family, subtype, false);
    printf("set_chip_type(%d)=%d subtype=0x%02x\n", family, result, *subtype);
    return result;
}

static int run_check(struct mrs_api *api, int family, int debug_mode, int speed) {
    uint8_t subtype = 0u;
    int linked_mcu_id;
    int result = open_and_configure(api, family, debug_mode, speed, &subtype);
    if (result != 0) {
        api->close_device();
        return result;
    }
    api->close_device();
    printf("close=0\n");
    linked_mcu_id = api->get_chip_id();
    printf("linked_mcu_id=%d (0x%x)\n", linked_mcu_id, linked_mcu_id);
    printf("query_rprotect=%d\n", api->query_protection(family, speed));
    printf("get_mem_type=%d\n", api->get_mem_type(family, speed));
    return 0;
}

int main(int argc, char **argv) {
    const char *command;
    const char *library_path = default_library_path;
    const char *location = default_location;
    const char *serial = default_serial;
    const char *file_path = NULL;
    int family = default_family;
    int debug_mode = 1;
    int speed = default_speed;
    int address = default_address;
    int flags = 0x06;
    int clear_type = 0;
    struct mrs_api api;
    int result;

    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }
    command = argv[1];
    if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0) {
        print_usage(argv[0]);
        return 0;
    }
    for (int index = 2; index < argc; ++index) {
        if (strcmp(argv[index], "--file") == 0) {
            if (++index >= argc) {
                fprintf(stderr, "--file requires a value\n");
                return 2;
            }
            file_path = argv[index];
        } else if (strncmp(argv[index], "--file=", 7) == 0) {
            file_path = argv[index] + 7;
        } else if (strcmp(argv[index], "--flags") == 0 || strcmp(argv[index], "--clear-type") == 0) {
            if (++index >= argc) {
                fprintf(stderr, "%s requires a value\n", argv[index - 1]);
                return 2;
            }
            result = parse_number(argv[index], strcmp(argv[index - 1], "--flags") == 0 ? &flags : &clear_type);
            if (result != 0) {
                fprintf(stderr, "invalid numeric value: %s\n", argv[index]);
                return 2;
            }
        } else if (strncmp(argv[index], "--flags=", 8) == 0) {
            if (parse_number(argv[index] + 8, &flags) != 0) {
                return 2;
            }
        } else if (strncmp(argv[index], "--clear-type=", 13) == 0) {
            if (parse_number(argv[index] + 13, &clear_type) != 0) {
                return 2;
            }
        } else if (!parse_common_option(&index, argc, argv, &library_path, &location,
                                        &serial, &family, &debug_mode, &speed, &address)) {
            fprintf(stderr, "unknown option: %s\n", argv[index]);
            print_usage(argv[0]);
            return 2;
        }
    }

    if (strcmp(command, "flash") == 0 || strcmp(command, "verify") == 0 ||
        strcmp(command, "download") == 0) {
        if (file_path == NULL) {
            fprintf(stderr, "%s requires --file PATH\n", command);
            return 2;
        }
        if (strcmp(command, "verify") == 0) {
            flags = 0x02;
        }
    } else if (strcmp(command, "reset") != 0 && strcmp(command, "erase") != 0 &&
               strcmp(command, "check") != 0 &&
               strcmp(command, "protect-enable") != 0 && strcmp(command, "protect-disable") != 0) {
        fprintf(stderr, "unknown command: %s\n", command);
        print_usage(argv[0]);
        return 2;
    }

    if (!load_api(&api, library_path)) {
        return 1;
    }

    char resolved_location[64];
    if (location == NULL) {
        if (!resolve_location(serial, resolved_location, sizeof(resolved_location))) {
            size_t matching_count = count_wchlink_devices();
            if (matching_count > 1u) {
                fprintf(stderr, "WCH-Link serial not accessible: %s (%zu matching devices)\n",
                        serial, matching_count);
                unload_api(&api);
                return 1;
            }
            fprintf(stderr,
                    "warning: cannot resolve WCH-Link serial through libusb, using MRS "
                    "device selection\n");
        } else {
            location = resolved_location;
        }
    }
    printf("serial=%s\n", serial);
    if (location != NULL && api.set_location != NULL) {
        api.set_location(location);
        printf("location=%s\n", location);
    }

    if (strcmp(command, "check") == 0) {
        result = run_check(&api, family, debug_mode, speed);
        unload_api(&api);
        return result == 0 ? 0 : 1;
    }

    {
        if (strcmp(command, "download") == 0) {
            uint8_t subtype = 0u;
            result = open_and_configure(&api, family, debug_mode, speed, &subtype);
            printf("configure_result=%d subtype=0x%02x\n", result, subtype);
            if (result == 0) {
                result = api.download(0, family, address, file_path);
                printf("download_result=%d\n", result);
            }
            api.close_device();
        } else if (strcmp(command, "reset") == 0) {
            uint8_t subtype = 0u;
            result = open_and_configure(&api, family, debug_mode, speed, &subtype);
            printf("configure_result=%d subtype=0x%02x\n", result, subtype);
            if (result == 0) {
                result = api.reset();
                printf("reset_result=%d\n", result);
            }
            api.close_device();
        } else if (strcmp(command, "flash") == 0) {
            result = api.set_target_chip(family, debug_mode);
            printf("set_target_chip(%d,%d)=%d\n", family, debug_mode, result);
            if (result != 0) {
                unload_api(&api);
                return 1;
            }
            printf("flash_operation(family=%d,speed=%d,flags=0x%02x,address=0x%08x,file=%s)\n",
                   family, speed, flags, (unsigned int)address, file_path);
            result = api.flash_operation_ex(family, speed, flags, address, file_path);
            printf("flash_operation_result=%d\n", result);
        } else if (strcmp(command, "verify") == 0) {
            result = api.set_target_chip(family, debug_mode);
            printf("set_target_chip(%d,%d)=%d\n", family, debug_mode, result);
            if (result != 0) {
                unload_api(&api);
                return 1;
            }
            printf("verify_operation(family=%d,speed=%d,address=0x%08x,file=%s)\n",
                   family, speed, (unsigned int)address, file_path);
            result = api.flash_operation_ex(family, speed, flags, address, file_path);
            printf("verify_operation_result=%d\n", result);
        } else if (strcmp(command, "erase") == 0) {
            result = api.set_target_chip(family, debug_mode);
            printf("set_target_chip(%d,%d)=%d\n", family, debug_mode, result);
            if (result != 0) {
                unload_api(&api);
                return 1;
            }
            result = api.clear_code_flash(family, speed, clear_type);
            printf("clear_code_flash(%d,%d,%d)=%d\n", family, speed, clear_type, result);
        } else if (strcmp(command, "protect-enable") == 0) {
            result = api.set_target_chip(family, debug_mode);
            printf("set_target_chip(%d,%d)=%d\n", family, debug_mode, result);
            if (result != 0) {
                unload_api(&api);
                return 1;
            }
            result = api.enable_protection(family, speed);
            printf("enable_rprotect=%d\n", result);
        } else if (strcmp(command, "protect-disable") == 0) {
            result = api.set_target_chip(family, debug_mode);
            printf("set_target_chip(%d,%d)=%d\n", family, debug_mode, result);
            if (result != 0) {
                unload_api(&api);
                return 1;
            }
            result = api.disable_protection(family, speed);
            printf("disable_rprotect=%d\n", result);
        }
    }
    unload_api(&api);
    return result == 0 ? 0 : 1;
}
