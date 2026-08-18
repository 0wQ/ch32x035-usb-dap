#include "rvswd_gpio.h"

#include "bsp/bsp_delay.h"
#include "wchlink_flash_loader.h"

#include <ch32x035.h>
#include <string.h>

#define RVSWD_CLOCK_PIN GPIO_Pin_2
#define RVSWD_DATA_PIN  GPIO_Pin_3
#define RVSWD_PINS      (RVSWD_CLOCK_PIN | RVSWD_DATA_PIN)

#define RVSWD_DMI_CONTROL 0x10u
#define RVSWD_DMI_CONFIG   0x7du
#define RVSWD_DMI_SHADOW   0x7eu

#define RVSWD_STATUS_OK   1u
#define RVSWD_STATUS_BUSY 3u

#define RVSWD_DMI_WRITE_RETRY_COUNT 16u
#define RVSWD_DMI_READ_RETRY_COUNT 64u
#define RVSWD_DMI_BUSY_DELAY_US 100u
#define RVSWD_DMI_ERROR_DELAY_US 50u
#define RVSWD_INTERFRAME_GUARD_US 8u
#define RVSWD_EXECUTE_TIMEOUT_MS 5000u

static uint32_t rvswd_flash_last_error;

static uint8_t rvswd_xor_bits(uint32_t value) {
    value ^= value >> 16u;
    value ^= value >> 8u;
    value ^= value >> 4u;
    value ^= value >> 2u;
    value ^= value >> 1u;
    return (uint8_t)(value & 1u);
}

static void rvswd_set_bit(uint8_t *buffer, uint8_t position, uint8_t value) {
    uint8_t mask = (uint8_t)(1u << (7u - (position & 7u)));

    if (value != 0u) {
        buffer[position >> 3u] |= mask;
    } else {
        buffer[position >> 3u] &= (uint8_t)~mask;
    }
}

static uint8_t rvswd_get_bit(const uint8_t *buffer, uint8_t position) {
    return (uint8_t)((buffer[position >> 3u] >> (7u - (position & 7u))) & 1u);
}

static void rvswd_config_data_output(void) {
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xfu << 12u)) | (0x01u << 12u);
}

static inline __attribute__((always_inline)) void rvswd_half_period(void) {
    __asm volatile(
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        );
}

static inline __attribute__((always_inline)) void rvswd_clock_low(void) {
    GPIOA->BSHR = RVSWD_CLOCK_PIN << 16u;
}

static inline __attribute__((always_inline)) void rvswd_clock_high(void) {
    GPIOA->BSHR = RVSWD_CLOCK_PIN;
}

static inline __attribute__((always_inline)) void rvswd_data_low(void) {
    GPIOA->BSHR = RVSWD_DATA_PIN << 16u;
}

static inline __attribute__((always_inline)) void rvswd_data_high(void) {
    GPIOA->BSHR = RVSWD_DATA_PIN;
}

static void rvswd_config_data_input(void) {
    // turnaround 期间只释放数据线，保持最后一个主机位的电平
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xfu << 12u)) | (0x04u << 12u);
}

static void rvswd_start(void) {
    rvswd_config_data_output();
    rvswd_clock_high();
    rvswd_data_high();
    rvswd_half_period();
    rvswd_data_low();
    rvswd_half_period();
}

static void rvswd_stop(void) {
    // 采样结束时 SWCLK 仍为高，先接管数据线，再结束当前位
    rvswd_config_data_output();
    rvswd_clock_low();
    rvswd_data_low();
    rvswd_half_period();
    rvswd_clock_high();
    rvswd_half_period();
    rvswd_data_high();
    rvswd_half_period();

}

static inline __attribute__((always_inline)) void rvswd_drive_bit(uint8_t value) {
    rvswd_clock_low();
    if (value != 0u) {
        rvswd_data_high();
    } else {
        rvswd_data_low();
    }
    rvswd_half_period();
    rvswd_clock_high();
    rvswd_half_period();
}

static inline __attribute__((always_inline)) uint8_t rvswd_sample_bit(void) {
    uint8_t value;

    rvswd_clock_low();
    rvswd_half_period();
    rvswd_clock_high();
    rvswd_half_period();
    // 目标数据在时钟高电平期间采样
    value = (GPIOA->INDR & RVSWD_DATA_PIN) != 0u ? 1u : 0u;
    return value;
}

static void rvswd_drive_range(const uint8_t *frame, uint8_t first_bit, uint8_t bit_count) {
    for (uint8_t bit = 0u; bit < bit_count; ++bit) {
        rvswd_drive_bit(rvswd_get_bit(frame, (uint8_t)(first_bit + bit)));
    }
}

