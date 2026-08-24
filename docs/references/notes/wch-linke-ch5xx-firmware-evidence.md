# LinkE 固件中的 CH5xx 分派与 loader 证据

## 分析对象

```text
文件：../../../../.tmp/WCH-LinkUtility/Firmware_Link/FIRMWARE_CH32V305.bin
大小：109544 bytes
SHA-256：9c1cd70565ee339409f8bf1f9e466ed0495e0e903fab4fd1044ac55ceb5d6695
```

该文件是 WCH-LinkE 主控固件，不是 CH32V305 目标的 Flash 算法。固件没有符号表，以下位置使用固件文件偏移标记，指令按 RV32IMC 反汇编。固件运行地址比文件偏移高 `0x2000`，跳转表中的绝对地址按这个差值换算

## 目标识别的直接证据

主机实现和 LinkE 的 CH5xx 分派还覆盖 CH58x。现有上位机源码将 CH581/CH582/CH583
映射为 WCH-Link family `0x07`，目标 ChipID 低字节分别使用 `0x81`、`0x82`、`0x83`；
CH591/CH592 使用 family `0x0b` 和 `0x91`、`0x92`。CH582/CH583 与 CH592 共用
`0x40001041` 的 8 位型号读取、`0x20004000/0x20005000/0x20007000` loader
布局和 CH5xx Flash 命令口，但 USB 回复必须回显各自 family

固件偏移 `0x108ca` 开始的目标识别路径包含：

```text
0x108ca  lui   s1, 0x40001
0x108d2  addi  a0, s1, 0x41
0x108d6  jal   ...
0x108dc  addi  a5, zero, 0x96
0x108e0  andi  a0, a0, 0xfe
0x10900  addi  a0, s1, 0x41
0x10908  jal   ...
0x1090c  andi  a0, a0, 0xf0
0x10910  addi  a5, zero, 0x90
```

因此 LinkE 固件确实读取 `0x40001041`，并把该地址返回值按 8 位 ChipID 形态比较。`0x90` 高半字节进入 CH5xx 专用识别路径；在满足附加条件后，`0x10976` 将 `0x0b` 写入目标状态结构：

```text
0x10976  c.li   a5, 0xb
0x1097a  sb     a5, 0x13(s0)
```

这直接验证了 CH58x/CH59x 使用 WCH-Link family `0x0b`。该路径没有把 `0x91` 和 `0x92` 分成两个 family；当前 CH591/CH592 共用 `0x0b` 是与固件行为一致的。仅凭该调用点不能证明底层 helper 的总线访问宽度，能确定的是返回值按低 8 位 ChipID 使用

## USB 命令分派的直接证据

在 `0x10d14` 附近，固件从 USB 缓冲区读取首字节和 family/command 字段：

```text
0x10d14  jal   ...       ; 取得 USB 请求缓冲区
0x10d18  lbu   s10, 0x7c8(s2)
0x10d28  bne   s10, 0x81, ...
0x10d30  lbu   a4, 1(s1) ; family
0x10d3a  sb    4, 0x16(s0)
0x10d40  beq   a4, 6, ...
0x10d56  beq   a4, 0xb, ...
```

这确认命令端点请求以 `0x81` 开头，第二字节参与 family 分派。family `0x02` 在文件偏移 `0x10fae` 读取第四字节，将 `command - 1` 作为索引访问运行地址 `0x1c760` 的跳转表。该表位于文件偏移 `0x1a760`，因此可以直接对应各 command 处理块：

| command | 文件偏移 | 已确认职责 |
| --- | ---: | --- |
| `0x02` | `0x11080` | 设置 loader mode `0x08`，进入 Flash 数据写入 |
| `0x04` | `0x110d0` | 设置 loader mode `0x18`，写入后增加校验 |
| `0x05` | `0x11112` | 准备接收 Flash loader |
| `0x06` | `0x11298` | 设置 Prepare 状态 |
| `0x07` | `0x112be` | 结束 loader 数据阶段并初始化 loader |
| `0x08` | `0x11308` | 结束编程状态 |
| `0x0c` | `0x11388` | 开始数据端点读取 |

`0x07` 处理块连续两次调用文件偏移 `0x10b8a` 的目标执行 helper。第一次参数为 `(1, 0, 0)`，第二次在 Prepare 状态下为 `(3, 0, 0)`，否则仍为 `(1, 0, 0)`。标准 CH59x 写入流程没有发送 `0x06`，因此两次均使用 mode `1`

## CH59x loader 的目标 RAM 布局

