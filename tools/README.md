# 开发工具

`tools/` 按执行对象组织。根目录只保留本索引，避免 MRS 调试、WCH-Link 协议和 CDC/UART
测试脚本混在一起。

| 目录 | 内容 | 主要入口 |
| --- | --- | --- |
| [`mrs/`](mrs/) | MRS 动态库探针、GUI 注入和 LLDB 跟踪 | `build_mrs_wchlink_probe.sh`、`mrs_gui_usb_trace.sh` |
| [`wchlink/`](wchlink/) | 项目 WCH-Link、ISP 和原始协议诊断 | `wlink_ours.sh`、`wchlink_enter_iap.sh` |
| [`transport/`](transport/) | CDC/UART 收发、ESP32 协议仿真和 USBFS 基准 | `usb_cdc_loopback_test.py`、`usbfs_port_benchmark_prototype.py` |

## MRS

构建 MRS 动态库探针和 USB trace：

```sh
sh tools/mrs/build_mrs_wchlink_probe.sh
```

产物位于 `build/tools/`，常用命令为：

```sh
build/tools/mrs_wchlink_probe check
build/tools/mrs_wchlink_probe erase
build/tools/mrs_wchlink_probe flash --file target.hex
build/tools/mrs_wchlink_probe verify --file target.hex
build/tools/mrs_wchlink_probe protect-enable
build/tools/mrs_wchlink_probe protect-disable
```

可通过 `--serial`、`--location`、`--family`、`--debug-mode`、`--speed`、`--address`、
`--flags` 和 `--clear-type` 固定目标和 MRS 参数。多个同 VID/PID WCH-Link 同时连接时，
优先指定完整序列号，必要时断开无关设备。

`mrs/lldb/` 保存所有 LLDB 断点脚本。创建带调试权限的 MRS 副本后，可使用：

```sh
sh tools/mrs/create_mrs_debug_copy.sh
sh tools/mrs/mrs_gui_usb_trace.sh
sh tools/mrs/mrs_gui_usb_trace_lldb.sh
```

## WCH-Link

`wlink_ours.sh` 按项目探针序列号前缀选择设备，适合项目探针的手工诊断和第三方交叉参考，
不作为 RVSWD 协议采样的执行器：

```sh
tools/wchlink/wlink_ours.sh --chip CH582 --speed low status
tools/wchlink/wlink_ours.sh dump --chip CH582 0x0a00 256 --out target.bin
```

项目固件维护使用官方 SetIAPMode 进入 CH32X035 USB ISP，随后在 10 秒窗口内写入新固件：

```sh
sh tools/wchlink/wchlink_enter_iap.sh 035CDAB8706E
```

RVSWD Logic 抓包、CSV 分类和原始采样归档已移到相邻的
[`wch-linke-captures`](../../../wch-linke-captures/)。其运行器仍调用本工程构建的
`build/tools/mrs_wchlink_probe` 和 MRS 自带 OpenOCD，但运行记录、CSV 与临时页镜像不写回本工程。

## 传输测试

`transport/` 中的脚本可直接从项目根目录执行。它们共享
`usb_cdc_loopback_test.py` 的串口配置和字节校验逻辑：

```sh
python3 tools/transport/usb_cdc_loopback_test.py /dev/cu.usbmodem...
python3 tools/transport/usb_uart_duplex_test.py /dev/cu.usbmodem... /dev/cu.usbserial...
python3 tools/transport/uart_full_duplex_test.py /dev/cu.usbmodem... /dev/cu.usbserial...
```

`esp32s3_flash_pattern_test.py` 用外部 UART 模拟 ESP32-S3 烧录交互；
`usbfs_port_benchmark_prototype.py` 会临时构建和刷入测试固件，除非传入
`--keep-benchmark-firmware`，否则结束时恢复正式固件配置。
