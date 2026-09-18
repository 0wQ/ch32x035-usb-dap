-- 工程与全局策略
set_project("ch32x035-usb-dap")
set_version("0.1.0")
set_xmakever("2.9.8")
set_policy("build.release.strip", false)

-- 构建模式与工具链
add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = ".vscode"})
includes("toolchains/wch-riscv/xmake.lua")

local arch_flags = {"-march=rv32imacxw", "-mabi=ilp32", "-msmall-data-limit=8", "-mno-save-restore"}
local startup_file = "sdk/Startup/startup_ch32x035_highcode.S"
local linker_script = "sdk/Ld/Link_highcode_nv_256B.ld"

-- 构建后统计输出用的辅助函数
local function parse_berkeley_size(size_output)
    for line in size_output:gmatch("[^\r\n]+") do
        local text, data, bss = line:match("^%s*(%d+)%s+(%d+)%s+(%d+)%s+%d+%s+%x+%s+")
        if text and data and bss then
            return {
                text = tonumber(text),
                data = tonumber(data),
                bss = tonumber(bss)
            }
        end
    end
    raise("failed to parse berkeley size output")
end

local function parse_sysv_section_sizes(size_output)
    local sections = {}
    for line in size_output:gmatch("[^\r\n]+") do
        local name, size = line:match("^%s*(%S+)%s+(%d+)%s+0x%x+%s*$")
        if name and size then
            sections[name] = tonumber(size)
        end
    end
    return sections
end

local function sum_section_sizes(sections, names)
    local total = 0
    for _, name in ipairs(names) do
        local size = sections[name]
        if size == nil then
            raise("missing section in sysv size output: " .. name)
        end
        total = total + size
    end
    return total
end

local function format_size(bytes)
    if bytes >= 1024 then
        return string.format("%.2f KiB (%d bytes)", bytes / 1024.0, bytes)
    end
    return string.format("%d bytes", bytes)
end

local function print_memory_summary(size_info)
    print(string.format("Flash used: %s", format_size(size_info.text + size_info.data)))
    print(string.format("RAM   used: %s", format_size(size_info.ram)))
end

target("firmware")
    -- 目标属性
    set_plat("cross")
    set_arch("riscv")
    set_kind("binary")
    set_languages("gnu11")
    set_toolchains("wch-riscv")
    set_warnings("all")
    set_targetdir("build/$(mode)")
    set_filename("firmware.elf")

    -- 本工程
    add_files("src/**.c")
    add_includedirs("src", "src/dap")

    -- CH32X035 SDK
    add_files(startup_file,
              "sdk/Core/*.c",
              "sdk/Peripheral/src/*.c",
              "sdk/System/*.c")
    add_includedirs("sdk/Core", "sdk/Peripheral/inc", "sdk/System")

    -- CherryUSB, CH32X035 port
    add_files("third_party/cherryusb/core/usbd_core.c",
              "third_party/cherryusb/class/cdc/usbd_cdc_acm.c",
              "third_party/cherryusb_port/usb_ch32x035_dc_usbfs.c")
    add_sysincludedirs("third_party/cherryusb/core",
                       "third_party/cherryusb/common",
                       "third_party/cherryusb/class/cdc",
                       "third_party/cherryusb/class/msc",
                       "third_party/cherryusb/class/hid",
                       "third_party/cherryusb_port")

    -- CherryDAP, CherryRB
    add_files("third_party/cherrydap/DAP/Source/DAP.c",
              "third_party/cherrydap/DAP/Source/DAP_vendor.c",
              "third_party/cherryrb/chry_ringbuffer.c")
    add_sysincludedirs("third_party/cherrydap",
                       "third_party/cherrydap/DAP/Include",
                       "third_party/cherryrb")

    -- 编译、汇编与链接选项
    add_cxflags(table.join(arch_flags, {
                   "-D__PACKED=__attribute__((packed))",
                   "-fmessage-length=0",
                   "-fsigned-char",
                   "-ffunction-sections",
                   "-fdata-sections",
                   "-fno-common",
                   "-Wno-comment",
                   "-Wno-unused-parameter",
                   "-Wno-missing-prototypes"}), {force = true})
    add_asflags(table.join(arch_flags, {
                   "-ffunction-sections",
                   "-fdata-sections"}), {force = true})
    add_ldflags(table.join(arch_flags, {
                   "-ffunction-sections",
                   "-fdata-sections",
                   "--specs=nano.specs",
                   "--specs=nosys.specs",
                   "-nostartfiles",
                   "-Wl,-T" .. linker_script,
                   "-Wl,--gc-sections"}), {force = true})

    -- 按模式追加优化与符号设置
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

    -- 构建钩子，按触发顺序排列

    -- 打印当前模式、工具链与 SDK 目录
    before_build(function (target)
        local toolchain = assert(target:toolchain("wch-riscv"))
        local toolchain_root = toolchain:get("toolchain_root")
        cprint("${cyan}Using mode:${clear} %s", get_config("mode") or "debug")
        cprint("${cyan}Using toolchain:${clear} %s", toolchain_root)
        cprint("${cyan}Using SDK:${clear} sdk/")
    end)

    -- 指定 map 文件，与 elf 同目录
    before_link(function (target)
        local mapfile = path.join(target:targetdir(), target:basename() .. ".map")
        target:add("ldflags", "-Wl,-Map=" .. mapfile, {force = true})
    end)

    -- 生成 bin 并统计 Flash 与 RAM 占用
    after_build(function (target)
        local toolchain = assert(target:toolchain("wch-riscv"))
        local prefix = toolchain:get("gcc_prefix")
        local targetfile = target:targetfile()
        local bindir = target:targetdir()

        os.execv(prefix .. "objcopy", {"-O", "binary", targetfile, path.join(bindir, "firmware.bin")})

        local sections_output = os.iorunv(prefix .. "size", {"--format=sysv", targetfile})
        local size_info = parse_berkeley_size(os.iorunv(prefix .. "size", {"--format=berkeley", targetfile}))
        local sections = parse_sysv_section_sizes(sections_output)

        print(sections_output)
        -- .highcode 在 SRAM 中执行；.stack 是 NOBITS，不应依赖 Berkeley 分类推断 RAM 占用
        size_info.ram = sum_section_sizes(sections, {".highcode", ".data", ".bss", ".stack"})
        print_memory_summary(size_info)
    end)

    -- 清理生成的 bin 与 map
    after_clean(function (target)
        local bindir = target:targetdir()
        os.rm(path.join(bindir, "firmware.bin"))
        os.rm(path.join(bindir, target:basename() .. ".map"))
    end)
