#include "rvswd_gpio.h"

#include "bsp/bsp_delay.h"

#include <stddef.h>

#include <ch32x035.h>

#define RVSWD_CLOCK_PIN GPIO_Pin_2
#define RVSWD_DATA_PIN  GPIO_Pin_3
#define RVSWD_PINS      (RVSWD_CLOCK_PIN | RVSWD_DATA_PIN)

#define RVSWD_DMI_CONTROL 0x10u
#define RVSWD_DMI_CONFIG  0x7du
#define RVSWD_DMI_SHADOW  0x7eu

#define RVSWD_STATUS_OK   1u
#define RVSWD_STATUS_BUSY 3u

#define RVSWD_DMI_WRITE_RETRY_COUNT     16u
#define RVSWD_DMI_READ_RETRY_COUNT      64u
#define RVSWD_MEMORY_READ_RETRY_COUNT   3u
#define RVSWD_DMI_BUSY_DELAY_US         100u
#define RVSWD_DMI_ERROR_DELAY_US        50u
#define RVSWD_INTERFRAME_GUARD_US       8u
#define RVSWD_ABSTRACT_COMMAND_DELAY_US 100u
#define RVSWD_ABSTRACT_TIMEOUT_US       10000u
#define RVSWD_RESUME_MIN_DELAY_US       1000u
#define RVSWD_EXECUTE_TIMEOUT_MS        5000u
#define RVSWD_LOADER_STACK_TOP          0x20005000u
#define RVSWD_DEBUG_UNLOCK              0x5aa50400u

#define RVSWD_CHIP_FAMILY_MASK 0xfff00000u
#define RVSWD_CHIP_FAMILY_X035 0x03500000u
#define RVSWD_CHIP_FAMILY_L103 0x10300000u
#define RVSWD_CHIP_FAMILY_V303 0x30300000u
#define RVSWD_CHIP_FAMILY_V305 0x30500000u
#define RVSWD_CHIP_FAMILY_V307 0x30700000u

#define RVSWD_FLASH_KEYR_ADDRESS     0x40022004u
#define RVSWD_FLASH_OBKEYR_ADDRESS   0x40022008u
#define RVSWD_FLASH_STATR_ADDRESS    0x4002200cu
#define RVSWD_FLASH_CTLR_ADDRESS     0x40022010u
#define RVSWD_FLASH_ADDR_ADDRESS     0x40022014u
#define RVSWD_FLASH_OBR_ADDRESS      0x4002201cu
#define RVSWD_FLASH_WPR_ADDRESS      0x40022020u
#define RVSWD_FLASH_MODEKEYR_ADDRESS 0x40022024u
#define RVSWD_OPTION_BYTES_ADDRESS   0x1ffff800u

#define RVSWD_FLASH_KEY1 0x45670123u
#define RVSWD_FLASH_KEY2 0xcdef89abu

#define RVSWD_FLASH_STATR_BUSY          (1u << 0u)
#define RVSWD_FLASH_STATR_WRPRTERR      (1u << 4u)
#define RVSWD_FLASH_STATR_EOP           (1u << 5u)
#define RVSWD_FLASH_CTLR_MER            (1u << 2u)
#define RVSWD_FLASH_CTLR_OPTION_PROGRAM (1u << 4u)
#define RVSWD_FLASH_CTLR_OPTER          (1u << 5u)
#define RVSWD_FLASH_CTLR_STRT           (1u << 6u)
#define RVSWD_FLASH_CTLR_LOCK           (1u << 7u)
#define RVSWD_FLASH_CTLR_OPTION_WRITE   (1u << 9u)
#define RVSWD_FLASH_CTLR_FAST_LOCK      (1u << 15u)
#define RVSWD_FLASH_CTLR_FAST_PROGRAM   (1u << 16u)
#define RVSWD_FLASH_CTLR_BUFFER_LOAD    (1u << 18u)
#define RVSWD_FLASH_CTLR_BUFFER_RESET   (1u << 19u)
#define RVSWD_FLASH_OBR_READ_PROTECTED  (1u << 1u)
#define RVSWD_OPTION_BYTES_WORD_COUNT   4u
#define RVSWD_OPTION_RDP_PROTECTED      0x00ffu
#define RVSWD_OPTION_RDP_UNPROTECTED    0x5aa5u
#define RVSWD_FLASH_ERASE_TIMEOUT_US    6000000u

#define RVSWD_WCHLINK_FAMILY_V30X 0x06u
#define RVSWD_WCHLINK_FAMILY_X035 0x0du
#define RVSWD_WCHLINK_FAMILY_L103 0x0eu

enum rvswd_flash_unlock_mode {
    RVSWD_FLASH_UNLOCK_MAIN_AND_FAST,
    RVSWD_FLASH_UNLOCK_MAIN_OPTION_AND_FAST,
};

enum rvswd_option_write_mode {
    RVSWD_OPTION_WRITE_FAST_BUFFER,
    RVSWD_OPTION_WRITE_HALFWORD,
};

struct rvswd_target_profile {
    uint8_t wchlink_family;
    enum rvswd_flash_unlock_mode erase_unlock;
    enum rvswd_option_write_mode option_write;
    uint32_t option_base;
};

static const struct rvswd_target_profile rvswd_target_profile_x035 = {
    .wchlink_family = RVSWD_WCHLINK_FAMILY_X035,
    .erase_unlock = RVSWD_FLASH_UNLOCK_MAIN_AND_FAST,
    .option_write = RVSWD_OPTION_WRITE_FAST_BUFFER,
    .option_base = RVSWD_OPTION_BYTES_ADDRESS,
};

static const struct rvswd_target_profile rvswd_target_profile_l103 = {
    .wchlink_family = RVSWD_WCHLINK_FAMILY_L103,
    .erase_unlock = RVSWD_FLASH_UNLOCK_MAIN_OPTION_AND_FAST,
    .option_write = RVSWD_OPTION_WRITE_FAST_BUFFER,
    .option_base = RVSWD_OPTION_BYTES_ADDRESS,
};

static const struct rvswd_target_profile rvswd_target_profile_v30x = {
    .wchlink_family = RVSWD_WCHLINK_FAMILY_V30X,
    .erase_unlock = RVSWD_FLASH_UNLOCK_MAIN_AND_FAST,
    .option_write = RVSWD_OPTION_WRITE_HALFWORD,
    .option_base = RVSWD_OPTION_BYTES_ADDRESS,
};

static uint32_t rvswd_flash_last_error;
static uint32_t rvswd_target_chip_id;
static uint8_t rvswd_expected_wchlink_family;
static bool rvswd_target_uses_family_hint;
static uint8_t rvswd_connect_last_error;
static uint8_t rvswd_memory_last_error;
static uint8_t rvswd_memory_failure_dmi_status;
static uint32_t rvswd_memory_failure_address;
static uint32_t rvswd_memory_failure_abstractcs;
static uint8_t rvswd_dmi_last_status;
static bool rvswd_dmi_failure_retryable;

static bool rvswd_gpio_wait_abstract_idle_timeout(uint32_t *abstractcs,
                                                  uint32_t timeout_us);
static bool rvswd_gpio_wait_abstract_idle(uint32_t *abstractcs);

