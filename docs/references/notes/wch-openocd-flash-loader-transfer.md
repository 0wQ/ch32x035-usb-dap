# WCH OpenOCD Flash loader 下发路径

## 结论

WCH OpenOCD 的可读实现直接表明，Flash loader 由上位机持有、按芯片类型选择，并通过 WCH-Link 数据端点下发到探针。CH32V30x 使用 `flash_op307`，整镜像写入不是仅靠一个 Flash 命令完成。

## 证据

- `../../.tmp/riscv-openocd-wch/src/jtag/drivers/wlinke.c:345` 定义 `flash_op307`。该数组共 512 字节，包含末尾的 `0xff` 填充
- `../../.tmp/riscv-openocd-wch/src/jtag/drivers/wlinke.c:733-748` 的 `wlink_ramcodewrite()` 将 loader 按 64 字节分包，通过 endpoint 2 写出
- `../../.tmp/riscv-openocd-wch/src/jtag/drivers/wlinke.c:1123-1206` 的 `wlink_ready_write()` 先发送 `81 02 01 05`，再根据 `riscvchip` 选择 loader。`riscvchip` 为 `5` 或 `6` 时下发 `flash_op307`，随后发送 `81 02 01 07`，部分模式改为 `81 02 01 0b`
- `../../.tmp/riscv-openocd-wch/src/jtag/drivers/wlinke.c:1794-1805` 的 `wlink_write()` 默认使用 4096 字节数据块，并在传输数据前调用 `wlink_ready_write()`。因此 loader 下发属于整镜像写入的准备阶段
- `../../.tmp/riscv-openocd-wch/src/jtag/drivers/wlinke.c:1272-1285` 的 `wlink_erase()` 只发送 `81 02 01 01` 并读取回复。擦除入口与 loader 下发路径分离

## 端点含义

`../../.tmp/riscv-openocd-wch/src/jtag/drivers/wlinke.c:74-97` 显示，写操作将参数中的 endpoint 原样传给 libusb，读操作则将 `1`、`2` 映射为 `0x81`、`0x82`。因此 `wlink_ramcodewrite()` 中的 endpoint `2` 是 `0x02` OUT。

## `openocd-hacks` 来源核验

`treideme/openocd-hacks` 的固定版本为 `569a2316fc5f7c22dc29ecf8c04fcaaa73bfe64a`。其历史保留了 `Import MounRiver WCH fork`、`Import MounRiver WCH fork 1.50`、`Import MounRiver WCH fork 1.60 release` 和 `Apply changes from WCH release 1.80` 四次导入记录，其中 1.60 提交明确说明源码由 MRS 邮件提供。1.80 导入提交 `fc0342607639bb120ddad507815c8d3b0123485a` 加入当前的 `wlinke.c`、`wchriscv.c`、SDI transport 和 WCH RISC-V target。

该版本再次直接给出完整路径：

- `../../.tmp/openocd-hacks/src/jtag/drivers/wlinke.c:347` 定义 512 字节的 `flash_op307`
- `../../.tmp/openocd-hacks/src/jtag/drivers/wlinke.c:735-750` 将 loader 按 64 字节写 endpoint 2
- `../../.tmp/openocd-hacks/src/jtag/drivers/wlinke.c:1125-1208` 先发送 `0x05`，对 `riscvchip` 为 `5` 或 `6` 的目标选择 `flash_op307`，随后发送 `0x07` 或 `0x0b`
- `../../.tmp/openocd-hacks/src/jtag/drivers/wlinke.c:1225-1240` 将编程数据按 64 字节写 endpoint 2，并从 endpoint 2 读取块完成状态
- `../../.tmp/openocd-hacks/src/jtag/drivers/wlinke.c:1274-1287` 的擦除路径独立发送 `81 02 01 01`
- `../../.tmp/openocd-hacks/src/jtag/drivers/wlinke.c:1796-1807` 的整镜像写入默认使用 4096 字节块，并先调用 `wlink_ready_write()`
- `../../.tmp/openocd-hacks/src/flash/nor/wchriscv.c:106-128` 和 `../../.tmp/openocd-hacks/src/flash/nor/wchriscv.c:132-175` 分别将 OpenOCD 的 erase、write 入口连接到 `wlink_erase()`、`wlink_write()`

`openocd-hacks` 与 `hathach/riscv-openocd-wch` 当前 `wlinke.c` 的差异只有 libusb handle 类型修正和两个无效指针自赋值的删除，Flash loader、命令序列和端点传输代码一致。从来源可追溯性看，`openocd-hacks` 比 hathach 的整仓快照更接近 MounRiver/WCH 发布源码；但提交说明仍由第三方维护者记录，不能等同于 WCH 官方仓库的源码签名。

## 适用边界

上述两份可读源码分别固定于 `treideme/openocd-hacks` 的 `569a2316fc5f7c22dc29ecf8c04fcaaa73bfe64a` 和 `hathach/riscv-openocd-wch` 的 `ccb04d768b3161d4801a27bc95cc773817b10dd4`。`openwch/openocd_wch` 的发行版没有公开对应源码，其 `openocd.exe` 可见 `wlink_ramcodewrite`、`wlink_ready_write`、`wlink_fastprogram` 和 `wlink_endprogram` 等同名符号，但仅凭符号不能证明发行二进制与可读源码的函数体完全一致。