static void rvswd_sample_range(uint8_t *frame, uint8_t first_bit, uint8_t bit_count) {
    for (uint8_t bit = 0u; bit < bit_count; ++bit) {
        rvswd_set_bit(frame, (uint8_t)(first_bit + bit), rvswd_sample_bit());
    }
}

static bool rvswd_transaction(const uint8_t *host, uint8_t *target, bool read) {
    __disable_irq();
    rvswd_start();

    // 地址、操作和校验之后，目标先返回三位握手，主机再发送两个控制位
    rvswd_drive_range(host, 0u, 9u);
    rvswd_config_data_input();
    rvswd_sample_range(target, 9u, 3u);
    rvswd_config_data_output();
    rvswd_drive_range(host, 12u, 2u);

    if (read) {
        // 读操作由目标返回数据、校验和三位状态，主机最后发送两个收尾位
        rvswd_config_data_input();
        rvswd_sample_range(target, 14u, 36u);
        rvswd_config_data_output();
        rvswd_drive_range(host, 50u, 2u);
    } else {
        // 写操作由主机发送数据和校验，目标随后返回三位状态
        rvswd_drive_range(host, 14u, 33u);
        rvswd_config_data_input();
        rvswd_sample_range(target, 47u, 3u);
        rvswd_config_data_output();
        rvswd_drive_range(host, 50u, 2u);
    }

    rvswd_stop();
    __enable_irq();

    // 目标在 STOP 后完成 DMI 状态更新，下一帧从空闲高电平开始
    bsp_delay_us(RVSWD_INTERFRAME_GUARD_US);

    return true;
}

static void rvswd_pack_common(uint8_t *frame, uint8_t address, uint8_t operation) {
    frame[0] = (uint8_t)((address & 0x7fu) << 1u | (operation & 1u));
    rvswd_set_bit(frame, 8u, (uint8_t)(rvswd_xor_bits(address & 0x7fu) ^ operation));
}

static void rvswd_pack_write(uint8_t *frame, uint8_t address, uint32_t value) {
    rvswd_pack_common(frame, address, 1u);
    for (uint8_t bit = 0u; bit < 32u; ++bit) {
        rvswd_set_bit(frame, (uint8_t)(14u + bit), (uint8_t)((value >> (31u - bit)) & 1u));
    }
    rvswd_set_bit(frame, 46u, rvswd_xor_bits(value));
    rvswd_set_bit(frame, 50u, 1u);
}

static void rvswd_pack_read(uint8_t *frame, uint8_t address) {
    rvswd_pack_common(frame, address, 0u);
    rvswd_set_bit(frame, 50u, 1u);
}

static uint8_t rvswd_unpack_handshake(const uint8_t *target) {
    return (uint8_t)(rvswd_get_bit(target, 48u) << 1u |
                     rvswd_get_bit(target, 49u));
}

static bool rvswd_status_is_ok(uint8_t status) {
    return status == RVSWD_STATUS_OK || status == 0u;
}

static uint32_t rvswd_unpack_data(const uint8_t *target) {
    uint32_t value = 0u;

    for (uint8_t bit = 0u; bit < 32u; ++bit) {
        value = (value << 1u) | rvswd_get_bit(target, (uint8_t)(14u + bit));
    }
    return value;
}

void rvswd_gpio_init(void) {
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA;
    GPIOA->BSHR = RVSWD_PINS;
    GPIOA->CFGLR = (GPIOA->CFGLR & ~((0xfu << 8u) | (0xfu << 12u))) |
                   (0x01u << 8u) | (0x04u << 12u);
}

void rvswd_gpio_disconnect(void) {
    GPIOA->BSHR = RVSWD_PINS;
    GPIOA->CFGLR = (GPIOA->CFGLR & ~((0xfu << 8u) | (0xfu << 12u))) |
                   (0x04u << 8u) | (0x04u << 12u);
}

bool rvswd_gpio_write_dmi(uint8_t address, uint32_t value) {
    uint8_t frame[7] = {0};
    uint8_t target[7] = {0};
    uint8_t status;

    rvswd_pack_write(frame, address, value);
    for (uint8_t retry = 0u; retry < RVSWD_DMI_WRITE_RETRY_COUNT; ++retry) {
        if (!rvswd_transaction(frame, target, false)) {
            return false;
        }

        status = rvswd_unpack_handshake(target);
        if (rvswd_status_is_ok(status)) {
            return true;
        }
        if (status == RVSWD_STATUS_BUSY) {
            bsp_delay_us(RVSWD_DMI_BUSY_DELAY_US);
        } else {
            // 非成功状态允许短暂重试，避免单次线噪声中断连续内存传输
            bsp_delay_us(RVSWD_DMI_ERROR_DELAY_US);
        }
    }

    return false;
}

