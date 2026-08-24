#!/bin/sh

set -eu

if [ "$#" -gt 2 ]; then
    printf '%s\n' "用法: $0 [WCH-Link 序列号] [firmware.bin]" >&2
    exit 2
fi

serial=${1:-035CDAB8706E}
project_directory=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
firmware_path=${2:-$project_directory/build/release/firmware.bin}

if [ ! -f "$firmware_path" ]; then
    printf '%s\n' "固件文件不存在: $firmware_path" >&2
    exit 1
fi

"$project_directory/build/tools/wchlink_raw_query" "$serial" iap-mode

# USB ISP 重枚举有短暂延迟，在 Boot 区窗口内快速重试整片写入
attempt=1
while [ "$attempt" -le 40 ]; do
    if wchisp flash "$firmware_path"; then
        exit 0
    fi
    attempt=$((attempt + 1))
    sleep 0.1
done

printf '%s\n' '未能在 USB ISP 窗口内找到设备' >&2
exit 1
