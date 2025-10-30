#include "ws2812_spi.h"
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <string.h>
#include <math.h>

// 编码模式定义
#define WS2812_BIT_0  0xC0  // 0b11000000
#define WS2812_BIT_1  0xFC  // 0b11111100
#define WS2812_RESET_BYTES 64

// 全局变量
static const struct device *spi_dev;
static struct spi_config spi_cfg;
static uint8_t led_colors[WS2812_MAX_LEDS * 3];
static uint8_t spi_buffer[(WS2812_MAX_LEDS * 24) + WS2812_RESET_BYTES];

// 呼吸灯控制
static struct k_timer breath_timer;
static bool breathing_active = false;
static uint8_t breath_color_r = 50;  // 较暗的白色，避免太刺眼
static uint8_t breath_color_g = 50;
static uint8_t breath_color_b = 50;
static float breath_phase = 0.0f;

// 私有函数声明
static uint8_t encode_ws2812_bit(bool bit_val);
static void ws2812_encode_rgb(uint8_t *spi_buf, uint8_t r, uint8_t g, uint8_t b);
static void configure_spi1_p0_31(void);
static void breath_timer_handler(struct k_timer *timer);

// 初始化WS2812驱动
int ws2812_spi_init(void) {
    // 获取SPI1设备
    spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi1));
    if (!device_is_ready(spi_dev)) {
        printk("SPI1 device not ready\n");
        return -ENODEV;
    }
    
    // 配置SPI1使用P0.31作为MOSI
    configure_spi1_p0_31();
    
    // 配置SPI参数
    spi_cfg.operation = SPI_WORD_SET(8) |      // 8位数据
                       SPI_OP_MODE_MASTER |   // 主模式
                       SPI_MODE_CPOL |       // 时钟极性
                       SPI_MODE_CPHA |       // 时钟相位
                       SPI_TRANSFER_MSB;      // MSB优先
    
    spi_cfg.frequency = WS2812_SPI_FREQ;
    spi_cfg.slave = 0;
    
    // 清空LED数据
    memset(led_colors, 0, sizeof(led_colors));
    memset(spi_buffer, 0, sizeof(spi_buffer));
    
    // 初始化呼吸灯定时器
    k_timer_init(&breath_timer, breath_timer_handler, NULL);
    
    printk("WS2812 SPI1 initialized with P0.31 as MOSI, %d LEDs\n", WS2812_MAX_LEDS);
    return 0;
}

int ws2812_deinit(void) {
    // 停止呼吸效果
    ws2812_stop_breathing_effect();
    
    // 关闭所有LED
    ws2812_clear_leds();
    ws2812_update();
    
    return 0;
}

// 配置SPI1使用P0.31作为MOSI引脚
static void configure_spi1_p0_31(void) {
#if defined(CONFIG_SOC_SERIES_NRF52X)
    // 直接配置nRF52 SPI1外设寄存器，设置MOSI为P0.31
    NRF_SPI_Type *spi = (NRF_SPI_Type *)NRF_SPI1;
    
    // 禁用SPI
    spi->ENABLE = 0;
    
    // 配置MOSI引脚为P0.31
    spi->PSEL.MOSI = 31;
    
    // 将MISO和SCK设置为未连接（0xFFFFFFFF）
    spi->PSEL.MISO = 0xFFFFFFFF;
    spi->PSEL.SCK = 0xFFFFFFFF;
    
    // 重新启用SPI
    spi->ENABLE = 1;
    
    printk("SPI1 configured: MOSI=P0.31\n");
#endif
}

// 编码单个WS2812位
static uint8_t encode_ws2812_bit(bool bit_val) {
    return bit_val ? WS2812_BIT_1 : WS2812_BIT_0;
}

// 编码RGB数据到SPI缓冲区
static void ws2812_encode_rgb(uint8_t *spi_buf, uint8_t r, uint8_t g, uint8_t b) {
    uint32_t grb_data = ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
    int spi_index = 0;
    
    // WS2812使用GRB顺序，从最高位开始编码
    for (int i = 23; i >= 0; i--) {
        bool bit_val = (grb_data >> i) & 0x01;
        spi_buf[spi_index++] = encode_ws2812_bit(bit_val);
    }
}

// 设置单个LED颜色
int ws2812_set_led(uint16_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index >= WS2812_MAX_LEDS) {
        return -EINVAL;
    }
    
    led_colors[index * 3] = r;
    led_colors[index * 3 + 1] = g;
    led_colors[index * 3 + 2] = b;
    
    return 0;
}