bool rvswd_gpio_read_memory32(uint32_t address, uint32_t *value) {
    uint32_t data;

    if (value == NULL) {
        return false;
    }

    // V30x 使用 x8 执行 c.lw，避免连续内存访问时的寄存器别名问题
    if (!rvswd_gpio_write_dmi(0x20u, 0x90024000u) ||
        !rvswd_gpio_write_dmi(0x04u, address) ||
        !rvswd_gpio_write_dmi(0x17u, 0x00271008u) ||
        !rvswd_gpio_write_dmi(0x17u, 0x00221008u) ||
        !rvswd_gpio_read_dmi(0x04u, &data)) {
        return false;
    }

    *value = data;
    return true;
}

bool rvswd_gpio_write_memory32(uint32_t address, uint32_t value) {
    uint32_t abstractcs;

    // V30x 使用 x8 保存数据，x9 保存目标地址
    if (!rvswd_gpio_write_dmi(0x20u, 0x0084a023u) ||
        !rvswd_gpio_write_dmi(0x04u, address) ||
        !rvswd_gpio_write_dmi(0x17u, 0x00231009u) ||
        !rvswd_gpio_write_dmi(0x04u, value) ||
        !rvswd_gpio_write_dmi(0x17u, 0x00271008u) ||
        !rvswd_gpio_read_dmi(0x16u, &abstractcs)) {
        return false;
    }

    return ((abstractcs >> 8u) & 0x07u) == 0u;
}

static bool rvswd_gpio_write_memory_slow(uint32_t address, const uint8_t *data,
                                         uint32_t length) {
    uint32_t abstractcs;

    // 先配置一次程序缓冲区，后续每个字只更新地址和数据寄存器
    if (!rvswd_gpio_write_dmi(0x18u, 0u) ||
        !rvswd_gpio_write_dmi(0x16u, 0x00000700u) ||
        !rvswd_gpio_write_dmi(0x20u, 0x0084a023u) ||
        !rvswd_gpio_write_dmi(0x21u, 0x00100073u)) {
        return false;
    }

    for (uint32_t offset = 0u; offset < length; offset += 4u) {
        uint32_t value = ((uint32_t)data[offset + 0u]) |
                         ((uint32_t)data[offset + 1u] << 8u) |
                         ((uint32_t)data[offset + 2u] << 16u) |
                         ((uint32_t)data[offset + 3u] << 24u);
        if (!rvswd_gpio_write_dmi(0x16u, 0x00000700u) ||
            !rvswd_gpio_write_dmi(0x04u, address + offset) ||
            !rvswd_gpio_write_dmi(0x17u, 0x00231009u) ||
            !rvswd_gpio_write_dmi(0x04u, value) ||
            !rvswd_gpio_write_dmi(0x17u, 0x00271008u)) {
            return false;
        }
    }

    return rvswd_gpio_read_dmi(0x16u, &abstractcs) &&
           ((abstractcs >> 8u) & 0x07u) == 0u;
}

bool rvswd_gpio_write_memory(uint32_t address, const uint8_t *data, uint32_t length) {
    if (data == NULL || length == 0u || (address & 3u) != 0u || (length & 3u) != 0u) {
        return false;
    }
    return rvswd_gpio_write_memory_slow(address, data, length);
}

bool rvswd_gpio_write_register(uint16_t regno, uint32_t value) {
    uint32_t abstractcs;

    if (!rvswd_gpio_write_dmi(0x04u, value) ||
        !rvswd_gpio_write_dmi(0x16u, 0x00000700u) ||
        !rvswd_gpio_write_dmi(0x17u, 0x00230000u | (uint32_t)regno) ||
        !rvswd_gpio_read_dmi(0x16u, &abstractcs)) {
        return false;
    }
    return ((abstractcs >> 8u) & 0x07u) == 0u;
}

bool rvswd_gpio_read_register(uint16_t regno, uint32_t *value) {
    uint32_t abstractcs;

    if (value == NULL ||
        !rvswd_gpio_write_dmi(0x16u, 0x00000700u) ||
        !rvswd_gpio_write_dmi(0x17u, 0x00220000u | (uint32_t)regno) ||
        !rvswd_gpio_read_dmi(0x16u, &abstractcs) ||
        ((abstractcs >> 8u) & 0x07u) != 0u) {
        return false;
    }
    return rvswd_gpio_read_dmi(0x04u, value);
}

