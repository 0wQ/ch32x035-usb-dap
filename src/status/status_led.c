#include "status/status_led.h"

#include "bsp/bsp_delay.h"
#include "drv/drv_ws2816c.h"

#include <stdint.h>

// 每个状态一种固定颜色，统一走呼吸动画，只有周期不同：
//   IDLE 红慢呼吸、CONNECTED 绿慢呼吸、RUNNING 蓝但周期减半
//
// 出帧限速
#define FRAME_INTERVAL_MS 10u
#define BREATH_MS         2000u
#define BREATH_FAST_MS    1000u
#define RETRY_MS          1000u
// 三个颜色共用的通道电平（16 位），只影响亮度不影响色相
#define COLOR_LEVEL 0x0400u

enum led_state {
    LED_STATE_IDLE,
    LED_STATE_CONNECTED,
    LED_STATE_RUNNING,
};

// 三个状态只有点亮的通道不同，电平值完全一致
static const drv_ws2816c_pixel_t state_colors[] = {
    [LED_STATE_IDLE] = {.red = COLOR_LEVEL},
    [LED_STATE_CONNECTED] = {.green = COLOR_LEVEL},
    [LED_STATE_RUNNING] = {.blue = COLOR_LEVEL},
};

static enum led_state led_state;
static bool display_valid;
static drv_ws2816c_pixel_t display_pixel;
static uint32_t next_frame_ms;
static uint32_t retry_deadline_ms;
static bool retry_armed;

static bool time_reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

// 三角波过 smoothstep 3x^2-2x^3，返回 0..255，无浮点也无查表
// 相位直接取自绝对时间，全局唯一周期，因此没有需要维护的呼吸状态
static uint32_t breath_coefficient(uint32_t now, uint32_t period) {
    uint32_t half = period / 2u;
    uint32_t phase = now % period;
    uint32_t t = phase < half ? phase : period - phase;
    uint32_t x = t * 255u / half;
    return x * x * (768u - 2u * x) / 65536u;
}

static drv_ws2816c_pixel_t scaled_pixel(drv_ws2816c_pixel_t base, uint32_t coefficient) {
    drv_ws2816c_pixel_t pixel;
    pixel.red = (uint16_t)((uint32_t)base.red * coefficient / 255u);
    pixel.green = (uint16_t)((uint32_t)base.green * coefficient / 255u);
    pixel.blue = (uint16_t)((uint32_t)base.blue * coefficient / 255u);
    return pixel;
}

static bool pixel_equal(drv_ws2816c_pixel_t a, drv_ws2816c_pixel_t b) {
    return a.red == b.red && a.green == b.green && a.blue == b.blue;
}

static drv_ws2816c_pixel_t state_pixel(uint32_t now) {
    uint32_t period = led_state == LED_STATE_RUNNING ? BREATH_FAST_MS : BREATH_MS;
    return scaled_pixel(state_colors[led_state], breath_coefficient(now, period));
}

void status_led_init(void) {
    led_state = LED_STATE_IDLE;
    display_valid = false;
    display_pixel = (drv_ws2816c_pixel_t){0};
    next_frame_ms = 0u;
    retry_deadline_ms = 0u;
    retry_armed = false;
}

static void invalidate(void) {
    display_valid = false;
    next_frame_ms = 0u;
}

void status_led_set_connected(bool connected) {
    led_state = connected ? LED_STATE_CONNECTED : LED_STATE_IDLE;
    invalidate();
}

void status_led_set_running(bool running) {
    // running 清零表示目标暂停，此时主机仍在连接，因此退回 CONNECTED 而不是 IDLE
    led_state = running ? LED_STATE_RUNNING : LED_STATE_CONNECTED;
    invalidate();
}

void status_led_process(void) {
    uint32_t now = bsp_time_ms();

    // 帧间隔和 DMA 回收都在毫秒量级，先做一次最便宜的时间判断再碰外设
    // 一帧必定结束，逐轮轮询 DMA 与 SPI 寄存器只会拖慢主循环
    if (display_valid && !time_reached(now, next_frame_ms)) {
        return;
    }

    drv_ws2816c_process();

    if (!drv_ws2816c_ready()) {
        // 硬件失败后拉低 DIN 是安全行为，但必须退避重试，否则一次失败后不再恢复
        if (!retry_armed) {
            retry_armed = true;
            retry_deadline_ms = now + RETRY_MS;
        } else if (time_reached(now, retry_deadline_ms)) {
            retry_armed = false;
            drv_ws2816c_init();
            invalidate();
        }
        return;
    }
    retry_armed = false;

    drv_ws2816c_pixel_t pixel = state_pixel(now);

    // 呼吸在谷值和峰值附近变化很慢，去重可省掉这部分重复帧
    if (display_valid && pixel_equal(pixel, display_pixel)) {
        return;
    }
    if (!drv_ws2816c_write_async(&pixel, 1u)) {
        return;
    }
    display_pixel = pixel;
    display_valid = true;
    next_frame_ms = now + FRAME_INTERVAL_MS;
}