static const struct rvswd_target_profile *rvswd_gpio_profile_from_chip_id(
    uint32_t chip_id) {
    switch (chip_id & RVSWD_CHIP_FAMILY_MASK) {
        case RVSWD_CHIP_FAMILY_X035:
            return &rvswd_target_profile_x035;
        case RVSWD_CHIP_FAMILY_L103:
            return &rvswd_target_profile_l103;
        case RVSWD_CHIP_FAMILY_V303:
        case RVSWD_CHIP_FAMILY_V305:
        case RVSWD_CHIP_FAMILY_V307:
            return &rvswd_target_profile_v30x;
        default:
            return NULL;
    }
}

static const struct rvswd_target_profile *rvswd_gpio_profile_from_wchlink_family(
    uint8_t family) {
    switch (family) {
        case RVSWD_WCHLINK_FAMILY_X035:
            return &rvswd_target_profile_x035;
        case RVSWD_WCHLINK_FAMILY_L103:
            return &rvswd_target_profile_l103;
        case RVSWD_WCHLINK_FAMILY_V30X:
            return &rvswd_target_profile_v30x;
        default:
            return NULL;
    }
}

static const struct rvswd_target_profile *rvswd_gpio_target_profile(void) {
    const struct rvswd_target_profile *profile =
        rvswd_gpio_profile_from_chip_id(rvswd_target_chip_id);

    if (profile != NULL || !rvswd_target_uses_family_hint) {
        return profile;
    }
    return rvswd_gpio_profile_from_wchlink_family(rvswd_expected_wchlink_family);
}

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
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n"
        "nop\n");
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
    // turnaround 开始前将锁存置高，释放数据线并启用内部上拉
    GPIOA->BSHR = RVSWD_DATA_PIN;
    GPIOA->CFGLR = (GPIOA->CFGLR & ~(0xfu << 12u)) | (0x08u << 12u);
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
    // 在时钟上升沿后采样，随后保持完整高电平周期
    value = (GPIOA->INDR & RVSWD_DATA_PIN) != 0u ? 1u : 0u;
    rvswd_half_period();
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

    // 头部校验后目标返回三位握手，主机再发送两个控制位
    rvswd_drive_range(host, 0u, 9u);
    rvswd_config_data_input();
    rvswd_sample_range(target, 9u, 3u);
    rvswd_config_data_output();
    rvswd_drive_range(host, 12u, 2u);

    if (read) {
        // 读操作由目标返回数据、校验和状态，主机发送两个收尾位
        rvswd_config_data_input();
        rvswd_sample_range(target, 14u, 36u);
        rvswd_config_data_output();
        rvswd_drive_range(host, 50u, 2u);
    } else {
        // 写操作由主机发送数据和校验，目标返回三位状态
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
    rvswd_set_bit(frame, 13u, 1u);
    rvswd_set_bit(frame, 51u, 1u);
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
    return status == RVSWD_STATUS_OK;
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
                   (0x01u << 8u) | (0x08u << 12u);
}