目标连接流程按 WCH-Link family 初始化 loader 布局。索引表位于运行地址 `0x1c7a8`，文件偏移 `0x1a7a8`；family `0x0b` 的表项指向运行地址 `0x13fd0`，对应文件偏移 `0x11fd0`：

```text
0x11fd0  lui   a5, 0x20004
0x11fd4  sw    a5, 0x54(s0)  ; loader 入口 0x20004000
0x11fd6  lui   a5, 0x20005
0x11fda  sw    a5, 0x58(s0)  ; 数据缓冲区 0x20005000
0x11fdc  lui   a5, 0x20007
0x11fe0  sw    a5, 0x5c(s0)  ; 栈顶 0x20007000
```

执行 helper 在文件偏移 `0x10b8a` 将三个调用参数写入目标 `x10`、`x11`、`x12`，从状态结构 `0x5c` 字段装载 `sp`，从 `0x54` 字段装载 `dpc`。因此这三个地址不是主机侧约定的推测，而是 LinkE 固件直接使用的 CH58x/CH59x 目标布局

Flash 数据处理在文件偏移 `0x137de` 调用同一 helper，参数依次是 loader mode、当前目标 Flash 地址和本次字节数。结果处理位于 `0x13810` 附近：返回 `0` 对应数据端点状态 `0x04`，返回 `8` 对应状态 `3`，返回 `16` 对应状态 `5`

## CH59x 全擦命令

family `0x02` command `0x01` 在文件偏移 `0x10fd0` 进入全擦处理，随后调用文件偏移 `0xe69c` 的目标族分派。family `0x0b` 进入 `0xe6de` 路径，并以起始地址 `0`、长度和结束地址 `0x78000` 调用文件偏移 `0x3204` 的分块擦除函数

CH58x/CH59x 分支使用两组 8 位目标寄存器：

```text
0x40001040  Flash key
0x40001044  Flash 工作模式
0x40001804  命令、地址和状态数据
0x40001806  命令口控制
```

文件偏移 `0x315c`、`0x3130` 和 `0x317a` 共同组成命令发送、24 位地址发送和完成轮询。文件偏移 `0xafc6` 使用文件偏移 `0x1a6a0` 的 Program Buffer，依次执行 `sb a0, 0(a3)`、`sb a1, 0(a3)` 和 `sb a2, 4(a3)`。调用点将 `a3` 设为 `0x40001040`，并将 `a0`、`a1`、`a2` 设为 `0x57`、`0xa8`、`0xe0`，因此该调用是解锁与工作模式配置，不是向 `0x40001804` 发送 `0xe0` 命令

CH592 识别路径在文件偏移 `0x10976` 将内部 family 状态写为普通 `0x0b`。`0x3204` 对这个状态执行以下顺序：

1. 向 `0x40001040` 连续写入 `0x57`、`0xa8`，再向 `0x40001044` 写入 `0xe0`
2. 从地址 `0` 开始发送 `0x20`，每次擦除 `0x1000` 字节
3. 每个块完成后最多轮询 102 次，每次以命令 `0x05` 读取两次状态，第二次状态 bit 0 清零表示完成

在进入分块擦除前，官方固件还会调用文件偏移 `0x31c6` 的 FlashOpen 路径，将
`0x40001806` 写为 `4`，发送 `0xff` 命令并等待命令口结束。模拟探针的 RAM stub
必须保留这一步，不能只复用解锁后的 `0x20` 扇区循环，否则连续 MRS 会话可能继承上次
命令口状态而出现间歇性擦除失败。stub 同时在整段擦除期间屏蔽目标全局中断，完成后恢复
进入执行器前保存的状态

该路径覆盖 `0x00000000..0x00078000`，共执行 120 个 4 KiB 块，没有访问 CH32 使用的 `0x400220xx` Flash 控制器。`0xd8` 的 64 KiB 路径属于 `0x3204` 中其他内部 family 状态，不能外推到 CH592

## loader 长度边界

固定的 1536 字节来自上位机 loader 资料，不是目前已定位到的 LinkE 固件固定常量：

| 来源 | CH58x/CH59x loader 内容 | 实际传输方式 |
| --- | ---: | --- |
| `minichlink/pgm-wch-linke.c` `bootloader_v3` | `.len = 1536` | 64 字节分包 |
| WCH OpenOCD `flash_op583` | 1344 字节 | 64 字节分包 |
| Rust `wlink` `flash_op::CH583` | 1326 字节 | 256 字节分包，末包填充 `0xff` |

