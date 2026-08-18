set_project("ch32x035-usb-dap")
set_version("0.1.0")
set_xmakever("2.9.8")
set_policy("build.release.strip", false)

add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode"})
includes("toolchains/wch-riscv/xmake.lua")

local arch_flags = {"-march=rv32imacxw", "-mabi=ilp32", "-msmall-data-limit=8", "-mno-save-restore"}

target("firmware")
    set_plat("cross")
    set_arch("riscv")
    set_kind("binary")
    set_targetdir("build/$(mode)")
    set_filename("firmware.elf")
    set_languages("gnu11")
    set_toolchains("wch-riscv")
    set_warnings("all")

    add_files("src/main.c", "src/bsp/bsp_delay.c", "src/usb/bsp_usb.c", "src/wchlink/*.c", "sdk/Startup/startup_ch32x035_highcode.S")
    add_files("sdk/Core/*.c", "sdk/Peripheral/src/*.c", "sdk/System/*.c")
    add_files("third_party/cherryusb/core/usbd_core.c", "third_party/cherryusb_port/usb_ch32x035_dc_usbfs.c")
    add_files("third_party/cherryrb/chry_ringbuffer.c")
    add_includedirs("src", "src/wchlink", "src/usb", "src/dap", "sdk/Core", "sdk/Peripheral/inc", "sdk/System")
    add_sysincludedirs("third_party/cherryusb/core", "third_party/cherryusb/common", "third_party/cherryusb/class/cdc", "third_party/cherryusb/class/msc", "third_party/cherryusb/class/hid", "third_party/cherryusb_port", "third_party/cherryrb", "third_party/cherrydap", "third_party/cherrydap/DAP/Include")

    add_cxflags(arch_flags, "-D__PACKED=__attribute__((packed))", "-fmessage-length=0", "-fsigned-char", "-ffunction-sections", "-fdata-sections", "-fno-common", "-Wno-comment", "-Wno-unused-parameter", "-Wno-missing-prototypes", {force = true})
    add_asflags(arch_flags, "-ffunction-sections", "-fdata-sections", {force = true})
    add_ldflags(arch_flags, "-ffunction-sections", "-fdata-sections", "--specs=nano.specs", "--specs=nosys.specs", "-nostartfiles", "-Wl,-Tsdk/Ld/Link_highcode_nv_256B.ld", "-Wl,--gc-sections", {force = true})

    if is_mode("debug") then
        set_symbols("debug")
        set_optimize("none")
        add_cxflags("-Og", {force = true})
    else
        set_symbols("hidden")
        set_optimize("smallest")
        add_cxflags("-flto", {force = true})
        add_ldflags("-flto", {force = true})
    end

    after_build(function (target)
        local toolchain = assert(target:toolchain("wch-riscv"))
        local objcopy = toolchain:get("gcc_prefix") .. "objcopy"
        os.execv(objcopy, {"-O", "binary", target:targetfile(), path.join(target:targetdir(), "firmware.bin")})
    end)
