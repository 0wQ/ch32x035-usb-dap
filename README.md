# CH32X035 DAPLink

## 引脚

| 引脚 | 功能 |
| --- | --- |
| PA2 | SWCLK |
| PA3 | SWDIO |
| PA7 | WS2816C，状态指示 |
| PA5 | 按键，按下后复位进入 USB ISP |
| PA0 | 目标设备 Type-C SBU mux 使能 |
| PA4 | 目标设备 Type-C SBU mux 选择 |
| PB12 | 目标设备 Type-C 电源开关，同时作为 CMSIS-DAP 的 nRESET |
| PB11 | 目标设备 Type-C DP 上拉控制 |


## 第三方依赖

依赖 CherryUSB、CherryRB 和 CherryDAP，以 submodule 形式提供，需要初始化：

```sh
git submodule update --init
```

## 构建

需要 WCH RISC-V GCC 工具链：设置 `WCH_TOOLCHAIN_ROOT` 环境变量，或安装 MounRiver
Studio 2 由 xmake 构建脚本自动查找默认路径。

```sh
# bash/zsh
export WCH_TOOLCHAIN_ROOT="/Applications/MounRiver Studio 2.app/Contents/Resources/app/resources/darwin/components/WCH/Toolchain/RISC-V Embedded GCC12"
xmake -r

# PowerShell
$env:WCH_TOOLCHAIN_ROOT="C:/MounRiver/MounRiver_Studio2/resources/app/resources/win32/components/WCH/Toolchain/RISC-V Embedded GCC12"
xmake -r
```

产物：`build/release/firmware.elf`、`firmware.bin` 和 `firmware.map`。

## 验证

固件烧录后，主机应能看到一个 CMSIS-DAP 调试器和一个 CDC 串口。可用：

```sh
probe-rs list
probe-rs info --chip <目标型号> --probe <CMSIS-DAP探针序列号>
```

当前 USB 描述符沿用 CherryDAP 示例的 VID/PID `0d28:0204`
