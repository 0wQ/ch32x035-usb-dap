#include <stdbool.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>

typedef struct libusb_device_handle libusb_device_handle;
typedef int (*libusb_bulk_transfer_fn)(libusb_device_handle *, unsigned char,
                                       unsigned char *, int, int *, unsigned int);
typedef int (*libusb_bulk_write_fn)(libusb_device_handle *, unsigned char,
                                    unsigned char *, int, int *, unsigned int);
typedef int (*libusb_bulk_read_fn)(libusb_device_handle *, unsigned char,
                                   unsigned char *, int, int *, unsigned int);
typedef int (*jtag_bulk_fn)(libusb_device_handle *, int, char *, int, int, int *);
typedef int (*libusb_control_transfer_fn)(libusb_device_handle *, unsigned char,
                                          unsigned char, unsigned short,
                                          unsigned short, unsigned char *,
                                          unsigned short, unsigned int);

#ifndef MRS_TRACE_LIBUSB_PATH
#define MRS_TRACE_LIBUSB_PATH "/opt/homebrew/opt/libusb/lib/libusb-1.0.0.dylib"
#endif

#ifndef RTLD_FIRST
#define RTLD_FIRST 0x100
#endif

#define DYLD_INTERPOSE(replacement, replacee)                              \
    __attribute__((used)) static const struct {                            \
        const void *replacement;                                           \
        const void *replacee;                                              \
    } interpose_##replacee __attribute__((section("__DATA,__interpose"))) = { \
        (const void *)(uintptr_t)&replacement,                             \
        (const void *)(uintptr_t)&replacee,                                \
    }

extern int libusb_bulk_transfer(libusb_device_handle *handle,
                                unsigned char endpoint, unsigned char *data,
                                int length, int *transferred,
                                unsigned int timeout);
extern int libusb_bulk_write(libusb_device_handle *handle, unsigned char endpoint,
                             unsigned char *data, int length, int *transferred,
                             unsigned int timeout);
extern int libusb_bulk_read(libusb_device_handle *handle, unsigned char endpoint,
                            unsigned char *data, int length, int *transferred,
                            unsigned int timeout);
extern int jtag_libusb_bulk_write(libusb_device_handle *handle, int endpoint,
                                   char *data, int length, int timeout,
                                   int *transferred);
extern int jtag_libusb_bulk_read(libusb_device_handle *handle, int endpoint,
                                  char *data, int length, int timeout,
                                  int *transferred);
extern int libusb_control_transfer(libusb_device_handle *handle,
                                   unsigned char request_type,
                                   unsigned char request, unsigned short value,
                                   unsigned short index, unsigned char *data,
                                   unsigned short length,
                                   unsigned int timeout);

__attribute__((constructor)) static void trace_loaded(void) {
    char path[96];

    snprintf(path, sizeof(path), "/tmp/mrs_usb_trace-%d.log", getpid());
    // 每个被注入进程独立保存日志，避免 Electron 丢弃 helper 的标准错误
    (void)freopen(path, "a", stderr);
    fprintf(stderr, "USB TRACE loaded pid=%d\n", getpid());
    fflush(stderr);
}

static void dump_bytes(const unsigned char *data, int length) {
    int limit = length < 32 ? length : 32;

    for (int index = 0; index < limit; ++index) {
        fprintf(stderr, "%02x%s", data[index], index + 1 == limit ? "" : " ");
    }
    if (length > limit) {
        fprintf(stderr, " ...");
    }
}