static bool rvswd_gpio_write_raw_gpr(uint8_t regno, uint32_t value) {
    uint32_t abstractcs;

    if (!rvswd_gpio_write_dmi(0x04u, value) ||
        !rvswd_gpio_write_dmi(0x16u, 0x00000700u) ||
        !rvswd_gpio_write_dmi(0x17u, 0x00231000u | (uint32_t)regno) ||
        !rvswd_gpio_read_dmi(0x16u, &abstractcs)) {
        return false;
    }
    return ((abstractcs >> 8u) & 0x07u) == 0u;
}

static bool rvswd_gpio_read_raw_gpr(uint8_t regno, uint32_t *value) {
    uint32_t abstractcs;

    if (value == NULL ||
        !rvswd_gpio_write_dmi(0x16u, 0x00000700u) ||
        !rvswd_gpio_write_dmi(0x17u, 0x00221000u | (uint32_t)regno) ||
        !rvswd_gpio_read_dmi(0x16u, &abstractcs) ||
        ((abstractcs >> 8u) & 0x07u) != 0u) {
        return false;
    }
    return rvswd_gpio_read_dmi(0x04u, value);
}

static bool rvswd_gpio_wait_halted(uint32_t timeout_ms) {
    for (uint32_t elapsed = 0u; elapsed < timeout_ms * 10u; ++elapsed) {
        uint32_t status;

        if (!rvswd_gpio_read_dmi(0x11u, &status)) {
            return false;
        }
        if ((status & (1u << 9u)) != 0u) {
            return true;
        }
        bsp_delay_us(100u);
    }
    return false;
}

bool rvswd_gpio_halt(void) {
    return rvswd_gpio_write_dmi(0x10u, 0x80000001u) &&
           rvswd_gpio_wait_halted(100u);
}

bool rvswd_gpio_execute(uint32_t entry, uint32_t mode, uint32_t address,
                        uint32_t length, uint32_t data_address, uint32_t *result) {
    if (!rvswd_gpio_write_raw_gpr(10u, mode)) {
        if (result != NULL) *result = 0xe001u;
        return false;
    }
    if (!rvswd_gpio_write_raw_gpr(11u, address)) {
        if (result != NULL) *result = 0xe002u;
        return false;
    }
    if (!rvswd_gpio_write_raw_gpr(12u, length)) {
        if (result != NULL) *result = 0xe003u;
        return false;
    }
    if (!rvswd_gpio_write_raw_gpr(13u, data_address)) {
        if (result != NULL) *result = 0xe004u;
        return false;
    }
    if (!rvswd_gpio_write_register(0x1002u, 0x20002000u) ||
        !rvswd_gpio_write_register(0x7b0u, 0x000090c3u) ||
        !rvswd_gpio_write_register(0x300u, 0u) ||
        !rvswd_gpio_write_register(0x7b1u, entry)) {
        if (result != NULL) *result = 0xe005u;
        return false;
    }
    if (!rvswd_gpio_write_dmi(0x10u, 0x80000001u) ||
        !rvswd_gpio_write_dmi(0x10u, 0x80000001u) ||
        !rvswd_gpio_write_dmi(0x10u, 0x00000001u) ||
        !rvswd_gpio_write_dmi(0x10u, 0x40000001u)) {
        if (result != NULL) *result = 0xe006u;
        return false;
    }
    bsp_delay_ms(mode == 1u ? 10u : 5000u);
    if (!rvswd_gpio_halt()) {
        if (result != NULL) *result = 0xe007u;
        return false;
    }
    if (result != NULL) {
        uint32_t value = 0u;

        if (!rvswd_gpio_read_raw_gpr(10u, &value)) {
            *result = 0xe008u;
            return false;
        }
        *result = value;
    }
    return true;
}

static bool rvswd_gpio_flash_wait(uint32_t mask) {
    for (uint16_t retry = 0u; retry < 5000u; ++retry) {
        uint32_t status;

        if (!rvswd_gpio_read_memory32(0x4002200cu, &status)) {
            return false;
        }
        if ((status & mask) == 0u) {
            return (status & (1u << 4u)) == 0u;
        }
        bsp_delay_us(100u);
    }
    return false;
}

static bool rvswd_gpio_flash_unlock(void) {
    return rvswd_gpio_write_memory32(0x40022004u, 0x45670123u) &&
           rvswd_gpio_write_memory32(0x40022004u, 0xcdef89abu) &&
           rvswd_gpio_write_memory32(0x40022024u, 0x45670123u) &&
           rvswd_gpio_write_memory32(0x40022024u, 0xcdef89abu);
}

