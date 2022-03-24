#include <freertos/FreeRTOS.h>
#include <driver/i2s.h>


// comment this out if you want to use the internal DAC
#define USE_I2S

// speaker settings - if using I2S
#define I2S_SPEAKER_SERIAL_CLOCK GPIO_NUM_4
#define I2S_SPEAKER_LEFT_RIGHT_CLOCK GPIO_NUM_5
#define I2S_SPEAKER_SERIAL_DATA GPIO_NUM_18
//#define I2S_SPEAKDER_SD_PIN GPIO_NUM_5

// volume control - if required
//#define VOLUME_CONTROL ADC1_CHANNEL_7

// button - GPIO 0 is the built in button on most dev boards
#define PLAY_BUTTON GPIO_NUM_2
#define NEXT_BUTTON GPIO_NUM_16
#define BACK_BUTTON GPIO_NUM_17

#define MOUNT_POINT "/sdcard"
#define PIN_NUM_MISO GPIO_NUM_12
#define PIN_NUM_MOSI GPIO_NUM_13
#define PIN_NUM_CLK  GPIO_NUM_14
#define PIN_NUM_CS   GPIO_NUM_15
#define SPI_DMA_CHAN    1


#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO

#define ESP_INTR_FLAG_DEFAULT 0

// i2s speaker pins definition
extern i2s_pin_config_t i2s_speaker_pins;