void rvswd_gpio_disconnect(void) {
    GPIOA->BSHR = RVSWD_PINS;
    // 会话结束后释放两根信号线，避免目标断电时通过调试引脚倒灌
    GPIOA->CFGLR = (GPIOA->CFGLR & ~((0xfu << 8u) | (0xfu << 12u))) |
                   (0x04u << 8u) | (0x04u << 12u);
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

static void rvswd_gpio_restore_debug_module(void) {
    // QingKe 调试模块启动和异常恢复时会丢失关键写入，重复配置确保命令生效
    (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_SHADOW, RVSWD_DEBUG_UNLOCK);
    (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_CONFIG, RVSWD_DEBUG_UNLOCK);
    (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_SHADOW, RVSWD_DEBUG_UNLOCK);
    (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_CONFIG, RVSWD_DEBUG_UNLOCK);
    (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_CONTROL, 0x80000001u);
    (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_CONTROL, 0x80000001u);
    (void)rvswd_gpio_write_dmi_once(RVSWD_DMI_CONTROL, 0x80000001u);
}

bool rvswd_gpio_write_dmi(uint8_t address, uint32_t value) {
    uint8_t frame[7] = {0};
    uint8_t target[7] = {0};
    uint8_t status;

    rvswd_pack_write(frame, address, value);
    rvswd_dmi_failure_retryable = false;
    for (uint8_t retry = 0u; retry < RVSWD_DMI_WRITE_RETRY_COUNT; ++retry) {
        if (!rvswd_transaction(frame, target, false)) {
            return false;
        }

        status = rvswd_unpack_handshake(target);
        rvswd_dmi_last_status = status;
        if (rvswd_status_is_ok(status)) {
            if (address == 0x17u) {
                // COMMAND 写入完成后留出执行窗口，避免下一次 DMI 访问撞上 abstract busy
                bsp_delay_us(RVSWD_ABSTRACT_COMMAND_DELAY_US);
            }
            return true;
        }
        if (status == RVSWD_STATUS_BUSY) {
            rvswd_dmi_failure_retryable = true;
            bsp_delay_us(RVSWD_DMI_BUSY_DELAY_US);
        } else {
            rvswd_dmi_failure_retryable = false;
            // 非成功状态允许短暂重试，避免单次线噪声中断连续内存传输
            bsp_delay_us(RVSWD_DMI_ERROR_DELAY_US);
        }
    }

    return false;
}

static bool rvswd_gpio_read_memory32_synchronized(uint32_t address, uint32_t *value) {
    uint32_t abstractcs;
    uint32_t data;

    // 使用 x8 执行 c.lw，避免连续内存访问时的寄存器别名问题
    rvswd_memory_last_error = 0u;
    if (!rvswd_gpio_write_dmi(0x16u, 0x00000700u)) {
        rvswd_memory_last_error = 0x81u;
        return false;
    }
    if (!rvswd_gpio_write_dmi(0x20u, 0x90024000u)) {
        rvswd_memory_last_error = 0x82u;
        return false;
    }
    if (!rvswd_gpio_write_dmi(0x04u, address)) {
        rvswd_memory_last_error = 0x83u;
        return false;
    }
    if (!rvswd_gpio_write_dmi(0x17u, 0x00271008u)) {
        rvswd_memory_last_error = 0x84u;
        return false;
    }
    if (!rvswd_gpio_wait_abstract_idle(&abstractcs)) {
        rvswd_memory_last_error = 0x85u;
        return false;
    }
    if (((abstractcs >> 8u) & 0x07u) != 0u) {
        rvswd_memory_last_error = 0x90u | (uint8_t)((abstractcs >> 8u) & 0x07u);
        return false;
    }
    if (!rvswd_gpio_write_dmi(0x17u, 0x00221008u)) {
        rvswd_memory_last_error = 0x86u;
        return false;
    }
    if (!rvswd_gpio_wait_abstract_idle(&abstractcs)) {
        rvswd_memory_last_error = 0x87u;
        return false;
    }
    if (((abstractcs >> 8u) & 0x07u) != 0u) {
        rvswd_memory_last_error = 0xa0u | (uint8_t)((abstractcs >> 8u) & 0x07u);
        return false;
    }
    if (!rvswd_gpio_read_dmi(0x04u, &data)) {
        rvswd_memory_last_error = 0x88u;
        return false;
    }

    *value = data;
    return true;
}

static bool rvswd_gpio_read_memory32_v30x_once(uint32_t address, uint32_t *value) {
    uint32_t abstractcs;
    uint32_t data;

    rvswd_memory_last_error = 0u;
    if (!rvswd_gpio_write_dmi(0x20u, 0x0002a303u)) {
        rvswd_memory_last_error = 0xb1u;
        return false;
    }
    if (!rvswd_gpio_write_dmi(0x21u, 0x00100073u)) {
        rvswd_memory_last_error = 0xb2u;
        return false;
    }
    if (!rvswd_gpio_write_dmi(0x04u, address)) {
        rvswd_memory_last_error = 0xb3u;
        return false;
    }
    if (!rvswd_gpio_write_dmi(0x16u, 0x00000700u)) {
        rvswd_memory_last_error = 0xb4u;
        return false;
    }
    if (!rvswd_gpio_write_dmi(0x17u, 0x00271005u)) {
        rvswd_memory_last_error = 0xb5u;
        return false;
    }
    bsp_delay_us(RVSWD_ABSTRACT_COMMAND_DELAY_US);
    if (!rvswd_gpio_wait_abstract_idle(&abstractcs)) {
        rvswd_memory_last_error = 0xb6u;
        return false;
    }
    if (((abstractcs >> 8u) & 0x07u) != 0u) {
        rvswd_memory_last_error = 0xc0u | (uint8_t)((abstractcs >> 8u) & 0x07u);
        return false;
    }
    if (!rvswd_gpio_write_dmi(0x17u, 0x00221006u)) {
        rvswd_memory_last_error = 0xb7u;
        return false;
    }
    bsp_delay_us(RVSWD_ABSTRACT_COMMAND_DELAY_US);
    if (!rvswd_gpio_wait_abstract_idle(&abstractcs)) {
        rvswd_memory_last_error = 0xb8u;
        return false;
    }
    if (((abstractcs >> 8u) & 0x07u) != 0u) {
        rvswd_memory_last_error = 0xd0u | (uint8_t)((abstractcs >> 8u) & 0x07u);
        return false;
    }
    if (!rvswd_gpio_read_dmi(0x04u, &data)) {
        rvswd_memory_last_error = 0xb9u;
        return false;
    }

    *value = data;
    return true;
}

static bool rvswd_gpio_read_memory32_v30x(uint32_t address, uint32_t *value) {
    for (uint8_t retry = 0u; retry < RVSWD_MEMORY_READ_RETRY_COUNT; ++retry) {
        if (rvswd_gpio_read_memory32_v30x_once(address, value)) {
            return true;
        }
    }
    return false;
}

bool rvswd_gpio_read_memory32(uint32_t address, uint32_t *value) {
    const struct rvswd_target_profile *profile = rvswd_gpio_target_profile();

    if (value == NULL) {
        return false;
    }
    if (profile == NULL) {
        profile =
            rvswd_gpio_profile_from_wchlink_family(rvswd_expected_wchlink_family);
    }
    if (profile != NULL &&
        profile->wchlink_family == RVSWD_WCHLINK_FAMILY_L103) {
        return rvswd_gpio_read_memory32_synchronized(address, value);
    }
    if (rvswd_gpio_read_memory32_v30x(address, value)) {
        return true;
    }
    if (profile != NULL || rvswd_target_chip_id != 0u) {
        return false;
    }

    // 连接阶段尚未取得 ChipID，V30X 失败后兼容 L103 重试
    return rvswd_gpio_read_memory32_synchronized(address, value);
}

bool rvswd_gpio_write_memory32(uint32_t address, uint32_t value) {
    uint32_t abstractcs;

    // 使用 x8 保存数据，x9 保存目标地址
    if (!rvswd_gpio_write_dmi(0x16u, 0x00000700u) ||
        !rvswd_gpio_write_dmi(0x20u, 0x0084a023u) ||
        !rvswd_gpio_write_dmi(0x21u, 0x00100073u) ||
        !rvswd_gpio_write_dmi(0x04u, address) ||
        !rvswd_gpio_write_dmi(0x17u, 0x00231009u) ||
        !rvswd_gpio_wait_abstract_idle(&abstractcs) ||
        ((abstractcs >> 8u) & 0x07u) != 0u ||
        !rvswd_gpio_write_dmi(0x04u, value) ||
        !rvswd_gpio_write_dmi(0x17u, 0x00271008u) ||
        !rvswd_gpio_wait_abstract_idle(&abstractcs)) {
        return false;
    }

    return ((abstractcs >> 8u) & 0x07u) == 0u;
}

static bool rvswd_gpio_write_memory16(uint32_t address, uint16_t value,
                                      uint32_t timeout_us) {
    uint32_t abstractcs;

    // 使用 x8 保存数据，x9 保存目标地址，Program Buffer 执行 sh
    if (!rvswd_gpio_write_dmi(0x18u, 0u) ||
        !rvswd_gpio_write_dmi(0x16u, 0x00000700u) ||
        !rvswd_gpio_write_dmi(0x20u, 0x00849023u) ||
        !rvswd_gpio_write_dmi(0x21u, 0x00100073u) ||
        !rvswd_gpio_write_dmi(0x04u, address) ||
        !rvswd_gpio_write_dmi(0x17u, 0x00231009u) ||
        !rvswd_gpio_wait_abstract_idle(&abstractcs) ||
        ((abstractcs >> 8u) & 0x07u) != 0u ||
        !rvswd_gpio_write_dmi(0x04u, value) ||
        !rvswd_gpio_write_dmi(0x17u, 0x00271008u) ||
        !rvswd_gpio_wait_abstract_idle_timeout(&abstractcs, timeout_us)) {
        return false;
    }

    return ((abstractcs >> 8u) & 0x07u) == 0u;
}

static bool rvswd_gpio_wait_abstract_idle_timeout(uint32_t *abstractcs,
                                                  uint32_t timeout_us) {
    uint64_t start = bsp_time_us();

    do {
        if (!rvswd_gpio_read_dmi(0x16u, abstractcs)) {
            if (!rvswd_dmi_failure_retryable) {
                return false;
            }
            continue;
        }
        if ((*abstractcs & (1u << 12u)) == 0u) {
            return true;
        }
    } while ((bsp_time_us() - start) < timeout_us);

    return false;
}

static bool rvswd_gpio_wait_abstract_idle(uint32_t *abstractcs) {
    return rvswd_gpio_wait_abstract_idle_timeout(abstractcs,
                                                 RVSWD_ABSTRACT_TIMEOUT_US);
}

static bool rvswd_gpio_write_memory_slow(uint32_t address, const uint8_t *data,
                                         uint32_t length) {
    uint32_t abstractcs;

    // 先配置一次程序缓冲区，后续每个字只更新地址和数据寄存器
    if (!rvswd_gpio_write_dmi(0x18u, 0u)) {
        rvswd_memory_last_error = 0xe1u;
        return false;
    }
    if (!rvswd_gpio_write_dmi(0x16u, 0x00000700u)) {
        rvswd_memory_last_error = 0xe2u;
        return false;
    }
    if (!rvswd_gpio_write_dmi(0x20u, 0x0084a023u)) {
        rvswd_memory_last_error = 0xe3u;
        return false;
    }
    if (!rvswd_gpio_write_dmi(0x21u, 0x00100073u)) {
        rvswd_memory_last_error = 0xe4u;
        return false;
    }

    for (uint32_t offset = 0u; offset < length; offset += 4u) {
        uint32_t value = ((uint32_t)data[offset + 0u]) |
                         ((uint32_t)data[offset + 1u] << 8u) |
                         ((uint32_t)data[offset + 2u] << 16u) |
                         ((uint32_t)data[offset + 3u] << 24u);

        rvswd_memory_failure_address = address + offset;
        if (!rvswd_gpio_write_dmi(0x16u, 0x00000700u)) {
            rvswd_memory_last_error = 0x10u | (rvswd_dmi_last_status & 0x03u);
            return false;
        }
        if (!rvswd_gpio_write_dmi(0x04u, address + offset)) {
            rvswd_memory_last_error = 0x20u | (rvswd_dmi_last_status & 0x03u);
            return false;
        }
        if (!rvswd_gpio_write_dmi(0x17u, 0x00231009u)) {
            rvswd_memory_last_error = 0x30u | (rvswd_dmi_last_status & 0x03u);
            return false;
        }
        if (!rvswd_gpio_write_dmi(0x04u, value)) {
            rvswd_memory_last_error = 0x40u | (rvswd_dmi_last_status & 0x03u);
            return false;
        }
        if (!rvswd_gpio_write_dmi(0x17u, 0x00271008u)) {
            rvswd_memory_last_error = 0x50u | (rvswd_dmi_last_status & 0x03u);
            return false;
        }
        if (!rvswd_gpio_wait_abstract_idle(&abstractcs)) {
            rvswd_memory_last_error = 0x60u | (rvswd_dmi_last_status & 0x03u);
            return false;
        }
        if (((abstractcs >> 8u) & 0x07u) != 0u) {
            rvswd_memory_last_error = 0x70u | (uint8_t)((abstractcs >> 8u) & 0x07u);
            return false;
        }
    }

    if (!rvswd_gpio_read_dmi(0x16u, &abstractcs)) {
        rvswd_memory_last_error = 0xe5u;
        return false;
    }
    if (((abstractcs >> 8u) & 0x07u) != 0u) {
        rvswd_memory_last_error = 0xd0u | (uint8_t)((abstractcs >> 8u) & 0x07u);
        return false;
    }
    return true;
}

bool rvswd_gpio_write_memory(uint32_t address, const uint8_t *data, uint32_t length) {
    bool success;

    rvswd_memory_last_error = 0u;
    rvswd_memory_failure_dmi_status = 0u;
    rvswd_memory_failure_address = address;
    rvswd_memory_failure_abstractcs = 0xffffffffu;
    if (data == NULL || length == 0u || (address & 3u) != 0u || (length & 3u) != 0u) {
        rvswd_memory_last_error = 0xefu;
        return false;
    }

    success = rvswd_gpio_write_memory_slow(address, data, length);
    if (!success) {
        rvswd_memory_failure_dmi_status = rvswd_dmi_last_status;
        (void)rvswd_gpio_read_dmi(0x16u, &rvswd_memory_failure_abstractcs);
    }
    return success;
}

uint8_t rvswd_gpio_memory_last_error(void) {
    return rvswd_memory_last_error;
}

uint8_t rvswd_gpio_memory_failure_dmi_status(void) {
    return rvswd_memory_failure_dmi_status;
}

uint32_t rvswd_gpio_memory_failure_address(void) {
    return rvswd_memory_failure_address;
}

uint32_t rvswd_gpio_memory_failure_abstractcs(void) {
    return rvswd_memory_failure_abstractcs;
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

static bool rvswd_gpio_wait_dmstatus(uint32_t mask, bool set, uint32_t timeout_ms) {
    uint64_t start = bsp_time_us();

    do {
        uint32_t status;

        if (!rvswd_gpio_read_dmi(0x11u, &status)) {
            return false;
        }
        if (((status & mask) != 0u) == set) {
            return true;
        }
        bsp_delay_us(100u);
    } while ((bsp_time_us() - start) < (uint64_t)timeout_ms * 1000u);

    return false;
}

bool rvswd_gpio_halt(void) {
    return rvswd_gpio_write_dmi(0x10u, 0x80000001u) &&
           rvswd_gpio_wait_dmstatus(1u << 9u, true, 100u);
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
    if (!rvswd_gpio_write_register(0x1002u, RVSWD_LOADER_STACK_TOP) ||
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
    // V30X 的 resumeack 会跨会话保持，给 resumereq 留出处理时间后直接等待 ebreak
    bsp_delay_us(RVSWD_RESUME_MIN_DELAY_US);
    if (!rvswd_gpio_write_dmi(0x10u, 0x00000001u)) {
        if (result != NULL) *result = 0xe006u;
        return false;
    }
    if (!rvswd_gpio_wait_dmstatus(1u << 9u, true, RVSWD_EXECUTE_TIMEOUT_MS)) {
        (void)rvswd_gpio_halt();
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

static bool rvswd_gpio_flash_wait_ready(uint32_t *status, uint8_t read_error,
                                        uint8_t timeout_error) {
    uint64_t start = bsp_time_us();

    do {
        if (!rvswd_gpio_read_memory32(RVSWD_FLASH_STATR_ADDRESS, status)) {
            rvswd_flash_last_error = read_error;
            return false;
        }
        if ((*status & RVSWD_FLASH_STATR_BUSY) == 0u) {
            return true;
        }
        bsp_delay_us(100u);
    } while ((bsp_time_us() - start) < RVSWD_FLASH_ERASE_TIMEOUT_US);

    rvswd_flash_last_error = timeout_error;
    return false;
}

static bool rvswd_gpio_flash_unlock_main_option_and_fast(uint32_t control) {
    if ((control & (RVSWD_FLASH_CTLR_LOCK | RVSWD_FLASH_CTLR_FAST_LOCK)) == 0u) {
        return true;
    }

    // L103 需要同时解锁主存储区、用户字和快速编程模式
    return rvswd_gpio_write_memory32(RVSWD_FLASH_KEYR_ADDRESS, RVSWD_FLASH_KEY1) &&
           rvswd_gpio_write_memory32(RVSWD_FLASH_KEYR_ADDRESS, RVSWD_FLASH_KEY2) &&
           rvswd_gpio_write_memory32(RVSWD_FLASH_OBKEYR_ADDRESS, RVSWD_FLASH_KEY1) &&
           rvswd_gpio_write_memory32(RVSWD_FLASH_OBKEYR_ADDRESS, RVSWD_FLASH_KEY2) &&
           rvswd_gpio_write_memory32(RVSWD_FLASH_MODEKEYR_ADDRESS, RVSWD_FLASH_KEY1) &&
           rvswd_gpio_write_memory32(RVSWD_FLASH_MODEKEYR_ADDRESS, RVSWD_FLASH_KEY2);
}

static bool rvswd_gpio_flash_unlock_main_and_fast(uint32_t control) {
    if ((control & RVSWD_FLASH_CTLR_LOCK) != 0u &&
        (!rvswd_gpio_write_memory32(RVSWD_FLASH_KEYR_ADDRESS, RVSWD_FLASH_KEY1) ||
         !rvswd_gpio_write_memory32(RVSWD_FLASH_KEYR_ADDRESS, RVSWD_FLASH_KEY2))) {
        return false;
    }
    if ((control & RVSWD_FLASH_CTLR_FAST_LOCK) != 0u &&
        (!rvswd_gpio_write_memory32(RVSWD_FLASH_MODEKEYR_ADDRESS, RVSWD_FLASH_KEY1) ||
         !rvswd_gpio_write_memory32(RVSWD_FLASH_MODEKEYR_ADDRESS, RVSWD_FLASH_KEY2))) {
        return false;
    }
    return true;
}

bool rvswd_gpio_flash_erase_all(void) {
    const struct rvswd_target_profile *profile = rvswd_gpio_target_profile();
    uint32_t control;
    uint32_t idle_control;
    uint32_t status;
    bool unlocked;
    bool success = false;

    rvswd_flash_last_error = 0u;
    if (profile == NULL) {
        rvswd_flash_last_error = 0x0fu;
        return false;
    }

    if (!rvswd_gpio_flash_wait_ready(&status, 0x11u, 0x12u)) {
        return false;
    }

    if (!rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control)) {
        rvswd_flash_last_error = 0x13u;
        return false;
    }

    switch (profile->erase_unlock) {
        case RVSWD_FLASH_UNLOCK_MAIN_OPTION_AND_FAST:
            unlocked = rvswd_gpio_flash_unlock_main_option_and_fast(control);
            break;
        case RVSWD_FLASH_UNLOCK_MAIN_AND_FAST:
            unlocked = rvswd_gpio_flash_unlock_main_and_fast(control);
            break;
        default:
            rvswd_flash_last_error = 0x0fu;
            return false;
    }
    if (!unlocked) {
        rvswd_flash_last_error = 0x14u;
        return false;
    }

    if (!rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control)) {
        rvswd_flash_last_error = 0x15u;
        return false;
    }
    if ((control & (RVSWD_FLASH_CTLR_LOCK | RVSWD_FLASH_CTLR_FAST_LOCK)) != 0u) {
        rvswd_flash_last_error = 0x16u;
        return false;
    }

    idle_control = control & ~(RVSWD_FLASH_CTLR_MER | RVSWD_FLASH_CTLR_STRT);

    // 清除上一次操作遗留的完成和写保护状态，避免误判本次擦除
    if ((status & (RVSWD_FLASH_STATR_EOP | RVSWD_FLASH_STATR_WRPRTERR)) != 0u &&
        !rvswd_gpio_write_memory32(
            RVSWD_FLASH_STATR_ADDRESS,
            status & (RVSWD_FLASH_STATR_EOP | RVSWD_FLASH_STATR_WRPRTERR))) {
        rvswd_flash_last_error = 0x17u;
        goto cleanup;
    }

    if (!rvswd_gpio_write_memory32(RVSWD_FLASH_CTLR_ADDRESS, idle_control)) {
        rvswd_flash_last_error = 0x18u;
        goto cleanup;
    }
    if (!rvswd_gpio_write_memory32(RVSWD_FLASH_CTLR_ADDRESS,
                                   idle_control | RVSWD_FLASH_CTLR_MER)) {
        rvswd_flash_last_error = 0x19u;
        goto cleanup;
    }
    if (!rvswd_gpio_write_memory32(
            RVSWD_FLASH_CTLR_ADDRESS,
            idle_control | RVSWD_FLASH_CTLR_MER | RVSWD_FLASH_CTLR_STRT)) {
        rvswd_flash_last_error = 0x1au;
        goto cleanup;
    }
    if (!rvswd_gpio_flash_wait_ready(&status, 0x1bu, 0x1cu)) {
        goto cleanup;
    }
    if ((status & RVSWD_FLASH_STATR_WRPRTERR) != 0u) {
        rvswd_flash_last_error = 0x1du;
        goto cleanup;
    }
    success = true;

cleanup:
    if (!rvswd_gpio_write_memory32(RVSWD_FLASH_CTLR_ADDRESS, idle_control)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x1eu;
        }
        success = false;
    }
    return success;
}

bool rvswd_gpio_flash_read_protected(bool *protected) {
    const struct rvswd_target_profile *profile = rvswd_gpio_target_profile();
    uint32_t option_status;

    rvswd_flash_last_error = 0u;
    if (protected == NULL) {
        rvswd_flash_last_error = 0x21u;
        return false;
    }
    if (profile == NULL) {
        rvswd_flash_last_error = 0x22u;
        return false;
    }
    if (!rvswd_gpio_read_memory32(RVSWD_FLASH_OBR_ADDRESS, &option_status)) {
        rvswd_flash_last_error = 0x23u;
        return false;
    }

    *protected = (option_status & RVSWD_FLASH_OBR_READ_PROTECTED) != 0u;
    return true;
}

bool rvswd_gpio_flash_write_protected(bool *protected) {
    const struct rvswd_target_profile *profile = rvswd_gpio_target_profile();
    uint32_t write_protection;

    rvswd_flash_last_error = 0u;
    if (protected == NULL) {
        rvswd_flash_last_error = 0x24u;
        return false;
    }
    if (profile == NULL) {
        rvswd_flash_last_error = 0x25u;
        return false;
    }
    if (!rvswd_gpio_read_memory32(RVSWD_FLASH_WPR_ADDRESS, &write_protection)) {
        rvswd_flash_last_error = 0x26u;
        return false;
    }

    *protected = write_protection != 0xffffffffu;
    return true;
}

static bool rvswd_gpio_flash_unlock_option_bytes(bool unlock_fast_mode) {
    if (!rvswd_gpio_write_memory32(RVSWD_FLASH_KEYR_ADDRESS, RVSWD_FLASH_KEY1)) {
        rvswd_flash_last_error = 0xa1u;
        return false;
    }
    if (!rvswd_gpio_write_memory32(RVSWD_FLASH_KEYR_ADDRESS, RVSWD_FLASH_KEY2)) {
        rvswd_flash_last_error = 0xa2u;
        return false;
    }
    if (unlock_fast_mode) {
        if (!rvswd_gpio_write_memory32(RVSWD_FLASH_MODEKEYR_ADDRESS,
                                       RVSWD_FLASH_KEY1)) {
            rvswd_flash_last_error = 0xa3u;
            return false;
        }
        if (!rvswd_gpio_write_memory32(RVSWD_FLASH_MODEKEYR_ADDRESS,
                                       RVSWD_FLASH_KEY2)) {
            rvswd_flash_last_error = 0xa4u;
            return false;
        }
    }
    if (!rvswd_gpio_write_memory32(RVSWD_FLASH_OBKEYR_ADDRESS,
                                   RVSWD_FLASH_KEY1)) {
        rvswd_flash_last_error = 0xa5u;
        return false;
    }
    if (!rvswd_gpio_write_memory32(RVSWD_FLASH_OBKEYR_ADDRESS,
                                   RVSWD_FLASH_KEY2)) {
        rvswd_flash_last_error = 0xa6u;
        return false;
    }
    return true;
}

static bool rvswd_gpio_flash_write_option_bytes_fast_buffer(
    const struct rvswd_target_profile *profile, const uint32_t *option_words) {
    uint32_t control;
    uint32_t idle_control = 0u;
    uint32_t status;
    bool success = false;

    if (!rvswd_gpio_flash_wait_ready(&status, 0x31u, 0x32u) ||
        !rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x33u;
        }
        return false;
    }
    if (!rvswd_gpio_flash_unlock_option_bytes(true)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x34u;
        }
        return false;
    }
    if (!rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control)) {
        rvswd_flash_last_error = 0x35u;
        return false;
    }
    if ((control & (RVSWD_FLASH_CTLR_LOCK | RVSWD_FLASH_CTLR_FAST_LOCK)) != 0u ||
        (control & RVSWD_FLASH_CTLR_OPTION_WRITE) == 0u) {
        rvswd_flash_last_error = 0x36u;
        return false;
    }

    idle_control = control & ~(RVSWD_FLASH_CTLR_OPTER | RVSWD_FLASH_CTLR_STRT |
                               RVSWD_FLASH_CTLR_FAST_PROGRAM |
                               RVSWD_FLASH_CTLR_BUFFER_LOAD |
                               RVSWD_FLASH_CTLR_BUFFER_RESET);

    // Option Bytes 擦除和重写必须保持完整的 16 字节镜像
    if (!rvswd_gpio_write_memory32(RVSWD_FLASH_CTLR_ADDRESS,
                                   idle_control | RVSWD_FLASH_CTLR_OPTER) ||
        !rvswd_gpio_write_memory32(
            RVSWD_FLASH_CTLR_ADDRESS,
            idle_control | RVSWD_FLASH_CTLR_OPTER | RVSWD_FLASH_CTLR_STRT) ||
        !rvswd_gpio_flash_wait_ready(&status, 0x36u, 0x37u)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x38u;
        }
        goto cleanup;
    }
    if ((status & RVSWD_FLASH_STATR_WRPRTERR) != 0u) {
        rvswd_flash_last_error = 0x39u;
        goto cleanup;
    }

    // 选项字擦除完成后重新解锁快速编程模式，缓冲写入不保持 OPTWRE
    if (!rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control) ||
        !rvswd_gpio_flash_unlock_main_and_fast(control) ||
        !rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control)) {
        rvswd_flash_last_error = 0x3au;
        goto cleanup;
    }
    if ((control & (RVSWD_FLASH_CTLR_LOCK | RVSWD_FLASH_CTLR_FAST_LOCK)) != 0u) {
        rvswd_flash_last_error = 0x3bu;
        goto cleanup;
    }

    idle_control = control & ~(RVSWD_FLASH_CTLR_OPTION_WRITE |
                               RVSWD_FLASH_CTLR_OPTER | RVSWD_FLASH_CTLR_STRT |
                               RVSWD_FLASH_CTLR_FAST_PROGRAM |
                               RVSWD_FLASH_CTLR_BUFFER_LOAD |
                               RVSWD_FLASH_CTLR_BUFFER_RESET);

    if (!rvswd_gpio_write_memory32(RVSWD_FLASH_CTLR_ADDRESS,
                                   idle_control | RVSWD_FLASH_CTLR_FAST_PROGRAM) ||
        !rvswd_gpio_write_memory32(
            RVSWD_FLASH_CTLR_ADDRESS,
            idle_control | RVSWD_FLASH_CTLR_FAST_PROGRAM |
                RVSWD_FLASH_CTLR_BUFFER_RESET) ||
        !rvswd_gpio_flash_wait_ready(&status, 0x3au, 0x3bu) ||
        !rvswd_gpio_write_memory32(RVSWD_FLASH_CTLR_ADDRESS, idle_control)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x3cu;
        }
        goto cleanup;
    }

    for (uint32_t index = 0u; index < RVSWD_OPTION_BYTES_WORD_COUNT; ++index) {
        if (!rvswd_gpio_write_memory32(RVSWD_FLASH_CTLR_ADDRESS,
                                       idle_control | RVSWD_FLASH_CTLR_FAST_PROGRAM) ||
            !rvswd_gpio_write_memory32(profile->option_base + index * 4u,
                                       option_words[index]) ||
            !rvswd_gpio_write_memory32(
                RVSWD_FLASH_CTLR_ADDRESS,
                idle_control | RVSWD_FLASH_CTLR_FAST_PROGRAM |
                    RVSWD_FLASH_CTLR_BUFFER_LOAD) ||
            !rvswd_gpio_flash_wait_ready(&status, 0x3du, 0x3eu) ||
            !rvswd_gpio_write_memory32(RVSWD_FLASH_CTLR_ADDRESS, idle_control)) {
            if (rvswd_flash_last_error == 0u) {
                rvswd_flash_last_error = 0x3fu;
            }
            goto cleanup;
        }
    }

    if (!rvswd_gpio_write_memory32(RVSWD_FLASH_CTLR_ADDRESS,
                                   idle_control | RVSWD_FLASH_CTLR_FAST_PROGRAM) ||
        !rvswd_gpio_write_memory32(RVSWD_FLASH_ADDR_ADDRESS, profile->option_base) ||
        !rvswd_gpio_write_memory32(
            RVSWD_FLASH_CTLR_ADDRESS,
            idle_control | RVSWD_FLASH_CTLR_FAST_PROGRAM | RVSWD_FLASH_CTLR_STRT) ||
        !rvswd_gpio_flash_wait_ready(&status, 0x40u, 0x41u)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x42u;
        }
        goto cleanup;
    }
    if ((status & RVSWD_FLASH_STATR_WRPRTERR) != 0u) {
        rvswd_flash_last_error = 0x43u;
        goto cleanup;
    }
    success = true;

