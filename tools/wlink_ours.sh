#!/bin/sh

set -eu

readonly project_link_serial_prefix='035'
readonly target_power_cycle_delay_seconds='0.1'

for argument in "$@"; do
    case "$argument" in
        --device | --device=* | -d | -d*)
            printf '%s\n' "Do not pass --device or -d to this wrapper" >&2
            exit 2
            ;;
    esac
done

if ! command -v wlink >/dev/null 2>&1; then
    printf '%s\n' 'wlink was not found in PATH' >&2
    exit 127
fi

if ! device_list=$(wlink list); then
    printf '%s\n' 'Failed to enumerate WCH-Link devices' >&2
    exit 1
fi

device=$(printf '%s\n' "$device_list" | awk -v prefix="$project_link_serial_prefix" '
    $0 ~ ("Serial " prefix "[0-9A-Fa-f]{9}( |$)") {
        line = $0
        sub(/^.*<WCH-Link#/, "", line)
        sub(/[[:space:]].*$/, "", line)
        if (line ~ /^[0-9]+$/) {
            print line
        }
    }
')

device_count=$(printf '%s\n' "$device" | awk 'NF { count += 1 } END { print count + 0 }')
if [ "$device_count" -ne 1 ]; then
    printf '%s\n' "Expected one project WCH-Link with serial prefix $project_link_serial_prefix, found $device_count" >&2
    printf '%s\n' "$device_list" >&2
    exit 1
fi

if [ "$#" -eq 2 ] && [ "$1" = 'set-power' ] && [ "$2" = 'restart5v' ]; then
    wlink --device "$device" set-power disable5v
    sleep "$target_power_cycle_delay_seconds"
    exec wlink --device "$device" set-power enable5v
fi

exec wlink --device "$device" "$@"
