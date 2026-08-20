# WCH-Link 复位与全擦路径

## 资料范围

本文依据以下资料整理，不把 USB 子命令的未公开编号解释为官方定义：

- `docs/references/official/WCH-Link使用说明.pdf`，版本号 V2.8
- `docs/references/code/wlink/src/commands/mod.rs` 的 `Reset` 枚举
- `docs/references/code/ch32-tapioca-probe/docs/wch-link-usb-protocol.md`
- 本项目对 `libmcuupdate.dylib` 的动态调用和命令回显记录

## 手册中明确的两类动作

手册把下面两类动作分开描述。

### 下载后的复位并运行

MounRiver Studio 下载配置（4.1）和 WCH-LinkUtility 下载配置（5.1）都将
`Reset and run` 列为 `Erase/Program/Verify` 之后的独立选项。它表示下载流程完成后让目标
从复位向量运行，不等同于全片擦除，也没有在手册中声明必须连接 NRST 或由 Link 控制目标供电。

### Code Flash 全擦的两种触发方式

MounRiver Studio 的 4.3.2 和 WCH-LinkUtility 的 5.2.4 明确写出，Code Flash 全擦可以通过：

| 触发方式 | 手册要求 | 对本项目的含义 |
| --- | --- | --- |
| 硬件复位引脚 | Link 与目标必须连接复位引脚 | 没有 NRST 连接时不能宣称支持该路径 |
| 重新上电 | Link 必须为芯片供电 | 可由目标 VBUS/电源开关实现，但需要验证目标电源确实被关闭并重新建立 |

同一段落还限制该功能适用于 WCH-LinkE、WCH-DAPLink 和 WCH-LinkW。5.2.5 将 Link 电源
3.3 V/5 V 开关单独列为“电源输出可控”，说明电源循环是独立于普通调试复位的硬件动作。

因此，手册能够确认的是“下载后运行”和“全擦触发方式”两套语义，不能从中推导出两条
USB `0x0b` 复位子命令的名称或实现细节。

## USB 命令与主机实现的对应

`wlink` 主机代码将 family `0x0b` 的复位子命令定义为：

| 子命令 | `wlink` 名称 | 可确认的主机注释 | 当前证据边界 |
| ---: | --- | --- | --- |
| `0x01` | `Soft` | `wlink_quitreset`，reset and run | 主机明确这样命名，但 WCH 手册没有公开该编号 |
| `0x02` | `Chip` | chip reset，注释说明 memory is not reset | 未找到官方手册对该编号的进一步定义 |
| `0x03` | `Normal` | 普通复位枚举值 | 具体是运行还是保持调试状态，应以主机调用路径和实测为准 |

Tapioca 的协议笔记只把 `81 0b 01 01` 记录为 reset dance 中出现的命令，并没有给出
`0x01/0x02/0x03` 的 WCH 官方名称。因而不能把 `Soft`、`Chip`、`Normal` 当作手册已证实
的硬件复位类型。

## MRS 动态库的两个观察值

对官方 `libmcuupdate.dylib` 的调用记录显示：

| API | 观察到的请求 | 结论 |
| --- | --- | --- |
| `McuCompiler_Reset()` | `81 0b 01 01`，期待 `82 0b 01 01` | 与 `wlink` 的 `Soft` 子命令一致 |
| `McuCompiler_ResetB()` | `81 0b 01 06`，期待 `82 0b 01 06` | MRS 私有或扩展路径，`wlink` 枚举没有对应项 |

`ResetB` 的 `0x06` 不能仅凭名称解释为硬件 NRST、芯片复位或重新上电。当前没有
WCH 公布的命令表、MRS 符号语义或完整官方抓包把它绑定到其中一种硬件动作。MRS 的
`FlashOperationExB` 标志 `0x01` 只表示“复位并运行”，也不能单独证明它调用的是
`Reset` 还是 `ResetB`。

## 当前固件实现边界

当前协议层的实现应按下面的证据等级理解：

- `0x01`：走 RVSWD Debug Module 的软复位并发送 `resumereq`，对应“复位后运行”语义
- `0x06`：保留 MRS 观察到的扩展复位并运行路径，不能标记为硬件 NRST
- `0x03`：走 RVSWD 复位并保持调试状态的路径，属于当前实现约定，不是手册文字定义
- `0x02`：没有完成足够的官方行为核对，不应声称已实现芯片复位
- 硬件 NRST 全擦：当前硬件未连接目标 NRST，不能宣称支持
- 重新上电全擦：只有在目标由本项目电源开关供电且完成断电、放电、重新上电验证后才能宣称支持

## 后续验证

若要确定两套复位命令的实际差异，应在官方 LinkE 上分别执行 MRS 的普通复位、ResetB、
“Reset and run”和 Code Flash 全擦，并记录 USB 请求、目标 NRST、电源输出和 RVSWD 波形。
在补齐 NRST 或确认电源循环时序前，不应根据 `0x06` 的名称扩展协议行为。