cleanup:
    if (!rvswd_gpio_write_memory32(
            RVSWD_FLASH_CTLR_ADDRESS,
            (idle_control & ~RVSWD_FLASH_CTLR_OPTION_WRITE) |
                RVSWD_FLASH_CTLR_LOCK | RVSWD_FLASH_CTLR_FAST_LOCK)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x44u;
        }
        success = false;
    }
    return success;
}

static bool rvswd_gpio_flash_write_option_bytes_halfword(
    const struct rvswd_target_profile *profile, const uint32_t *option_words) {
    uint32_t control;
    uint32_t idle_control = 0u;
    uint32_t status;
    bool success = false;

    if (!rvswd_gpio_flash_wait_ready(&status, 0x51u, 0x52u) ||
        !rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x53u;
        }
        return false;
    }
    if (!rvswd_gpio_flash_unlock_option_bytes(true) ||
        !rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x54u;
        }
        return false;
    }
    if ((control & (RVSWD_FLASH_CTLR_LOCK | RVSWD_FLASH_CTLR_FAST_LOCK)) != 0u ||
        (control & RVSWD_FLASH_CTLR_OPTION_WRITE) == 0u) {
        rvswd_flash_last_error = 0x55u;
        return false;
    }

    idle_control = control & ~(RVSWD_FLASH_CTLR_OPTION_PROGRAM |
                               RVSWD_FLASH_CTLR_OPTER | RVSWD_FLASH_CTLR_STRT);

    // Option Bytes 擦除和重写必须保持完整的 16 字节镜像
    if (!rvswd_gpio_write_memory32(RVSWD_FLASH_CTLR_ADDRESS,
                                   idle_control | RVSWD_FLASH_CTLR_OPTER) ||
        !rvswd_gpio_write_memory32(
            RVSWD_FLASH_CTLR_ADDRESS,
            idle_control | RVSWD_FLASH_CTLR_OPTER | RVSWD_FLASH_CTLR_STRT) ||
        !rvswd_gpio_flash_wait_ready(&status, 0x56u, 0x57u)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x58u;
        }
        goto cleanup;
    }
    if ((status & RVSWD_FLASH_STATR_WRPRTERR) != 0u) {
        rvswd_flash_last_error = 0x59u;
        goto cleanup;
    }

    // Option Bytes 擦除后重新解锁主存储区、快速模式和 Option Bytes
    if (!rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control) ||
        !rvswd_gpio_flash_unlock_option_bytes(true) ||
        !rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control)) {
        rvswd_flash_last_error = 0x5au;
        goto cleanup;
    }
    if ((control & (RVSWD_FLASH_CTLR_LOCK | RVSWD_FLASH_CTLR_FAST_LOCK)) != 0u ||
        (control & RVSWD_FLASH_CTLR_OPTION_WRITE) == 0u) {
        rvswd_flash_last_error = 0x5bu;
        goto cleanup;
    }

    idle_control = control & ~(RVSWD_FLASH_CTLR_OPTION_PROGRAM |
                               RVSWD_FLASH_CTLR_OPTER | RVSWD_FLASH_CTLR_STRT);

    for (uint32_t index = 0u; index < RVSWD_OPTION_BYTES_WORD_COUNT * 2u; ++index) {
        uint16_t value = (uint16_t)(option_words[index / 2u] >>
                                    ((index & 1u) * 16u));

        if (!rvswd_gpio_write_memory32(
                RVSWD_FLASH_CTLR_ADDRESS,
                idle_control | RVSWD_FLASH_CTLR_OPTION_PROGRAM) ||
            !rvswd_gpio_write_memory16(profile->option_base + index * 2u, value,
                                       RVSWD_ABSTRACT_TIMEOUT_US) ||
            !rvswd_gpio_flash_wait_ready(&status, 0x5cu, 0x5du)) {
            if (rvswd_flash_last_error == 0u) {
                rvswd_flash_last_error = 0x5eu;
            }
            goto cleanup;
        }
        if ((status & RVSWD_FLASH_STATR_WRPRTERR) != 0u) {
            rvswd_flash_last_error = 0x5fu;
            goto cleanup;
        }
    }
    success = true;

