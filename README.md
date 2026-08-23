# CH32X035 WCH-LinkE 兼容探针

本工程使用 CH32X035 USBFS 实现 WCH-LinkE 风格的 RISC-V 调试探针，目标
是兼容 WCH-Link USB 命令、WCH OpenOCD、MounRiver Studio 和 `wlink`。
当前主线不是 CMSIS-DAP，也不是通用 ARM SWD 探针

## 当前状态

- USB 设备标识：VID `0x1a86`，PID `0x8010`
- WCH-Link 主命令端点：Bulk OUT `0x01`，Bulk IN `0x81`
- 目标数据端点：Bulk OUT `0x02`，Bulk IN `0x82`
- CDC-ACM 接口用于复合设备兼容和数据通路回环，ISP 维护使用官方 SetIAPMode 命令
- RVSWD 物理层当前由 CH32X035 `PA2/PA3` GPIO bit-bang 实现
- 已有一个目标族的识别、擦除、编程、校验和复位验证记录
- 其他 WCH RISC-V 目标族按独立 profile 和 loader 逐步适配

## RVSWD 连接

CH32X035 探针侧引脚固定为：

- `PA2`：RVSWD 时钟
- `PA3`：RVSWD 双向数据

目标侧调试引脚随芯片系列和封装变化，不能从探针侧 `PA2/PA3` 推断目标引脚。
必须按目标芯片资料确认时钟、双向数据、上拉、方向切换和共地连接。目标侧
SBU 如果仍连接到 UART 引脚，不能通过 USB 命令自动变成 RVSWD

## 构建

```sh
xmake -r
```

产物：

- `build/release/firmware.elf`
- `build/release/firmware.bin`

固件入口为 `src/main.c`，USB 处理入口为
`src/wchlink/wchlink_usb.c`，协议状态机位于
`src/wchlink/wchlink_protocol.c`，RVSWD GPIO 实现位于
`src/wchlink/rvswd_gpio.c`

固件维护时通过 `81 0f 01 01` 官方 SetIAPMode 命令进入 CH32X035 Boot 区，随后在约
10 秒 USB ISP 窗口内使用 `wchisp` 烧录自身固件，入口脚本会自动完成触发和重试。
入口脚本为 `tools/wchlink_enter_iap.sh`，CDC 端口仅执行回环

## 设备选择

同一台 macOS 主机连接多个 WCH-Link 设备时，设备索引可能因重新枚举而变化。
每次操作前先执行：

```sh
wlink list
```

然后使用明确的设备索引进行只读检查：

```sh
wlink --device 1 status
```

不要仅凭历史索引判断设备身份。当前项目探针的序列号通常以 `035` 开头，
官方 LinkE 和项目探针同时连接时必须再次确认列表

## MRS 工具探针

构建 MRS 动态库探针和 USB trace：

```sh
sh tools/build_mrs_wchlink_probe.sh
```

常用命令：

```sh
build/tools/mrs_wchlink_probe check --family N
build/tools/mrs_wchlink_probe erase --family N
build/tools/mrs_wchlink_probe flash --family N --file target.hex
build/tools/mrs_wchlink_probe verify --family N --file target.hex
```

`N` 必须与目标芯片对应的 WCH-Link family 一致，family、loader、Flash 地址
和数据包大小均属于目标族 profile，不能跨系列套用。macOS DriverKit 可能使
Homebrew libusb 无法读取设备序列号，此时优先使用 MRS/OpenOCD 的设备选择
机制，并断开无关的同 VID/PID 设备

## OpenOCD

MounRiver Studio 自带 OpenOCD 配置位于其安装目录下的
`OpenOCD/bin/wch-riscv.cfg`。选择项目探针时使用 WCH-Link 驱动提供的索引命令：

```sh
openocd \
  -f /path/to/wch-riscv.cfg \
  -c "wlink_set_index 1" \
  -c "init; shutdown"
```

目标检查成功后，再通过 GDB 端口验证 halt、resume、寄存器读写和内存读写。
出现 `LIBUSB_ERROR_TIMEOUT` 时，应记录超时前的最后一条主机命令和目标响应，
不能只依据 USB 枚举或固件编译成功判断调试兼容

## 验收边界

每个目标族都必须独立验证：

- 目标识别和 ChipID 读取
- 对应 loader 的长度、内容和主机实际传输分包
- 目标侧 RVSWD 波形和电气连接
- 全擦、编程、校验和复位运行
- GDB halt、resume、寄存器读写、内存读写和连续调试会话

一个目标族的验证结果不能直接外推到其他目标族

## 目录说明

- `src/wchlink/wchlink_usb.c`：USB 描述符、端点和请求生命周期
- `src/wchlink/wchlink_protocol.c`：WCH-Link 命令解析和响应
- `src/wchlink/rvswd_gpio.c`：52 位 RVSWD direct-DMI 和目标访问
- `tools/mrs_wchlink_probe.c`：MRS 动态库调用探针
- `tools/mrs_usb_trace.c`：libusb 调用跟踪层
- `tools/wlink_ours.sh`：本项目探针的常用 `wlink` 操作封装
