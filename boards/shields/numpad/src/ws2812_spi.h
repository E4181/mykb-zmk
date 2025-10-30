#ifndef WS2812_SPI_H
#define WS2812_SPI_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>

#ifdef __cplusplus
extern "C" {
#endif

// 配置参数
#define WS2812_MAX_LEDS         10      // 10个LED
#define WS2812_SPI_FREQ         8000000 // 8MHz SPI时钟
#define WS2812_BREATH_INTERVAL  20      // 呼吸效果更新间隔(ms)

// 初始化函数
int ws2812_spi_init(void);
int ws2812_deinit(void);

// 基础LED控制
int ws2812_set_led(uint16_t index, uint8_t r, uint8_t g, uint8_t b);
int ws2812_set_all_leds(uint8_t r, uint8_t g, uint8_t b);
int ws2812_clear_leds(void);
int ws2812_update(void);

// 呼吸灯效果
int ws2812_start_breathing_effect(uint8_t r, uint8_t g, uint8_t b);
int ws2812_stop_breathing_effect(void);

// 自动初始化（上电即显示呼吸灯）
void ws2812_auto_init(void);

#ifdef __cplusplus
}
#endif

#endif // WS2812_SPI_H