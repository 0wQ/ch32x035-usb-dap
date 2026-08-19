# WCH-LinkE Option Bytes 分派

## 分析对象

```text
文件    ../../../../.tmp/WCH-LinkUtility/Firmware_Link/FIRMWARE_CH32V305.bin
大小    109544 bytes
SHA-256 9c1cd70565ee339409f8bf1f9e466ed0495e0e903fab4fd1044ac55ceb5d6695
```

该文件是以 CH32V305 为主控的 Link 固件，不是只面向 CH32V305 目标的烧录算法。文件开头可以直接反汇编为 RV32 指令，没有压缩、加密或容器封装。固件没有符号表，以下函数名称均按常量、调用关系和 WCH-Link 命令语义标注。

## 直接证据

| 固件偏移 | 证据 | 含义 |
| --- | --- | --- |
| `0xBA32` | 构造 `0x40022000`、`0x45670123`、`0xCDEF89AB` | 目标 Flash 控制器访问和解锁 |
| `0xC520` | 访问 `0x1FFFF800`，解锁 `OBKEYR`，修改 `CTLR`，逐半字处理 Option Bytes | 一组 Option Bytes 配置算法 |
| `0xCB78` | 与 `0xC520` 对称，首个 Option Bytes 半字使用 `0x00FF` | 同组架构的读保护启用算法 |
| `0xD5E2` | 擦除 Option Bytes，重新写入 KEYR、MODEKEYR、OBKEYR，置 `CTLR.bit4` 后向首个半字写入 `0x5AA5` | 另一组架构的基础读保护关闭算法 |
| `0xD84C` | 构造首个 Option Bytes 半字为 `0x00FF`，再调用共用写入过程 | 同组架构的读保护启用算法 |
| `0x11476`、`0x11600` | 读取 `0x4002201C` 和 `0x40022020` | 查询 `OBR` 和 `WPR` 保护状态 |

解锁密钥由 `lui` 和 `addi` 指令构造，因此不会以完整的四字节小端常量出现在固件数据区。仅搜索原始字节会漏掉这些位置。

## Family 分派

配置命令处理代码位于 `0x11424` 附近。关闭保护分支从 `0x114CE` 开始，开启保护分支从 `0x11562` 开始。两条路径都读取已连接目标的 WCH-Link family，并使用相同的分组条件：

```text
(family & 0x1e) == 0x0c
    或
(family & 0x1f) == 0x0e
```

结合 `wlink` 和 Minichlink 的 family 定义，该条件覆盖：

| Family | 目标系列 |
| ---: | --- |
| `0x0C` | CH643 |
| `0x0D` | CH32X03x |
| `0x0E` | CH32L103 |

该组关闭保护时调用 `0xD5E2`，开启保护时调用 `0xD84C`。其他 CH32 family 分别调用 `0xC520` 和 `0xCB78`。基础四字节命令和携带 USER、WRP 的扩展命令还会向底层函数传入不同模式值。

因此，官方 Link 固件的结构是：

```text
目标 ChipID
    -> WCH-Link family
    -> Option Bytes 架构分组
    -> 组内共用保护算法
```

它不是为每个具体料号复制完整算法，也不是对所有 CH32 使用一条无差别流程。

## 对本项目的约束

- RVSWD、DMI、内存访问和主机下发 Flash loader 保持通用
- ChipID 只负责选择 WCH-Link family 和 Flash profile
- X035 与 L103 可以归入同一 Option Bytes profile
- V303、V305、V307 可以归入 V30X profile
- 查询保护状态可以共用 `OBR`、`WPR` 寄存器定义，但必须先确认 profile 支持
- 修改保护状态属于破坏性操作，未知 profile 必须拒绝
- 扩展配置帧会同时修改 USER 和 WRP，未完整实现前不能按基础保护命令处理

## 本地实现

`src/wchlink/rvswd_gpio.c` 使用目标 profile 统一维护 WCH-Link family、整片擦除解锁方式、Option Bytes 写入方式和基地址：