三者都在 loader 后发送 `81 02 01 07`。LinkE 的 `0x05` 状态通过数据端点逐包写入状态结构 `0x54` 指向的 loader 区域，并由 `0x07` 切换状态和执行 loader；该路径没有使用 1536 字节结束条件。因此探针不能把 CH59x loader 结束条件硬编码为恰好 1536，应在安全上限内接收分包，以 `0x07` 作为执行边界。当前实现对 CH59x 使用 2048 字节本地安全上限，对 V30x 保留 512 字节严格长度路径

## 实板验证

2026-08-21，使用官方 LinkE 将本项目固件写入 CH32X035C8T6，构建产物为：

```text
build/release/firmware.bin
size: 24284 bytes
sha256: f064d29df60d7a786976b5740c14a63c585d871d54a2a0de8c091cc99e101b69
```

项目探针连接 CH592 后，以下只读命令通过：

```text
./tools/wchlink/wlink_ours.sh status
Attached chip: CH59X [CH592] (ChipID: 0x92000000)
authenticated: true
allhalted: true
cmderr: 0x0
progbufsize: 0x8

./tools/wchlink/wlink_ours.sh dump 0x00000000 256
Read memory from 0x00000000 to 0x00000100
```

这验证了新固件的 CH592 识别、连接、DMI 和通用内存读取没有回归

随后将探针固件重新烧入 CH32X035，并使用本地 PlatformIO `genericCH592F` 示例构建最小测试镜像。该镜像只初始化系统时钟并保留固定签名，不配置外设引脚：

```text
文件：../../../../.tmp/ch592-write-verify-fixture/.pio/build/genericCH592F/firmware.bin
大小：2136 bytes
SHA-256：25e32b2a4601e008eced2505488fb908b2fb40e484228da4ff3f2098c4740061
```

使用项目探针 Serial `035CDAB8706E` 完成以下实板步骤，每次操作前均重新执行 `wlink list`：

```text
./tools/wchlink/wlink_ours.sh erase
Erase done
```

擦除后直接回读会得到重复的 `a9 bd f9 f3`，这是 wlink 对 CH59x“刚进入调试模式、代码区尚未写入有效固件”的无效代码标记，不应按全 `0xff` 判断擦除结果。该结果同时确认擦除前的应用向量表已消失

```text
./tools/wchlink/wlink_ours.sh -vv flash -R .../firmware.bin
write data ep total 1326 bytes
recv data 41010104
Fastprogram done
recv 820201 00
Flash done
```

按镜像实际长度 `2136` 字节回读后，源文件与目标回读严格一致：

```text
cmp=match
25e32b2a4601e008eced2505488fb908b2fb40e484228da4ff3f2098c4740061  firmware.bin
25e32b2a4601e008eced2505488fb908b2fb40e484228da4ff3f2098c4740061  program-readback.bin
```

最后执行 `./tools/wchlink/wlink_ours.sh reset` 后再次 `status`，得到 `CH59X [CH592]`、`allhavereset=true`、`allhalted=true`、`authenticated=true`、`allresumeack=true`、`cmderr=0x0`。这完成了 CH592 的全擦、loader 接收与两次初始化、编程、校验、严格回读和复位连接闭环

随后使用 MounRiver OpenOCD 发行版和 RISC-V GDB 做连续调试验证。由于官方 LinkE 和项目探针同时连接，OpenOCD 通过 `wlink_set_index 1` 选择项目探针 Serial `035CDAB8706E`，配置和工具路径如下：

```text
OpenOCD：/Users/sora/wch/OpenOCD/OpenOCD/bin/openocd
配置：/Users/sora/wch/OpenOCD/OpenOCD/bin/wch-riscv.cfg
GDB：/Users/sora/wch/Toolchain/RISC-V Embedded GCC12/bin/riscv-wch-elf-gdb
```

OpenOCD 启动时完成目标检查并报告 `datacount=2`、`progbufsize=8`、`XLEN=32`、`misa=0x40901105`，随后在 TCP `3333` 端口监听 GDB。GDB 会话依次完成：

```text
target extended-remote localhost:3333
monitor halt
读取 pc/sp/a0：pc=0x83c，sp=0x200067f0
写入并读回 a0：0x2468ace0
写入并读回 SRAM 0x20007f00：0xcafebabe
continue
Ctrl-C 中断
stepi：pc 从 0x83c 前进到 0x83e
再次 continue、Ctrl-C 中断、disconnect
```

该会话覆盖 GDB 连接、halt、寄存器读写、目标 SRAM 读写、连续运行、中断、单步和断开；OpenOCD 日志中 GDB 连接均正常建立和释放，没有出现 `LIBUSB_ERROR_TIMEOUT`

