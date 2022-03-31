#include "ssd1306.h"

void spi_master_init(SSD1306_t * dev, gpio_num_t GPIO_MOSI, gpio_num_t GPIO_SCLK, gpio_num_t GPIO_CS, gpio_num_t GPIO_DC, gpio_num_t GPIO_RESET);
bool spi_master_write_byte(spi_device_handle_t SPIHandle, const uint8_t* Data, size_t DataLength );
bool spi_master_write_command(SSD1306_t * dev, uint8_t Command );
bool spi_master_write_data(SSD1306_t * dev, const uint8_t* Data, size_t DataLength );
void spi_init(SSD1306_t * dev, int width, int height);
void spi_display_image(SSD1306_t * dev, int page, int seg, uint8_t * images, int width);
void spi_contrast(SSD1306_t * dev, int contrast);
void spi_hardware_scroll(SSD1306_t * dev, ssd1306_scroll_type_t scroll);