| 目标 | WCH-Link family | 整片擦除解锁 | Option Bytes 写入 |
| --- | ---: | --- | --- |
| CH32X035 | `0x0D` | 主存储区、快速编程 | 开启保护使用 fast-buffer，解除保护使用单个半字 |
| CH32L103 | `0x0E` | 主存储区、用户字、快速编程 | fast-buffer，完整重写 16 字节 |
| CH32V303/V305/V307 | `0x06` | 主存储区、快速编程 | `OPTPG`，逐半字完整重写 16 字节 |

X035 开启保护与 L103 的 fast-buffer 路径遵循 LinkE 中的顺序：选项字擦除使用
`OPTER` 和 `STRT`，完成后重新解锁主存储区和快速模式，清除 `OPTWRE`，再以
`FTPG`、`BUFRST`、`BUFLOAD` 装载完整镜像并启动编程。`OPTWRE` 只用于选项字
擦除，不会保留到快速缓冲写入阶段。

X035 和 L103 的基础解除保护按 `KEYR -> MODEKEYR -> OBKEYR` 重解锁，置 `CTLR.bit4` 后以半字
写入首个 RDP Option Byte `0x5AA5`。这与 LinkE 的 `0xD5E2` 分支一致，不读取也不重写
其余 Option Bytes。

RVSWD 的单次 32 位和 16 位目标写入会使用完整的 Program Buffer：`PROGBUF0` 放置
`sw` 或 `sh`，`PROGBUF1` 显式写入 `ebreak`。如果只更新 `PROGBUF0`，前一次内存读取
留下的 `PROGBUF1` 会继续执行，使随后的 Flash 寄存器写入触发 abstract command 异常。

V30X 半字流程与 `openwch/ch32v307` 官方 SDK 的 `FLASH_ReadOutProtection()` 一致：先用 `OPTER`、`STRT` 擦除，再置 `OPTPG`，按 `0x1FFFF800 + 2 * index` 写入 8 个半字。RVSWD 侧由 Program Buffer 执行 `sh`，不使用 32 位 `sw` 代替半字访问。
LinkE 的 `0xC520` 在每次调用 `0xB702` 写入半字后按 family 决定是否调用
`0xAA6A` 等待 Abstract Command。V30X family `0x06` 不调用 `0xAA6A`，而是直接轮询
`STATR.BUSY`；本地 V30X 路径同样将长等待放在 Flash 状态轮询中，不扩展普通
Abstract Command 超时。

`0xC520` 在 Option Bytes 擦除前通过 `0xC00C` 和 `0xBA2C` 解锁主存储区、快速模式
和 Option Bytes，擦除后再次调用 `0xBA2C` 并重新解锁 Option Bytes。V30X 本地路径
遵循相同顺序，不能在一次解锁后连续完成擦除和编程。

保护查询、整片擦除和保护修改在未知 profile 上均直接拒绝。WCH-Link 协议层只读取 profile 输出的 family，不再维护第二份 ChipID 分派。

读保护开启后，目标可能拒绝 ChipID 地址读取。主机在 `connect` 前下发的 SetSpeed family 只在两项条件同时成立时作为受限连接提示：实际 ChipID 未读出，且 `OBR.RDPRT` 已确认开启。未保护目标仍使用真实 ChipID 选择 profile 并由上位机检查 family。

当前代码已通过 release 构建和静态差异检查。保护修改会改变 Option Bytes，解除读保护还会自动擦除主 Flash，因此写入后必须复位或重新上电回读。X035、L103 和 V30X 均已完成实板验证。

## 硬件验证

2026-08-19，使用真实 WCH-LinkE 将本项目固件写入 CH32X035F8U6，再通过该模拟
WCH-LinkE 操作 CH32L103K8U6。以下命令均成功：

