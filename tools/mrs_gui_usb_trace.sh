#!/bin/sh

set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
app_path="/Applications/MounRiver Studio 2.app"
trace_path="$script_directory/../build/tools/mrs_usb_trace.dylib"
log_path="${1:-$script_directory/../build/tools/mrs_gui_usb_trace.log}"

if [ ! -x "$app_path/Contents/MacOS/Electron" ]; then
    printf '找不到 MounRiver Studio: %s\\n' "$app_path" >&2
    exit 1
fi

if [ ! -f "$trace_path" ]; then
    "$script_directory/build_mrs_wchlink_probe.sh" >/dev/null
fi

mkdir -p "$(dirname -- "$log_path")"
printf 'MRS USB trace log: %s\\n' "$log_path"
printf '请在 MRS 中依次执行单独全擦和编程加校验，结束后关闭 MRS\\n'

DYLD_INSERT_LIBRARIES="$trace_path${DYLD_INSERT_LIBRARIES:+:$DYLD_INSERT_LIBRARIES}" \
    "$app_path/Contents/MacOS/Electron" >>"$log_path" 2>&1 &
mrs_pid=$!
printf 'MRS PID: %s\\n' "$mrs_pid"
wait "$mrs_pid"