## 证据边界

- LinkE 固件直接证据：`0x40001041`、`0x90` 高半字节分支、family `0x0b` 写入、USB 请求首字节 `0x81`、family `0x02` command 跳转表、CH59x loader RAM 布局、`0x07` 两次初始化、Flash loader mode/结果映射和 CH592 全擦寄存器序列
- 主机源码证据：CH59x loader 的三种长度及 64/256 字节分包方式
- 当前实现约束：2048 字节是探针防止 loader 覆盖数据区的本地上限，不是 LinkE 固件中的固定长度常量
- 尚未证实：`0x06` Prepare 在不同上位机流程中的完整目标寄存器语义

## CH582/CH583 适配边界

当前探针代码已按上述 ChipID 和 family 增加 CH582/CH583 profile：

- `0x82`、`0x83` 读取结果分别映射为 `0x82000000`、`0x83000000`
- USB 协议 family 使用 `0x07`
- loader RAM、CH5xx 8 位 Flash 命令口和 `0x78000` 擦除边界沿用 CH58x/CH59x 共用路径

2026-08-23 重新确认目标 CH582 已供电后，使用项目探针 Serial `035CDAB8706E`
完成了 CH58x RVSWD long frame 适配。官方 LinkE 物理抓包显示每个事务包含 84 个有效
字段时钟，随后由 STOP 序列产生一个额外时钟；host parity 位在本次 CH582 连接中固定为
0，target 末位按读写方向变化，不能直接套用 CH32V307 short frame 的状态和校验规则。

按该抓包修正后，项目探针对 CH582 完成以下闭环：

```text
./tools/wchlink/wlink_ours.sh --chip CH582 --speed low status
Attached chip: CH582 [CH582] (ChipID: 0x82000000)

./tools/wchlink/wlink_ours.sh --chip CH582 erase
Erase done

./tools/wchlink/wlink_ours.sh --chip CH582 flash -R blink_ch5xx.bin
Flash done

readback: cmp=match
sha256: 8f2399937656bb6f77d4429afeb631956842bafb19e4aab480285a4dd3775860

./tools/wchlink/wlink_ours.sh --chip CH582 reset
./tools/wchlink/wlink_ours.sh --chip CH582 status
Dmstatus.version=2, authenticated=true, cmderr=0
```

MRS `libmcuupdate.dylib` 使用 family `7`、Flash 地址 `0` 完成 `check`、独立
`clear_type=2` 擦除、`flags=0x0f` 编程/校验/复位和独立 verify，均返回 `0`。
`clear_type=0/1` 分别返回 `125/126`，这是裸 X035 开发板没有目标电源和 NRST 控制时
对应的硬件擦除边界，不代表 CH582 RVSWD 或 Code Flash 路径失败。MRS 默认地址
`0x08000000` 也不适用于 CH582，必须显式使用地址 `0`

使用 MounRiver OpenOCD 发行版对同一项目探针完成 CH582 examine，并通过 GDB 完成
halt、resume、再次 halt、PC/SP 读取和 SRAM 读取。CH583 仍只有代码 profile，未做独立
实板验收。CH58x 的调试引脚仍需连接为 PB15=SWCLK、PB14=SWDIO、共地并保证目标供电

## MRS `libmcuupdate.dylib` 边界验证

2026-08-21，官方 LinkE 和项目探针同时连接，设备序列号分别为
`0A018F068ED4` 和 `035CDAB8706E`。对两台设备分别执行
`mrs_wchlink_probe check --family 11 --debug-mode 1 --speed 3`，USB trace 完全一致：

```text
OUT 81 0d 01 01
IN  82 0d 04 03 03 12 00
OUT 81 0c 02 0b 03
IN  82 0c 01 01
OUT 81 0d 01 02
IN  82 0d 05 0b 92 00 00 00
OUT 81 11 01 0b
IN  81 11 01 02
set_chip_type(11)=5
```

真实 `libmcuupdate.dylib` 在 `McuCompiler_SetChipType` 的非跳过路径中为 `0x11`
查询申请 20 字节接收缓冲，收到不足 20 字节时返回错误码 `5`。官方 LinkE 连接
CH592 的直接 USB 实测返回完整 20 字节：