```text
wlink --device 1 --chip CH32L103 status
wlink --device 1 --chip CH32L103 protect
wlink --device 1 --chip CH32L103 status
wlink --device 1 --chip CH32L103 unprotect
wlink --device 1 --chip CH32L103 status
```

保护后 `status` 仅能识别为 `CH32L103`，报告 `Flash protected: true`；解除后恢复真实
ChipID `0x10320710`，并报告 `Flash protected: false`。这验证了 L103 的基础保护查询、
保护和解除保护闭环。

2026-08-20，通过模拟 WCH-LinkE 操作 CH32X035C8T6，使用 `set-power` 控制目标真实
断电和重新上电。以下闭环已通过：

```text
tools/wlink_ours.sh --chip CH32X035 protect
tools/wlink_ours.sh set-power restart5v
tools/wlink_ours.sh --chip CH32X035 status
tools/wlink_ours.sh --chip CH32X035 unprotect
tools/wlink_ours.sh set-power restart5v
tools/wlink_ours.sh --chip CH32X035 status
```

保护后断电回读为 `Flash protected: true`，解除后再次断电回读恢复 ChipID
`0x03510611`，并报告 `Flash protected: false`。

2026-08-20，通过刷入本项目固件的 CH32X035C8T6 操作 CH32V307VCT6。探针使用
`PA2=SWCLK`、`PA3=SWDIO`，SWDIO 外接 `4.7 kOhm` 上拉。以下闭环已通过：

```text
tools/wlink_ours.sh --chip CH32V30X status
tools/wlink_ours.sh --chip CH32V30X protect
tools/wlink_ours.sh --chip CH32V30X status
tools/wlink_ours.sh --chip CH32V30X unprotect
tools/wlink_ours.sh --chip CH32V30X status
tools/wlink_ours.sh --chip CH32V30X dump 0x08000000 294912
```

初始状态识别 ChipID `0x30700528`，报告 `Flash protected: false`。保护后新会话只返回
family `CH32V30X`，报告 `Flash protected: true`。解除保护后新会话恢复 ChipID，并报告
`Flash protected: false`。

解除保护后的完整 `288 KiB` 主 Flash 共包含 `73728` 个 32 位字，唯一值均为
`0xe339e339`，转储 SHA-256 为
`f5c994f5ed0e387b9d5ef2c3c275533db41cddef08a4f5541e0868e3f83345c9`。
`wlink` 上游将 `0xe339e339` 标注为 WCH Flash 擦除后的初始值，因此整片擦除已完成实测。

SWDIO 上拉电阻虚接时，DMI 基础寄存器仍可响应，但 Program Buffer 内存读取失败，
连接最终返回 `0x13`。恢复外部 `4.7 kOhm` 上拉后，同一固件立即完成全部闭环；该次
失败属于测试接线问题，不作为固件缺陷处理。

最初的 X035 解除保护在半字写入后返回 `0x72`，细分后定位为 `0xBA`：写入
`0x5AA5` 的 Abstract Command 已下发，但目标自动擦除主 Flash 时持续返回 DMI BUSY，
原等待函数在单轮 DMI 重试耗尽后提前失败。官方 LinkE 的 `0xD5E2` 路径调用
`0xB702` 下发半字写命令，随后由 `0xAA6A` 等待 Abstract Command，再轮询
`STATR.BUSY`。本地实现据此允许等待函数跨多轮可重试的 DMI BUSY，普通内存操作保持
10 ms 超时，RDP 解除使用 6 s Flash 超时。

Option Bytes 擦除在最终 `0x5AA5` 写入失败时也可能使目标在下次上电后显示未保护，
因此不能只根据命令返回或同一会话状态判断结果，必须重新上电回读。

## 证据边界

固件无符号，当前分析足以证明 family 分派和两组保护算法的存在，但不能单凭函数偏移确定每一个临时状态和错误码的源码名称。实现细节还需要与目标参考手册、WCH SDK 和硬件结果交叉验证。