cleanup:
    if (!rvswd_gpio_write_memory32(
            RVSWD_FLASH_CTLR_ADDRESS,
            (idle_control & ~RVSWD_FLASH_CTLR_OPTION_WRITE) |
                RVSWD_FLASH_CTLR_LOCK | RVSWD_FLASH_CTLR_FAST_LOCK)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x60u;
        }
        success = false;
    }
    return success;
}

static bool rvswd_gpio_flash_unprotect_option_bytes(
    const struct rvswd_target_profile *profile) {
    uint32_t control;
    uint32_t idle_control = 0u;
    uint32_t status;
    bool success = false;

    if (!rvswd_gpio_flash_wait_ready(&status, 0x61u, 0x62u) ||
        !rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x63u;
        }
        return false;
    }
    if (!rvswd_gpio_flash_unlock_option_bytes(true) ||
        !rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x64u;
        }
        return false;
    }
    if ((control & RVSWD_FLASH_CTLR_LOCK) != 0u ||
        (control & RVSWD_FLASH_CTLR_OPTION_WRITE) == 0u) {
        rvswd_flash_last_error = 0x65u;
        return false;
    }

    idle_control = control & ~(RVSWD_FLASH_CTLR_OPTION_PROGRAM |
                               RVSWD_FLASH_CTLR_OPTER | RVSWD_FLASH_CTLR_STRT);
    if (!rvswd_gpio_write_memory32(RVSWD_FLASH_CTLR_ADDRESS,
                                   idle_control | RVSWD_FLASH_CTLR_OPTER) ||
        !rvswd_gpio_write_memory32(
            RVSWD_FLASH_CTLR_ADDRESS,
            idle_control | RVSWD_FLASH_CTLR_OPTER | RVSWD_FLASH_CTLR_STRT) ||
        !rvswd_gpio_flash_wait_ready(&status, 0x66u, 0x67u)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x68u;
        }
        goto cleanup;
    }
    if ((status & RVSWD_FLASH_STATR_WRPRTERR) != 0u) {
        rvswd_flash_last_error = 0x69u;
        goto cleanup;
    }

    // 解除读保护只恢复 RDP，保留 Option Bytes 擦除后的默认状态
    if (!rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control) ||
        !rvswd_gpio_flash_unlock_option_bytes(true) ||
        !rvswd_gpio_read_memory32(RVSWD_FLASH_CTLR_ADDRESS, &control)) {
        rvswd_flash_last_error = 0x6au;
        goto cleanup;
    }
    if ((control & RVSWD_FLASH_CTLR_LOCK) != 0u ||
        (control & RVSWD_FLASH_CTLR_OPTION_WRITE) == 0u) {
        rvswd_flash_last_error = 0x6bu;
        goto cleanup;
    }

    idle_control = control & ~(RVSWD_FLASH_CTLR_OPTION_PROGRAM |
                               RVSWD_FLASH_CTLR_OPTER | RVSWD_FLASH_CTLR_STRT);
    if (!rvswd_gpio_write_memory32(
            RVSWD_FLASH_CTLR_ADDRESS,
            idle_control | RVSWD_FLASH_CTLR_OPTION_PROGRAM)) {
        rvswd_flash_last_error = 0x71u;
        goto cleanup;
    }
    if (!rvswd_gpio_write_memory16(profile->option_base,
                                   RVSWD_OPTION_RDP_UNPROTECTED,
                                   RVSWD_FLASH_ERASE_TIMEOUT_US)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x72u;
        }
        goto cleanup;
    }
    if (!rvswd_gpio_flash_wait_ready(&status, 0x6cu, 0x6du)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x73u;
        }
        goto cleanup;
    }
    if ((status & RVSWD_FLASH_STATR_WRPRTERR) != 0u) {
        rvswd_flash_last_error = 0x6fu;
        goto cleanup;
    }
    success = true;