```text
82 0d 01 ff 92 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

其中 `0xff` 和 `0x92` 是 CH59x 目标专用字段，不能把其他芯片族的 4 字节拒绝帧用于
CH592

外部主机实现对 CH59X 的处理方式不同：Rust `wlink` 将 `CH592` 映射到 family
`0x0b`，并明确关闭 `GetChipInfo(0x11)` 查询；MounRiver OpenOCD 的当前发行版
包含 `CH59x` 目标分支，实测对项目探针完成 examine，并报告 `448 kbytes`、Flash
范围 `0x00000000..0x00070000`。此前模拟 Link 曾按第三方实现伪造 20 字节 CH5xx
信息，但该格式没有 LinkE 或 CH592 上的 dylib 实测依据，现已移除。当前 CH5xx
收到 `0x11` 查询时按官方 CH592 的 20 字节格式回复。真实 dylib 的
`MRSFunc_FlashOperationExB` 普通目标族分支会转入 `MRSFunc_FlashOperation`，再以
`skip=false` 调用 `McuCompiler_SetChipType`。低层 `McuCompiler_Download` 已独立验证
CH592 loader 可工作，模拟 Link 上的完整 MRS ExB 仍需在 CH592 回接后验收。CH592 的
擦除、编程和校验继续以 OpenOCD、Rust `wlink` 和 LinkE 固件证据为准

2026-08-22 对 `McuCompiler_GetDeviceMode` 做了独立验证。MRS 调用顺序必须是
`SetTargetChip(11,1)`、`OpenDevice`、`GetDeviceMode`；在该完整顺序下，官方 LinkE 和
模拟 Link 都收到并返回：

```text
OUT 81 0f 01 02
IN  82 0f 01 02
```

DLL 反汇编确认第四字节映射为内部模式值：`0` 返回 `2`，`1` 返回 `1`，`2` 返回 `3`。
因此 `0x02` 是 RISC-V 调试模式查询的事实响应，不能为了让界面显示而改成其他值。裸
USB 工具在未执行 MRS 初始化时可能读到端点中的旧帧，不能作为该命令的协议结论。

`81 0f 01 01` 是 `SetIAPMode`，官方 LinkE 收到后退出 WCH-Link 枚举进入升级态，MRS
不会读取该命令的响应。模拟探针保留该无响应语义，但将其作为显式维护触发，复位到
CH32X035 自身 USB ISP；后续镜像仍需使用 `wchisp`，不能把它解释为官方 LinkE IAP
数据协议。CDC-ACM 仅执行回环，不参与 ISP 触发，普通 MRS 版本查询 `81 0f 01 02`
仍返回 RISC-V 调试模式响应

同日将该固件烧回 X035 后，MRS DLL 的 CH592 验收结果如下：

```text
GetDeviceMode = 3
CompareVersion = 0
query_rprotect = 4
get_mem_type = 104
flash-gui flags=0x0f, address=0: result=0
verify: result=0
reset: result=0
```

首次编程后再次执行 `flash-gui` 曾返回 109。抓包显示第二次 `SetChipType` 的
`81 0d 01 ff` 没有收到响应，根因是本地实现曾在 CH5xx 编程完成后抑制下一次 STOP
回复。移除该一次性抑制、始终按 LinkE 格式返回 20 字节 CH5xx 目标信息后，先复位
目标，再连续三次 `flash-gui` 均返回 0；随后复位后独立 `verify` 也返回 0

2026-08-23 将包含 FlashOpen 和中断屏蔽的诊断版探针固件写入项目 X035，目标回读与
`build/release/firmware.bin` 的 `25960` 字节镜像严格一致，SHA-256 为
`1846c0b3754f493fc8ee5b67bd034bc60e3a7b8caf46047cb0ef106c91c08cba`。使用真实
`libmcuupdate.dylib` 对 CH592 连续执行 10 次独立全擦和 20 次完整
`flags=0x0f` 流程，全部返回 `0`。MRS HEX 镜像回读 `2820` 字节并通过 `cmp`。
随后使用 `/Users/sora/wch/OpenOCD` 和 RISC-V GDB 完成 examine、halt、寄存器和
SRAM 读写、continue、中断、stepi 与 disconnect，OpenOCD 日志未出现 USB timeout

随后切换为 CDC 纯回环、官方 SetIAPMode 维护入口的最终版，镜像大小为 `25780`
字节，SHA-256 为 `3d12d6044fd0cd2bc8db1877bd79867ed7069cff7c3b61b0f1eeb22608bb36ae`。
`wchlink_enter_iap.sh` 实测发送 `81 0f 01 01` 后在 10 秒窗口内完成 `wchisp`
擦除、编程和校验；向 CDC 写入 `WCHISP` 得到同样的六字节回显。最终版再连续执行
10 次 MRS `flags=0x0f` 全流程，全部返回 `0`
