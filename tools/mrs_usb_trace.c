#include <stdbool.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>

typedef struct libusb_device_handle libusb_device_handle;
typedef int (*libusb_bulk_transfer_fn)(libusb_device_handle *, unsigned char,
                                       unsigned char *, int, int *, unsigned int);
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
extern int libusb_control_transfer(libusb_device_handle *handle,
                                   unsigned char request_type,
                                   unsigned char request, unsigned short value,
                                   unsigned short index, unsigned char *data,
                                   unsigned short length,
                                   unsigned int timeout);

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
    static void *library;
    void *symbol;

    // 优先解析插入层之后的真实符号，避免再次解析到当前拦截函数
    symbol = dlsym(RTLD_NEXT, name);
    if (symbol != NULL) {
        return symbol;
    }

    if (library == NULL) {
        library =
            dlopen(MRS_TRACE_LIBUSB_PATH, RTLD_NOW | RTLD_LOCAL | RTLD_FIRST);
        if (library == NULL) {
            fprintf(stderr, "cannot open %s: %s\n", MRS_TRACE_LIBUSB_PATH,
                    dlerror());
            return NULL;
        }
    }
    return dlsym(library, name);
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

DYLD_INTERPOSE(trace_libusb_bulk_transfer, libusb_bulk_transfer);
DYLD_INTERPOSE(trace_libusb_control_transfer, libusb_control_transfer);