cleanup:
    if (!rvswd_gpio_write_memory32(
            RVSWD_FLASH_CTLR_ADDRESS,
            (idle_control & ~RVSWD_FLASH_CTLR_OPTION_WRITE) |
                RVSWD_FLASH_CTLR_LOCK)) {
        if (rvswd_flash_last_error == 0u) {
            rvswd_flash_last_error = 0x70u;
        }
        success = false;
    }
    return success;
}

bool rvswd_gpio_flash_set_read_protected(bool protected) {
    const struct rvswd_target_profile *profile = rvswd_gpio_target_profile();
    uint32_t option_words[RVSWD_OPTION_BYTES_WORD_COUNT];
    bool current;

    if (profile == NULL) {
        rvswd_flash_last_error = 0x22u;
        return false;
    }
    if (!rvswd_gpio_flash_read_protected(&current)) {
        return false;
    }
    if (current == protected) {
        return true;
    }

    if (!protected && profile->option_write == RVSWD_OPTION_WRITE_FAST_BUFFER) {
        if (!rvswd_gpio_flash_unprotect_option_bytes(profile)) {
            return false;
        }
    } else {
        for (uint32_t index = 0u; index < RVSWD_OPTION_BYTES_WORD_COUNT; ++index) {
            if (!rvswd_gpio_read_memory32(profile->option_base + index * 4u,
                                          &option_words[index])) {
                rvswd_flash_last_error = 0x45u;
                return false;
            }
        }

        option_words[0] = (option_words[0] & 0xffff0000u) |
                          (protected ? RVSWD_OPTION_RDP_PROTECTED
                                     : RVSWD_OPTION_RDP_UNPROTECTED);
        switch (profile->option_write) {
            case RVSWD_OPTION_WRITE_FAST_BUFFER:
                if (!rvswd_gpio_flash_write_option_bytes_fast_buffer(profile,
                                                                     option_words)) {
                    return false;
                }
                break;
            case RVSWD_OPTION_WRITE_HALFWORD:
                if (!rvswd_gpio_flash_write_option_bytes_halfword(profile,
                                                                  option_words)) {
                    return false;
                }
                break;
            default:
                rvswd_flash_last_error = 0x5fu;
                return false;
        }
    }

    // 解除读保护时目标硬件会自动整片擦除主存储区，复位后 OBR 才加载新状态
    if (!rvswd_gpio_reset_and_halt()) {
        rvswd_flash_last_error = 0x46u;
        return false;
    }
    if (!rvswd_gpio_flash_read_protected(&current)) {
        return false;
    }
    if (current != protected) {
        rvswd_flash_last_error = 0x47u;
        return false;
    }
    return true;
}