// 设置所有LED为相同颜色
int ws2812_set_all_leds(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < WS2812_MAX_LEDS; i++) {
        ws2812_set_led(i, r, g, b);
    }
    return 0;
}

// 清空所有LED
int ws2812_clear_leds(void) {
    return ws2812_set_all_leds(0, 0, 0);
}

// 更新LED显示
int ws2812_update(void) {
    if (spi_dev == NULL) {
        return -ENODEV;
    }

    // 编码所有LED数据到SPI缓冲区
    int spi_index = 0;
    for (int i = 0; i < WS2812_MAX_LEDS; i++) {
        uint8_t *color = &led_colors[i * 3];
        ws2812_encode_rgb(&spi_buffer[spi_index], color[0], color[1], color[2]);
        spi_index += 24;
    }
    
    // 添加复位码（低电平）
    memset(&spi_buffer[spi_index], 0x00, WS2812_RESET_BYTES);
    
    // 配置SPI传输
    struct spi_buf tx_buf = {
        .buf = spi_buffer,
        .len = spi_index + WS2812_RESET_BYTES
    };
    
    struct spi_buf_set tx_bufs = {
        .buffers = &tx_buf,
        .count = 1
    };
    
    // 执行SPI传输
    int ret = spi_write(spi_dev, &spi_cfg, &tx_bufs);
    if (ret != 0) {
        printk("WS2812 SPI write failed: %d\n", ret);
        return ret;
    }
    
    return 0;
}

// 呼吸灯定时器处理函数
static void breath_timer_handler(struct k_timer *timer) {
    if (!breathing_active) {
        return;
    }
    
    // 更新呼吸相位
    breath_phase += 0.05f;
    if (breath_phase > 2 * 3.14159f) {
        breath_phase = 0.0f;
    }
    
    // 计算呼吸亮度 (使用正弦波的半周期)
    float intensity = (sinf(breath_phase) + 1.0f) * 0.5f;
    
    // 应用呼吸效果到颜色
    uint8_t r = (uint8_t)(breath_color_r * intensity);
    uint8_t g = (uint8_t)(breath_color_g * intensity);
    uint8_t b = (uint8_t)(breath_color_b * intensity);
    
    // 设置并更新所有LED
    ws2812_set_all_leds(r, g, b);
    ws2812_update();
}

// 开始呼吸灯效果
int ws2812_start_breathing_effect(uint8_t r, uint8_t g, uint8_t b) {
    breath_color_r = r;
    breath_color_g = g;
    breath_color_b = b;
    breath_phase = 0.0f;
    breathing_active = true;
    
    // 启动定时器，周期性更新呼吸效果
    k_timer_start(&breath_timer, 
                  K_MSEC(WS2812_BREATH_INTERVAL), 
                  K_MSEC(WS2812_BREATH_INTERVAL));
    
    printk("WS2812 breathing effect started with color (%d, %d, %d)\n", r, g, b);
    return 0;
}

// 停止呼吸灯效果
int ws2812_stop_breathing_effect(void) {
    breathing_active = false;
    k_timer_stop(&breath_timer);
    
    // 关闭所有LED
    ws2812_clear_leds();
    ws2812_update();
    
    printk("WS2812 breathing effect stopped\n");
    return 0;
}

// 自动初始化函数 - 在系统启动时调用
static void ws2812_init_delayed(struct k_timer *timer) {
    ARG_UNUSED(timer);
    
    // 初始化WS2812驱动
    if (ws2812_spi_init() == 0) {
        // 启动呼吸灯效果（使用较暗的白色避免刺眼）
        ws2812_start_breathing_effect(breath_color_r, breath_color_g, breath_color_b);
        printk("WS2812 auto-initialized with breathing effect\n");
    } else {
        printk("WS2812 auto-initialization failed\n");
    }
}

// 延迟初始化定时器
static struct k_timer init_timer;

// 自动初始化入口点
static int ws2812_breathing_auto_init(const struct device *dev) {
    ARG_UNUSED(dev);
    
    // 初始化延迟定时器
    k_timer_init(&init_timer, ws2812_init_delayed, NULL);
    
    // 延迟1秒后初始化，确保系统完全启动
    k_timer_start(&init_timer, K_MSEC(1000), K_FOREVER);
    
    return 0;
}

// 注册自动初始化
SYS_INIT(ws2812_breathing_auto_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);