# CH32V307 RVSWD MVP 事实与边界

## 目标和规范

- 当前目标为 CH32V307VCT6，核心为 QingKe V4A
- QingKe V4 手册第 7 章明确其 Debug Module 遵循 RISC-V External Debug Support 0.13.2
- 实测 `dmstatus.version = 2`，与 0.13 一致
- `riscv-debug-0.13.2.pdf` 是实现依据。`riscv-debug-1.0.pdf` 只用于比较版本差异和查阅后续澄清

因此，不能把 Debug 1.0 的 `dmstatus.version = 3`、新增寄存器或 JTAG DTM 的细节直接当作 V307 行为。

## 三层协议

| 层 | 当前实现 | 关键事实 |
| --- | --- | --- |
| WCH-Link USB | `src/wchlink/wchlink_usb.c`、`wchlink_protocol.c` | 命令端点 `0x01`、回复端点 `0x81`，DMI 命令为 `81 08 06 <reg> <data-be> <op>` |
| WCH RVSWD | `src/wchlink/rvswd_gpio.c` | PA2 是 SWCLK，PA3 是 SWDIO，空闲高，START/STOP 类似 I2C，但数据编码不是 ARM SWD |
| RISC-V Debug Module | `rvswd_gpio.c` 的 DMI、abstract command 和 Program Buffer 路径 | `dmcontrol`、`dmstatus`、`abstractcs`、`command`、`data0`、`progbuf` 遵循 0.13.2 |

USB DMI 成功状态为 `0`，而 RVSWD short-frame 线上成功状态为 `1`。这是不同层的编码，固件需要映射，不能直接比较。

## 52-bit short frame

CH32V307 当前采用 52-bit short frame。字段和端点所有权以 `code/ch32-tapioca-probe/src/wchlink/rvswd_frame.hpp`、对应夹具和 Sigrok 解码器为交叉依据：

| 位 | 字段 |
| --- | --- |
| 0..6 | 7-bit DMI address |
| 7 | 操作，0 为读，1 为写 |
| 8..13 | host parity、park 和 padding |
| 14..45 | 32-bit data |
| 46..47 | data parity 和 park |
| 48..49 | target status，1 成功，3 busy，2 失败 |
| 50..51 | target padding |

推荐的总线所有权是读 `14 host / 38 target`、写 `48 host / 4 target`，每帧一次 turnaround。当前 GPIO 实现存在额外方向切换，这是基于已有可连接、擦除和烧录结果保留的实验实现。没有新的正常/异常对照波形前，不应仅凭 long-frame 文档改动它。

## busy 与 abstract command

RVSWD 线上的 `status = 3` 表示目标暂不能接受该帧。它在语义上接近 RISC-V DMI busy，但它不是 JTAG `dtmcs.dmistat = 3` 的直接寄存器读数。

因此：

- 对 RVSWD short frame：保持有界重试、帧间隔和超时，不能发送 JTAG `dmireset` 帧
- 对 abstract command：写 `command` 后，在继续访问 `command`、`abstractcs`、`data` 或 `progbuf` 前必须确认 `abstractcs.busy = 0`
- `cmderr != 0` 时，新 command 会被忽略，应写 1 清除对应 `cmderr` 位后再继续

Debug 0.13.2 中的 `dmireset` 属于 JTAG DTM，不是 WCH RVSWD short-frame 协议的一部分。没有目标侧证据前，不实现假设的等价恢复帧。

## 波形基线

`captures/wch-linke-v2.19-ch32v307-status-20260818.csv` 是真实 WCH-LinkE 对 CH32V307 执行状态查询的导出。它用于：

- 验证空闲电平、START/STOP、时钟相位和读写字段
- 验证当前采集数据能被 Sigrok decoder 解析为合理帧
- 遇到 `status = 3`、连接失败或 turnaround 争议时，作为正常链路的对照

它不替代协议规范，也不证明未覆盖的烧录、复位或异常恢复流程。