bool rvswd_gpio_flash_erase_all(void) {
    uint8_t loader[448] = {0};
    uint32_t result = 0xffffffffu;

    rvswd_flash_last_error = 0u;
    memcpy(loader, wchlink_flash_loader, WCHLINK_FLASH_LOADER_SIZE);
    if (!rvswd_gpio_write_memory(0x20000000u, loader, sizeof(loader))) {
        rvswd_flash_last_error = 0xe101u;
        return false;
    }
    if (!rvswd_gpio_execute(0x20000000u, 0x03u, 0u, 0u, 0x20001000u, &result)) {
        rvswd_flash_last_error = result;
        return false;
    }
    if (result != 0u) {
        rvswd_flash_last_error = result;
        return false;
    }
    return true;
}

uint32_t rvswd_gpio_flash_last_error(void) {
    return rvswd_flash_last_error;
}

bool rvswd_gpio_flash_program_page(uint32_t address, const uint8_t *data, uint32_t length) {
    uint32_t control;

    if (data == NULL || length == 0u || length > 256u || (address & 0xffu) != 0u ||
        (length & 3u) != 0u || !rvswd_gpio_flash_unlock() ||
        !rvswd_gpio_read_memory32(0x40022010u, &control) ||
        !rvswd_gpio_write_memory32(0x40022010u, control | (1u << 16u))) {
        return false;
    }

    for (uint32_t offset = 0u; offset < length; offset += 4u) {
        uint32_t value = ((uint32_t)data[offset + 0u]) |
                         ((uint32_t)data[offset + 1u] << 8u) |
                         ((uint32_t)data[offset + 2u] << 16u) |
                         ((uint32_t)data[offset + 3u] << 24u);
        if (!rvswd_gpio_write_memory32(address + offset, value) ||
            !rvswd_gpio_flash_wait(1u << 1u)) {
            return false;
        }
    }

    if (!rvswd_gpio_write_memory32(0x40022010u, control | (1u << 16u) | (1u << 21u)) ||
        !rvswd_gpio_flash_wait(1u) ||
        !rvswd_gpio_write_memory32(0x40022010u, control & ~(1u << 16u))) {
        return false;
    }
    return true;
}

static uint8_t rvswd_gpio_write_dmi_once(uint8_t address, uint32_t value) {
    uint8_t frame[7] = {0};
    uint8_t target[7] = {0};

    rvswd_pack_write(frame, address, value);
    if (!rvswd_transaction(frame, target, false)) {
        return 2u;
    }
    return rvswd_unpack_handshake(target);
}

bool rvswd_gpio_read_dmi(uint8_t address, uint32_t *value) {
    uint8_t frame[7] = {0};
    uint8_t target[7] = {0};
    uint8_t status;

    rvswd_pack_read(frame, address);
    for (uint8_t retry = 0u; retry < RVSWD_DMI_READ_RETRY_COUNT; ++retry) {
        if (!rvswd_transaction(frame, target, true)) {
            return false;
        }

        status = rvswd_unpack_handshake(target);
        if (status == RVSWD_STATUS_BUSY) {
            bsp_delay_us(RVSWD_DMI_BUSY_DELAY_US);
            continue;
        }
        if (!rvswd_status_is_ok(status)) {
            bsp_delay_us(RVSWD_DMI_ERROR_DELAY_US);
            continue;
        }
        *value = rvswd_unpack_data(target);
        if (rvswd_get_bit(target, 46u) != rvswd_xor_bits(*value)) {
            bsp_delay_us(RVSWD_DMI_ERROR_DELAY_US);
            continue;
        }
        return true;
    }

    return false;
}

static bool rvswd_try_connect(void) {
    const uint32_t unlock = 0x5aa50400u;

    for (uint8_t attempt = 0u; attempt < 3u; ++attempt) {
        uint32_t status = 0u;

        // 目标上电或复位后需要留出调试模块启动时间
        bsp_delay_ms(16u);

        // 通过短帧 DMI 解锁调试模块并读回配置签名
        // 初始化阶段按 DTM 管线推进请求，单次 BUSY 不重复占用同一请求
        (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_SHADOW, unlock);
        (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_CONFIG, unlock);
        (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_SHADOW, unlock);
        (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_CONFIG, unlock);
        (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_CONTROL, 0x80000001u);
        (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_CONTROL, 0x80000001u);
        (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_CONTROL, 0x80000001u);
        if (!rvswd_gpio_read_dmi(RVSWD_DMI_CONFIG, &status)) {
            continue;
        }
        if ((status & 0xffff0000u) == 0x5aa50000u) {
            return true;
        }
    }
    return false;
}

bool rvswd_gpio_connect(void) {
    return rvswd_try_connect();
}
