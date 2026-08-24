#!/bin/sh

set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_directory=$(CDPATH= cd -- "$script_directory/../.." && pwd)
output_directory="$project_directory/build/tools"
output_path="$output_directory/mrs_wchlink_probe"
trace_output_path="$output_directory/mrs_usb_trace.dylib"
libusb_prefix=$(brew --prefix libusb 2>/dev/null || printf '%s' /opt/homebrew/opt/libusb)

mkdir -p "$output_directory"
clang -std=c11 -Wall -Wextra -Wpedantic -Wno-cast-function-type \
    -I"$libusb_prefix/include/libusb-1.0" \
    "$script_directory/mrs_wchlink_probe.c" -o "$output_path" \
    -L"$libusb_prefix/lib" -lusb-1.0 -ldl
# 注入到 MRS 后才由目标进程提供 interpose 的符号，构建期保留未解析引用
clang -std=c11 -Wall -Wextra -Wpedantic \
    -DMRS_TRACE_LIBUSB_PATH=\"$libusb_prefix/lib/libusb-1.0.0.dylib\" \
    "$script_directory/mrs_usb_trace.c" -o "$trace_output_path" \
    -dynamiclib -Wl,-undefined,dynamic_lookup -L"$libusb_prefix/lib" -lusb-1.0 -ldl
printf '%s\n' "$output_path"
printf '%s\n' "$trace_output_path"
