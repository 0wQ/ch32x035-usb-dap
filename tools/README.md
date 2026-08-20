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

工具优先按 USB 序列号解析物理位置，并调用 `jtag_usb_set_location`。macOS 的
DriverKit USB 设备可能被 Homebrew libusb 枚举不到，此时工具仅在 MRS 自身选择
设备，多个同 VID/PID 设备同时连接时应先断开无关设备。

## MRS 调用顺序

工具遵循 MRS 扩展中的实际顺序：

1. `McuCompiler_SetTargetChip(family, debug_mode)`
2. `McuCompiler_OpenDevice()`
3. `McuCompiler_GetDeviceVersion()`
4. `McuCompiler_SetTwolineLowSpeed(family, speed)`
5. `McuCompiler_SetChipType(family, subtype, false)`
6. 调用具体的 `MRSFunc_*` 操作

MRS 的 `SetChipType` 实际发送 `81 0d 01 04`，并将该命令作为首次目标连接入口。探针
需要在回复 `82 0d` 中回显此前设置的目标 family，随后 MRS 才会继续发送擦除或编程命令。

`flash` 使用 `MRSFunc_FlashOperationExB`，默认 flags `0x06`，即编程和校验。
`verify` 使用同一入口但只保留校验标志。需要完整擦除、编程和校验时使用
`--flags 0x46`，再加复位并运行则使用 `--flags 0x47`。

`MRSFunc_FlashOperationExB` 的 flags 定义如下：

```text
0x80 关闭 Link 电源输出
0x40 全擦
0x20 解除读保护
0x10 OptEnd
0x08 McuCompiler_Clear
0x04 编程
0x02 校验
0x01 复位并运行
```
