# RVSWD / WCH-Link 参考资料

本目录收录 CH32X035 模拟 WCH-LinkE、通过 RVSWD 调试和烧录多个 WCH RISC-V 目标族时需要的协议、规范、抓包和上游实现。它不参与固件构建，目的是让这些资料不再依赖工作区外的 `.docs/`、`.tmp/` 目录。

当前实现以 CH32V307 的双线 RVSWD 和 WCH-Link USB direct-DMI 路径作为已验证基线，其他目标族按独立的 ChipID、family、Flash profile 和 loader 逐步适配。这里的单一目标证据不能直接外推到其他芯片，也不等同于完整复刻 WCH-LinkE 固件。

## 事实依据和验收优先级

涉及 MRS 烧录、WCH-Link 命令和目标族兼容性时，按以下顺序采信和验收：

1. `libmcuupdate.dylib`，MRS 内部烧录核心，负责擦除、编程、校验和复位入口
2. `wch-openocd`，MRS 内置调试和 GDB 工具
3. `FIRMWARE_CH32V305.bin`，官方 LinkE 固件的静态事实依据
4. `wlink`，第三方 Rust 工具，只作辅助交叉验证
5. `minichlink`，低优先级参考实现

`libmcuupdate.dylib`、`wch-openocd` 和 `FIRMWARE_CH32V305.bin` 的证据优先级相同，高于 `wlink` 和 `minichlink`

## 使用顺序

1. 先读目标芯片对应的 QingKe、调试模块和 Flash 资料，确认该目标族的寄存器与访问边界
2. 以 `official/riscv-debug-0.13.2.pdf` 和 `official/riscv-debug-1.0.pdf` 作为通用 Debug Module 规范依据，再标注芯片手册中的差异
3. 用 `code/ch32-tapioca-probe/docs/wch-rvswd-protocol.md`、`rvswd_frame.hpp` 和测试夹具核对 RVSWD 帧格式
4. 用 `code/ch32-tapioca-probe/docs/wch-link-usb-protocol.md`、`code/wlink/` 核对 WCH-Link USB 请求、回复和 host 侧重试行为
5. 分析新抓取的波形时使用 `code/sigrok-rvswd/pd.py`，并与对应目标族的真实 LinkE 基线对比；当前仓库内的 CSV 是 CH32V307 基线

## 导航

| 路径 | 内容 | 用途 |
| --- | --- | --- |
| [`official/`](official/) | QingKe V4 与 RISC-V Debug 规范 | V307 Debug Module、抽象命令和版本差异 |
| [`notes/ch32v307-rvswd-mvp.md`](notes/ch32v307-rvswd-mvp.md) | 当前实现的事实、边界和排查顺序 | 避免混用 JTAG DTM、RVSWD short frame 与 WCH 私有扩展 |
| [`notes/wch-link-reset-modes.md`](notes/wch-link-reset-modes.md) | WCH-Link 下载复位、全擦触发方式和 MRS 复位命令证据边界 | 区分复位后运行、硬件复位脚全擦和重新上电全擦 |
| [`notes/wch-openocd-flash-loader-transfer.md`](notes/wch-openocd-flash-loader-transfer.md) | WCH OpenOCD 的 loader 选择和下发路径 | 确认目标专用 loader 由上位机持有并通过数据端点传输 |
| [`notes/wch-linke-ch5xx-firmware-evidence.md`](notes/wch-linke-ch5xx-firmware-evidence.md) | LinkE 固件中的 CH5xx ChipID、loader 和全擦证据 | CH58x/CH59x RAM 布局、loader mode、命令口与长度边界 |
| [`notes/wch-linke-option-byte-dispatch.md`](notes/wch-linke-option-byte-dispatch.md) | 原厂 Link 固件中的保护命令和 family 分派 | 约束 Flash profile 分组和 Option Bytes 实现 |
| [`code/ch32-tapioca-probe/`](code/ch32-tapioca-probe/) | 52-bit codec、捕获夹具和 USB 协议文档 | 帧格式和 WCH-Link direct-DMI 协议的主要交叉验证 |
| [`code/sigrok-rvswd/`](code/sigrok-rvswd/) | Sigrok RVSWD 协议解码器 | 解码 52-bit short frame 与 84-bit long frame |
| [`code/wlink/`](code/wlink/) | `wlink` 主机的命令和 DMI 路径 | 确认当前测试工具实际发送的 USB 请求和重试行为 |
| [`captures/`](captures/) | 正常 WCH-LinkE 到 CH32V307 的基线 CSV | 需要验证时序或字段时的原始证据 |

