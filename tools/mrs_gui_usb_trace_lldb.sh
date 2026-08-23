#!/bin/sh

set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
trace_script="$script_directory/mrs_gui_usb_trace.lldb"
log_path="${1:-$script_directory/../build/tools/mrs_gui_usb_trace_lldb.log}"
profile_path="$script_directory/../.tmp/mrs-debug-profile"

target_pid=$(ps -axo pid=,args= |
    awk -v profile="$profile_path"
        '/MounRiver Studio 2 Helper \(Plugin\)/ && index($0, profile) {print $1; exit}')
if [ -z "$target_pid" ]; then
    printf '%s\n' '未找到隔离调试副本中的 MounRiver Studio Plugin helper' >&2
    exit 1
fi

mkdir -p "$(dirname -- "$log_path")"
printf '附加 MRS Plugin helper PID %s\n' "$target_pid"
printf 'LLDB 日志: %s\n' "$log_path"
printf '%s\n' '输入管理员密码后保持此终端运行，再在 MRS 中执行目标操作'

exec sudo /usr/bin/lldb -p "$target_pid" -s "$trace_script" 2>&1 | tee "$log_path"
