# WCH-Link 动态库探针

`mrs_wchlink_probe` 通过 MounRiver Studio 的 `libmcuupdate.dylib` 调用真实的
WCH-Link 通讯接口，用于复现 MRS 的只读查询、擦除、编程、校验和保护操作。

## 构建

```sh
sh tools/build_mrs_wchlink_probe.sh
```

产物为 `build/tools/mrs_wchlink_probe`。构建依赖 Homebrew 的 `libusb`，运行时
需要本机已安装 MounRiver Studio 2。

## 常用命令

```sh
build/tools/mrs_wchlink_probe check
build/tools/mrs_wchlink_probe erase
build/tools/mrs_wchlink_probe flash --file target.hex
build/tools/mrs_wchlink_probe verify --file target.hex
build/tools/mrs_wchlink_probe protect-enable
build/tools/mrs_wchlink_probe protect-disable
```

CH32V307 默认参数为 `--family 6 --debug-mode 1 --speed 3`。其中 `debug-mode 1`
表示双线调试，`speed 3` 是 MRS 的两线速度参数，两者含义不同。

可以通过以下参数覆盖设备和目标设置：

```text
--serial SERIAL
--location BUS-PORT
--family N
--debug-mode N
--speed N
--address N
--flags N
--clear-type N
```

未指定 `--serial` 时默认选择序列号以 `035` 开头的本项目探针，完整序列号仍可通过 `--serial` 精确指定

工具优先按 USB 序列号解析物理位置，并调用 `jtag_usb_set_location`。macOS 的
DriverKit USB 设备可能被 Homebrew libusb 枚举不到，此时工具仅在 MRS 自身选择
设备，多个同 VID/PID 设备同时连接时应先断开无关设备。

## MRS 调用顺序

`check` 命令复现 MRS 中显式调用的顺序：

1. `McuCompiler_SetTargetChip(family, debug_mode)`
2. `McuCompiler_OpenDevice()`
3. `McuCompiler_GetDeviceVersion()`
4. `McuCompiler_SetTwolineLowSpeed(family, speed)`
5. `McuCompiler_SetChipType(family, subtype, false)`
6. 调用具体的 `MRSFunc_*` 操作

MRS 的 `SetChipType` 实际先发送 `81 0d 01 04`。手动检查使用 `skip=false`，随后会
发送 `81 11 01 <family>` 查询扩展信息。官方 LinkE 连接 CH592 时，对该查询返回：

```text
82 0d 01 ff 92 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

`MRSFunc_FlashOperationExB` 是 MRS 编程主路径，但普通目标族分支会转入
`MRSFunc_FlashOperation`，再以 `skip=false` 调用 `McuCompiler_SetChipType`，要求
完整 20 字节扩展信息。CH59x 必须按官方 LinkE 的专用格式回复，不能套用其他芯片族的
4 字节拒绝帧

`flash` 使用 `MRSFunc_FlashOperationExB`，默认 flags `0x06`，即编程和校验。
`verify` 使用同一入口但只保留校验标志。CH59x 工程的普通全擦、编程、校验和复位
使用 `--flags 0x0f`。

`MRSFunc_FlashOperationExB` 的 flags 定义如下：

```text
0x80 关闭 Link 电源输出
0x40 掉电方式清 CodeFlash
0x20 解除读保护
0x10 OptEnd
0x08 McuCompiler_Clear
0x04 编程
0x02 校验
0x01 复位并运行
```

## 进入 CH32X035 USB ISP

模拟 Link 也接受官方 LinkE 的 `81 0f 01 01` SetIAPMode 触发。该命令只负责退出
WCH-Link 工作态，固件随后进入自身 CH32X035 的 USB ISP，后续镜像传输仍使用
`wchisp`，不是官方 LinkE IAP 数据协议：

```sh
sh tools/wchlink_enter_iap.sh 035CDAB8706E
```

脚本会在 10 秒窗口内自动重试 `wchisp flash`，也可以把第二个参数换成指定镜像路径

CDC-ACM 只保留数据回环，不触发 ISP，`WCHISP` 字节会按普通 CDC 数据原样回显

该入口只影响模拟 Link 自身，不会操作 SWD 目标芯片。发送命令前应关闭 MRS、OpenOCD
和 `wlink`，避免它们占用同一个 USB CDC 或 WCH-Link 设备