uint32_t rvswd_gpio_flash_last_error(void) {
    return rvswd_flash_last_error;
}

bool rvswd_gpio_reset_and_halt(void) {
    // ndmreset 保持 Debug Module 工作，释放后重新停住目标核
    if (!rvswd_gpio_write_dmi(RVSWD_DMI_CONTROL, 0x80000003u)) {
        return false;
    }
    bsp_delay_us(1000u);
    return rvswd_gpio_write_dmi(RVSWD_DMI_CONTROL, 0x80000001u) &&
           rvswd_gpio_wait_dmstatus(1u << 9u, true, 100u);
}

bool rvswd_gpio_reset_and_run(void) {
    // 不设置 haltreq，释放 ndmreset 后让目标从复位向量继续运行
    if (!rvswd_gpio_write_dmi(RVSWD_DMI_CONTROL, 0x00000003u)) {
        return false;
    }
    bsp_delay_us(1000u);
    return rvswd_gpio_write_dmi(RVSWD_DMI_CONTROL, 0x00000001u);
}

bool rvswd_gpio_read_dmi(uint8_t address, uint32_t *value) {
    uint8_t frame[7] = {0};
    uint8_t target[7] = {0};
    uint8_t status;

    rvswd_pack_read(frame, address);
    rvswd_dmi_failure_retryable = false;
    for (uint8_t retry = 0u; retry < RVSWD_DMI_READ_RETRY_COUNT; ++retry) {
        if (!rvswd_transaction(frame, target, true)) {
            return false;
        }

        status = rvswd_unpack_handshake(target);
        rvswd_dmi_last_status = status;
        if (status == RVSWD_STATUS_BUSY) {
            rvswd_dmi_failure_retryable = true;
            bsp_delay_us(RVSWD_DMI_BUSY_DELAY_US);
            continue;
        }
        if (!rvswd_status_is_ok(status)) {
            rvswd_dmi_failure_retryable = false;
            bsp_delay_us(RVSWD_DMI_ERROR_DELAY_US);
            continue;
        }
        *value = rvswd_unpack_data(target);
        if (rvswd_get_bit(target, 46u) != rvswd_xor_bits(*value)) {
            rvswd_dmi_failure_retryable = true;
            bsp_delay_us(RVSWD_DMI_ERROR_DELAY_US);
            continue;
        }
        return true;
    }

    return false;
}

