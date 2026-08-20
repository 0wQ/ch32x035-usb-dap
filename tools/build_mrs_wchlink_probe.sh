#!/bin/sh

set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
output_directory="$script_directory/../build/tools"
output_path="$output_directory/mrs_wchlink_probe"
trace_output_path="$output_directory/mrs_usb_trace.dylib"
libusb_prefix=$(brew --prefix libusb 2>/dev/null || printf '%s' /opt/homebrew/opt/libusb)

mkdir -p "$output_directory"
clang -std=c11 -Wall -Wextra -Wpedantic -Wno-cast-function-type \
    -I"$libusb_prefix/include/libusb-1.0" \
    "$script_directory/mrs_wchlink_probe.c" -o "$output_path" \
    -L"$libusb_prefix/lib" -lusb-1.0 -ldl
clang -std=c11 -Wall -Wextra -Wpedantic \
    -DMRS_TRACE_LIBUSB_PATH=\"$libusb_prefix/lib/libusb-1.0.0.dylib\" \
    "$script_directory/mrs_usb_trace.c" -o "$trace_output_path" \
    -dynamiclib -L"$libusb_prefix/lib" -lusb-1.0 -ldl
printf '%s\n' "$output_path"
printf '%s\n' "$trace_output_path"