static void *load_libusb_symbol(const char *name) {
    uint32_t image_count = _dyld_image_count();

    // dlsym 会再次命中 interpose，直接从 libusb 的 Mach-O 符号表取原始地址
    for (uint32_t image_index = 0u; image_index < image_count; ++image_index) {
        const struct mach_header *header = _dyld_get_image_header(image_index);
        const struct load_command *command;
        const struct symtab_command *symtab = NULL;
        const uint8_t *cursor;
        intptr_t slide;

        const char *image_name = _dyld_get_image_name(image_index);
        if (header == NULL || image_name == NULL ||
            (strcmp(image_name, MRS_TRACE_LIBUSB_PATH) != 0 &&
             strstr(image_name, "libusb-1.0.0.dylib") == NULL)) {
            continue;
        }
        command = (const struct load_command *)((const uint8_t *)header +
                                                sizeof(struct mach_header_64));
        for (uint32_t command_index = 0u;
             command_index < header->ncmds; ++command_index) {
            if (command->cmd == LC_SYMTAB) {
                symtab = (const struct symtab_command *)command;
                break;
            }
            command = (const struct load_command *)((const uint8_t *)command +
                                                    command->cmdsize);
        }
        if (symtab == NULL) {
            continue;
        }
        slide = _dyld_get_image_vmaddr_slide(image_index);
        cursor = (const uint8_t *)header;
        const struct nlist_64 *symbols =
            (const struct nlist_64 *)(cursor + symtab->symoff);
        const char *strings = (const char *)(cursor + symtab->stroff);
        for (uint32_t symbol_index = 0u; symbol_index < symtab->nsyms;
             ++symbol_index) {
            const char *symbol_name = strings + symbols[symbol_index].n_un.n_strx;
            if (strcmp(symbol_name, name) == 0 ||
                (symbol_name[0] == '_' && strcmp(symbol_name + 1, name) == 0)) {
                return (void *)(uintptr_t)(symbols[symbol_index].n_value + slide);
            }
        }
    }
    fprintf(stderr, "cannot resolve real %s from libusb Mach-O symbols\n", name);
    return NULL;
}

static int trace_libusb_bulk_transfer(libusb_device_handle *handle,
                                      unsigned char endpoint,
                                      unsigned char *data, int length,
                                      int *transferred, unsigned int timeout) {
    static libusb_bulk_transfer_fn real_transfer;
    static _Thread_local bool active;
    int result;

    if (real_transfer == NULL) {
        real_transfer =
            (libusb_bulk_transfer_fn)load_libusb_symbol("libusb_bulk_transfer");
    }
    if (real_transfer == NULL || active) {
        fprintf(stderr, "USB TRACE bulk interpose recursion\n");
        return -99;
    }
    fprintf(stderr, "USB OUT? ep=0x%02x len=%d timeout=%u data=", endpoint, length,
            timeout);
    if ((endpoint & 0x80u) == 0u) {
        dump_bytes(data, length);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "<in>\n");
    }

    active = true;
    result = real_transfer(handle, endpoint, data, length, transferred, timeout);
    active = false;
    fprintf(stderr, "USB DONE ep=0x%02x result=%d transferred=%d data=", endpoint,
            result, transferred != NULL ? *transferred : -1);
    if ((endpoint & 0x80u) != 0u && result == 0 && transferred != NULL) {
        dump_bytes(data, *transferred);
    }
    fprintf(stderr, "\n");
    return result;
}

static int trace_libusb_control_transfer(
    libusb_device_handle *handle, unsigned char request_type,
    unsigned char request, unsigned short value, unsigned short index,
    unsigned char *data, unsigned short length, unsigned int timeout) {
    static libusb_control_transfer_fn real_transfer;
    static _Thread_local bool active;
    int result;

    if (real_transfer == NULL) {
        real_transfer = (libusb_control_transfer_fn)load_libusb_symbol(
            "libusb_control_transfer");
    }
    if (real_transfer == NULL || active) {
        fprintf(stderr, "USB TRACE control interpose recursion\n");
        return -99;
    }
    fprintf(stderr,
            "USB CTRL type=0x%02x req=0x%02x value=0x%04x index=0x%04x len=%u "
            "timeout=%u data=",
            request_type, request, value, index, length, timeout);
    if ((request_type & 0x80u) == 0u) {
        dump_bytes(data, length);
    } else {
        fprintf(stderr, "<in>");
    }
    fprintf(stderr, "\n");
    active = true;
    result = real_transfer(handle, request_type, request, value, index, data, length,
                           timeout);
    active = false;
    fprintf(stderr, "USB CTRL DONE result=%d data=", result);
    if ((request_type & 0x80u) != 0u && result > 0) {
        dump_bytes(data, result);
    }
    fprintf(stderr, "\n");
    return result;
}

