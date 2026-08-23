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

## 当前芯片适配进度

下表按目标芯片记录适配和验证边界。代码中存在对应 profile 不等于已经完成该料号的
实板验收；一个芯片的结果不能直接外推到同族的其他料号。探针主控 CH32X035 自身的
USB ISP 烧录不计入目标侧验收

| 目标芯片/族 | 代码适配 | 当前验证进度 | 证据与边界 |
| --- | --- | --- | --- |
| CH592 / CH59X | 已适配，ChipID `0x92`，family `0x0b` | 已完成实板验收 | MRS `libmcuupdate.dylib` 全擦、编程、校验、复位，镜像严格回读，OpenOCD examine 和 GDB 连续调试均通过 |
| CH591 / CH59X | 与 CH592 共用 profile | 未独立实板验收 | 代码覆盖 ChipID `0x91` 和 family `0x0b`，不能用 CH592 的实测结果代替 CH591 验收 |
| CH582 / CH583 | 已适配 CH58X profile，ChipID `0x82/0x83`，family `0x07` | 实板验收未完成 | CH582 实测连接返回 `81 55 01 12`，尚未取得目标 ChipID；接线和开发板状态需要先确认，CH581 未纳入当前 profile |
| CH32V307 / V30X | 已适配，和 V303/V305 共用 profile | 已完成基础实板验收 | CH32V307 已验证状态、保护/解除保护、Flash 回读和基础调试；V303/V305 未独立实测 |
| CH32L103 | 已适配，family `0x0e` | MRS 擦除、下载、校验已验证 | 保护/解除保护也有实板记录；完整复位和 GDB 边界尚未单独形成验收记录 |
| CH32X035（目标） | 已适配，family `0x0d` | 已完成基础实板验证 | 已验证保护、解除保护及目标供电断电重启回读；完整目标侧 MRS 编程、校验、复位和 GDB 验收尚未单独记录 |
| CH571 / CH57X | 当前未纳入 target profile | 不支持 | 现有 CH571 连接记录不构成协议或硬件验收依据 |

更详细的协议和实板记录见 [`docs/references/notes/`](docs/references/notes/)

## RVSWD 连接

CH32X035 探针侧与目标调试链路相关的引脚固定为：

| 引脚 | 当前用途 | 电平和控制策略 |
| --- | --- | --- |
| `PA1` | SWDIO 外部上拉控制 | 通过 `5.1 kOhm` 外部电阻连接 SWDIO，RVSWD 会话开始时输出高电平；会话结束时释放为浮空输入 |
| `PA2` | RVSWD 时钟 | GPIO 推挽输出 |
| `PA3` | RVSWD 双向数据 | 按传输阶段在输出和输入之间切换，空闲保持高电平 |

PA1 不是 RVSWD 数据线本身，而是通过 `5.1 kOhm` 外部电阻给 PA3/目标 SWDIO 提供上拉。目标侧的
SWCLK、SWDIO、供电和复位引脚仍需按具体目标芯片和开发板接线确认。

## 辅助控制引脚

| 引脚 | 外部网络或功能 | 当前固件行为 |
| --- | --- | --- |
| `PA0` | U52 `FSW7227` 的 `EN#`，UART/SBU 复用器使能 | 上电初始化为低电平，当前协议不动态切换 |
| `PA4` | U52 `FSW7227` 的 `SEL`，UART/SBU 方向选择 | 上电初始化为低电平，当前协议不动态切换 |
| `PA7` | WS2816C RGB 指示灯 `DIN` | 由 SPI1 MOSI 输出启动指示效果 |
| `PB11` | 经 `R9=1 kOhm` 连接目标 USB D+ 的上拉控制，用于目标进入 USB ISP | 由 WCH-Link 3.3V 电源命令 `0x09/0x0a` 控制；禁用时浮空输入，启用时输出高电平 |
| `PB12` | SY6280 `POWER_EN`，目标 5V VBUS 开关 | 由 WCH-Link 5V 电源命令 `0x0b/0x0c` 控制；低电平关闭，高电平开启，启动初始化后默认开启 |
| `PC15` | 目标 USB-C CC2 检测网络 | 属于板级硬件连接，当前 WCH-Link 主流程不读取 |
| `PA5` | 板级用户按键网络 | 当前 WCH-Link 主流程不读取 |

当前硬件没有接入独立的目标 NRST 线。固件的 `Reset and Run` 和复位保持路径使用
RVSWD Debug Module 的 `ndmreset`，不等同于拉低物理 NRST；WCH-Link `0x13`
`RESET_LOW` 当前仅返回协议应答，不驱动独立复位引脚。

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