## 已归档上游快照

| 本地内容 | 上游来源 | 固定版本 | 许可证 | 归档理由 |
| --- | --- | --- | --- | --- |
| `official/qingke-v4-processor-manual-v1.5.pdf` | WCH QingKeV4 Processor Manual V1.5 | 本地取得于 2026-08-19 | 文档内版权声明 | CH32V307 的 QingKe V4A Debug Module 直接依据，明确遵循 Debug 0.13.2 |
| `official/riscv-debug-0.13.2.pdf` | RISC-V External Debug Support 0.13.2 | 2019-03-23 发布版 | 文档内版权声明 | V307 的直接规范依据 |
| `official/riscv-debug-1.0.pdf` | RISC-V Debug Specification 1.0 | 2025-02-21 Ratified | 文档内版权声明 | 用于查阅 0.13 到 1.0 的澄清和差异，不作为 V307 行为依据 |
| `code/ch32-tapioca-probe/` | [pierrejay/ch32-tapioca-probe](https://github.com/pierrejay/ch32-tapioca-probe) | `260f55fee9334ad5813c26274b96e5ce0ee42cb9` | MIT，见目录内 `LICENSE` | 捕获验证过的 52-bit short-frame codec、夹具和 LinkE USB 协议说明 |
| `code/sigrok-rvswd/` | [perigoso/sigrok-rvswd](https://github.com/perigoso/sigrok-rvswd) | `5d2e1d5ba1e10e70fdef293bdcf3b7d6c976f8af` | BSD-3-Clause，见目录内 `LICENSE` | 当前最直接的 RVSWD 波形字段解码参考 |
| `code/wlink/` | [ch32-rs/wlink](https://github.com/ch32-rs/wlink) | `249f2c100005827dce8c7d82ff46917e52cddad9`，v0.1.2 | MIT 或 Apache-2.0，见目录内许可证 | 本项目实际用来验收的主机工具，保留其 USB 命令与 DMI 访问实现 |
| `captures/wch-linke-v2.19-ch32v307-status-20260818.csv` | 本项目实测 | 2026-08-18 | 项目数据 | 真正 WCH-LinkE 到 CH32V307 的状态查询波形基线 |

归档的第三方文件保持原始内容和原始路径层级，不作为本项目源码，也不被 `xmake` 编译。需要更新时，先在上游核对差异、许可证和提交，再替换对应目录。

## 有价值但不复制的资料

以下资料对排查有帮助，但不符合当前最小依赖或许可证边界，因此仅保留外部来源：

- [perigoso/rins](https://github.com/perigoso/rins)，`f1cc403e7cb52d51cf84233d5f4057fcbccef542`，CC BY-SA 4.0。它记录了 RVSWD 的公共物理层和 84-bit long frame，但不描述 V307 当前使用的 52-bit short frame
- [perigoso/blackmagic](https://github.com/perigoso/blackmagic)，GPL-3.0。其 `rvswd_dtm.c` 可交叉检查 wakeup 和短帧状态映射，但不应直接拷贝进本项目
- [hathach/riscv-openocd-wch](https://github.com/hathach/riscv-openocd-wch)，GPL-2.0。用于研究真实 WCH-Link 主机驱动，不作为本项目的可复用源码
- `wch_swio_flasher` 和 `swindle`：前者主要是单线 SWIO，后者不提供比 Tapioca/Sigrok 更直接的 52-bit V307 依据

## 维护规则

- 实现 WCH-Link 兼容关键功能前，先从原厂 LinkE 固件确认命令分派、寄存器顺序、等待条件和错误路径
- 目标芯片行为优先采用 WCH QingKe V4 手册和 Debug 0.13.2
- 52-bit 线格式优先采用真实捕获、Sigrok 解码器和 Tapioca 测试夹具的交叉结果
- USB 兼容优先采用当前实际测试的 `wlink` 和 `minichlink -C linke` 行为
- 任何抓包结论必须注明目标、探针、命令和采样条件，未经验证的猜测不写入本目录