static int trace_libusb_bulk_write(libusb_device_handle *handle,
                                   unsigned char endpoint, unsigned char *data,
                                   int length, int *transferred,
                                   unsigned int timeout) {
    static libusb_bulk_write_fn real_write;
    int result;

    if (real_write == NULL) {
        real_write = (libusb_bulk_write_fn)load_libusb_symbol("libusb_bulk_write");
    }
    fprintf(stderr, "USB WRITE ep=0x%02x len=%d data=", endpoint, length);
    dump_bytes(data, length);
    fprintf(stderr, "\n");
    if (real_write == NULL) {
        return -99;
    }
    result = real_write(handle, endpoint, data, length, transferred, timeout);
    fprintf(stderr, "USB WRITE DONE ep=0x%02x result=%d transferred=%d\n",
            endpoint, result, transferred != NULL ? *transferred : -1);
    return result;
}

static int trace_libusb_bulk_read(libusb_device_handle *handle,
                                  unsigned char endpoint, unsigned char *data,
                                  int length, int *transferred,
                                  unsigned int timeout) {
    static libusb_bulk_read_fn real_read;
    int result;

    if (real_read == NULL) {
        real_read = (libusb_bulk_read_fn)load_libusb_symbol("libusb_bulk_read");
    }
    result = real_read == NULL ? -99
                               : real_read(handle, endpoint, data, length,
                                           transferred, timeout);
    fprintf(stderr, "USB READ ep=0x%02x len=%d result=%d transferred=%d data=",
            endpoint, length, result, transferred != NULL ? *transferred : -1);
    if (result == 0 && transferred != NULL) {
        dump_bytes(data, *transferred);
    }
    fprintf(stderr, "\n");
    return result;
}
static int trace_jtag_libusb_bulk_write(libusb_device_handle *handle, int endpoint,
                                        char *data, int length, int timeout,
                                        int *transferred) {
    static jtag_bulk_fn real_write;

    if (real_write == NULL) {
        real_write = (jtag_bulk_fn)dlsym(RTLD_NEXT, "jtag_libusb_bulk_write");
    }
    fprintf(stderr, "JTAG WRITE ep=0x%02x len=%d data=", endpoint, length);
    dump_bytes((const unsigned char *)data, length);
    fprintf(stderr, "\n");
    if (real_write == NULL) {
        fprintf(stderr, "JTAG WRITE original symbol unavailable\n");
        return -99;
    }
    int result = real_write(handle, endpoint, data, length, timeout, transferred);
    fprintf(stderr, "JTAG WRITE DONE result=%d transferred=%d\n", result,
            transferred != NULL ? *transferred : -1);
    return result;
}

static int trace_jtag_libusb_bulk_read(libusb_device_handle *handle, int endpoint,
                                       char *data, int length, int timeout,
                                       int *transferred) {
    static jtag_bulk_fn real_read;
    int result;

    if (real_read == NULL) {
        real_read = (jtag_bulk_fn)dlsym(RTLD_NEXT, "jtag_libusb_bulk_read");
    }
    if (real_read == NULL) {
        fprintf(stderr, "JTAG READ original symbol unavailable\n");
        return -99;
    }
    result = real_read(handle, endpoint, data, length, timeout, transferred);
    fprintf(stderr, "JTAG READ ep=0x%02x len=%d result=%d transferred=%d data=",
            endpoint, length, result, transferred != NULL ? *transferred : -1);
    if (result == 0 && transferred != NULL) {
        dump_bytes((const unsigned char *)data, *transferred);
    }
    fprintf(stderr, "\n");
    return result;
}
DYLD_INTERPOSE(trace_libusb_bulk_transfer, libusb_bulk_transfer);
DYLD_INTERPOSE(trace_libusb_bulk_write, libusb_bulk_write);
DYLD_INTERPOSE(trace_libusb_bulk_read, libusb_bulk_read);
DYLD_INTERPOSE(trace_jtag_libusb_bulk_write, jtag_libusb_bulk_write);
DYLD_INTERPOSE(trace_jtag_libusb_bulk_read, jtag_libusb_bulk_read);
DYLD_INTERPOSE(trace_libusb_control_transfer, libusb_control_transfer);
