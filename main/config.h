#include <freertos/FreeRTOS.h>
#include "freertos/timers.h"
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
#define DEBOUNCE_US 100000

#define MOUNT_POINT "/sdcard"
#define PIN_NUM_MISO GPIO_NUM_12
#define PIN_NUM_MOSI GPIO_NUM_13
#define PIN_NUM_CLK  GPIO_NUM_14
#define PIN_NUM_CS   GPIO_NUM_15
#define SPI_DMA_CHAN    1

#define DISPLAY_UPDATE_INTERVAL_US 500000

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#define MP3_BUFFER_SIZE 1024
#define ESP_INTR_FLAG_DEFAULT 0
#define RESTART_CURRENT_US 2000000
// i2s speaker pins definition
extern i2s_pin_config_t i2s_speaker_pins;


#define BT_AV_TAG				"BT_AV"
#define BT_RC_CT_TAG			"RCCT"

// number of retries when connecting
#define BT_CONNECT_RETRY 2

// AVRCP used transaction label
#define APP_RC_CT_TL_GET_CAPS			 (0)
#define APP_RC_CT_TL_RN_VOLUME_CHANGE	 (1)

/* event for handler "bt_av_hdl_stack_up */
enum {
	BT_APP_EVT_STACK_UP = 0,
};

/* A2DP global state */
enum {
	APP_AV_STATE_IDLE,
	APP_AV_STATE_DISCOVERING,
	APP_AV_STATE_DISCOVERED,
	APP_AV_STATE_UNCONNECTED,
	APP_AV_STATE_CONNECTING,
	APP_AV_STATE_CONNECTED,
	APP_AV_STATE_DISCONNECTING,
};

/* sub states of APP_AV_STATE_CONNECTED */
enum {
	APP_AV_MEDIA_STATE_IDLE,
	APP_AV_MEDIA_STATE_STARTING,
	APP_AV_MEDIA_STATE_STARTED,
	APP_AV_MEDIA_STATE_STOPPING,
};

#define BT_APP_HEART_BEAT_EVT				 (0xff00)