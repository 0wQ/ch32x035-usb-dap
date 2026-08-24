#!/bin/sh

set -eu

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_directory=$(CDPATH= cd -- "$script_directory/../.." && pwd)
source_app="/Applications/MounRiver Studio 2.app"
target_app="$project_directory/.tmp/MounRiver Studio 2 Debug.app"
entitlements_path="$script_directory/mrs_debug.entitlements.plist"
plugin_binary="$target_app/Contents/Frameworks/MounRiver Studio 2 Helper (Plugin).app/Contents/MacOS/MounRiver Studio 2 Helper (Plugin)"

if [ -e "$target_app" ]; then
    printf '调试副本已存在: %s\n' "$target_app" >&2
    printf '%s\n' '请先确认该副本不在运行，再手动移除后重新执行' >&2
    exit 1
fi

if [ ! -x "$source_app/Contents/MacOS/Electron" ]; then
    printf '找不到 MounRiver Studio: %s\n' "$source_app" >&2
    exit 1
fi

mkdir -p "$(dirname -- "$target_app")"
ditto "$source_app" "$target_app"
codesign --force --sign - --entitlements "$entitlements_path" "$plugin_binary"
codesign --verify --verbose=2 "$plugin_binary"
codesign -d --entitlements :- "$plugin_binary" 2>&1
printf '调试副本已创建: %s\n' "$target_app"
