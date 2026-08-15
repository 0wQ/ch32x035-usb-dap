# CH32X035 CherryDAP

基于 CH32X035 USBFS 和 CherryDAP 的 CMSIS-DAP 固件，支持 ARM SWD 和
USB CDC 串口。

## 当前连接

- PA5：SWCLK
- PA7：SWDIO
- PA2：目标 UART TX（USB CDC 发出的数据）
- PA3：目标 UART RX（目标返回到 USB CDC）
- PA4：目标 NRST

当前默认使用 GPIO bit-bang 完成 SWD。PA7 在目标回传时切为高阻，因此
无需额外并接 PA6。当前仅启用 ARM SWD，不启用 JTAG 和 SWO。PA4 支持
`nRESET`，可用于 CMSIS-DAP 的 `connect-under-reset` 和显式复位命令。

`src/dap/SW_DP_SPI.c` 保留 SPI1 辅助实验实现：SPI 输出 8 位 request 和
32 位写数据，GPIO 完成 turnaround、ACK、读数据和 parity。SPI1 根据主机
请求使用 187.5 kHz 至 24 MHz 的不超频分频档位；低于 187.5 kHz 时回退
到完整 GPIO bit-bang。该路径由文件顶部的 `SWD_SPI_ENABLE` 控制，默认关闭。

AT32F403A、24 MHz 请求、32 KiB RAM benchmark 的五轮实测结果：纯 GPIO
读取/写入为 119.6/118.6 kB/s，SPI 辅助为 96.7/107.3 kB/s。逐事务切换
GPIO/SPI 的开销超过了硬件移位的收益，所以当前不启用 SPI 辅助路径。

当前纯 GPIO fast path 将 `DAP_ProcessCommand` 和 SWD 传输代码放在 SRAM，
在每笔 SWD 事务入口预计算 PA7 的输入/输出配置，并将正常数据阶段按
4 位展开。AT32F403A、24 MHz 请求的三轮 benchmark 平均结果为：2 KiB
读取/写入 144.3/136.8 kB/s，32 KiB 读取/写入 131.6/127.8 kB/s。
24 MSa/s 逻辑分析仪实测连续 SWCLK 主峰为 3.00 MHz 和 3.43 MHz，连续
突发段加权平均约 3.17 MHz。
单独对 `SWD_TransferFast` 启用 `O3` 后，读取仅提升约 0.4%，写入下降
约 0.8% 至 1.4%，且 SRAM 代码增加 140 字节，因此保留全局 `-Os` 结果。
8 位展开比 4 位展开慢约 1%，因此保留 4 位展开。

## USB 接口

CherryDAP 原版注册机制提供一个复合 USB 设备：

- CMSIS-DAP：EP1 IN、EP2 OUT，64 字节 bulk 包
- USB CDC-ACM：EP3 notification IN、EP4 OUT、EP5 IN

CH32X035 的 USB 设备控制器（DCD）适配位于
`third_party/cherryusb_port/usb_ch32x035_dc_usbfs.c`，负责把 CherryUSB
的通用设备请求和端点操作映射到 X035 USBFS 外设。

CDC 的 line coding 会配置 USART2。CDC 收到的数据通过 PA2 发送，PA3
接收的数据由主循环转发回 CDC。该 UART 适配目前采用轮询接收和阻塞发送，
用于框架联调，不代表最终高速 UART 实现。

## 构建

```sh
xmake -r
```

产物：`build/release/firmware.elf` 和 `build/release/firmware.bin`。

## 验证

固件烧录后，主机应能看到一个 CMSIS-DAP 调试器和一个 CDC 串口。可用：

```sh
probe-rs list
probe-rs info --chip <ARM目标型号> --probe <CMSIS-DAP探针序列号>
```

当前 USB 描述符沿用 CherryDAP 示例的开发 VID/PID `0x0D28:0x0204`。
正式产品使用前应替换为已分配的 VID/PID。

## 第三方依赖

外层 submodule 为实际编译依赖：

- `third_party/cherryusb`
- `third_party/cherryrb`
- `third_party/cherrydap`

CherryDAP 内部声明的 `CherryUSB` 和 `CherryRB` 是其上游示例的嵌套
submodule，当前不初始化也不参与编译，不与外层依赖冲突。
