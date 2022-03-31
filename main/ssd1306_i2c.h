#include "ssd1306.h"

void i2c_master_init(SSD1306_t * dev, gpio_num_t sda, gpio_num_t scl, gpio_num_t reset);
void i2c_init(SSD1306_t * dev, int width, int height);
void i2c_display_image(SSD1306_t * dev, int page, int seg, uint8_t * images, int width);
void i2c_contrast(SSD1306_t * dev, int contrast);
void i2c_hardware_scroll(SSD1306_t * dev, ssd1306_scroll_type_t scroll);