bool rvswd_gpio_dmi_failure_retryable(void) {
    return rvswd_dmi_failure_retryable;
}

static bool rvswd_gpio_identify_target(void) {
    const struct rvswd_target_profile *expected_profile =
        rvswd_gpio_profile_from_wchlink_family(rvswd_expected_wchlink_family);
    uint32_t option_status;

    if (rvswd_gpio_read_memory32(0x1ffff704u, &rvswd_target_chip_id) &&
        rvswd_target_chip_id != 0u) {
        return true;
    }
    if (expected_profile != NULL &&
        rvswd_gpio_read_memory32(RVSWD_FLASH_OBR_ADDRESS, &option_status) &&
        (option_status & RVSWD_FLASH_OBR_READ_PROTECTED) != 0u) {
        // ChipID 读取失败时，受限会话使用主机 SetSpeed 提示的 profile
        rvswd_target_uses_family_hint = true;
        return true;
    }
    return false;
}

static bool rvswd_try_connect(void) {
    rvswd_connect_last_error = 0u;
    rvswd_target_chip_id = 0u;
    rvswd_target_uses_family_hint = false;
    for (uint8_t attempt = 0u; attempt < 3u; ++attempt) {
        uint32_t config = 0u;
        uint32_t dmstatus = 0u;
        bool config_read;

        // 目标上电或复位后需要留出调试模块启动时间
        bsp_delay_ms(16u);

        // 通过短帧 DMI 解锁调试模块并读回配置签名
        // 初始化阶段按 DTM 管线推进请求，单次 BUSY 不重复占用同一请求
        rvswd_gpio_restore_debug_module();
        config_read = rvswd_gpio_read_dmi(RVSWD_DMI_CONFIG, &config);
        if (config_read && (config & 0xffff0000u) == 0x5aa50000u) {
            if (rvswd_gpio_identify_target()) {
                return true;
            }
            rvswd_connect_last_error = 0x13u;
        }

        // 失败诊断继续读取 DMSTATUS，区分严格签名不匹配和链路不可用
        if (rvswd_gpio_read_dmi(0x11u, &dmstatus)) {
            uint8_t version = (uint8_t)(dmstatus & 0x0fu);

            // 当前支持的 QingKe V4 目标仅接受 Debug 0.13.2
            if (version == 2u) {
                if (rvswd_gpio_identify_target()) {
                    return true;
                }
                rvswd_connect_last_error = 0x13u;
                continue;
            }
            rvswd_connect_last_error = 0x20u | version;
        } else {
            rvswd_connect_last_error = config_read ? 0x11u : 0x12u;
        }
    }
    return false;
}

bool rvswd_gpio_connect(void) {
    return rvswd_try_connect();
}

uint8_t rvswd_gpio_connect_last_error(void) {
    return rvswd_connect_last_error;
}

uint32_t rvswd_gpio_target_chip_id(void) {
    return rvswd_target_chip_id;
}

void rvswd_gpio_set_target_wchlink_family_hint(uint8_t family) {
    rvswd_expected_wchlink_family = family;
    rvswd_target_uses_family_hint = false;
}

uint8_t rvswd_gpio_target_wchlink_family(void) {
    const struct rvswd_target_profile *profile = rvswd_gpio_target_profile();

    if (profile != NULL) {
        return profile->wchlink_family;
    }
    profile = rvswd_gpio_profile_from_wchlink_family(rvswd_expected_wchlink_family);
    return profile == NULL ? 0u : profile->wchlink_family;
}
