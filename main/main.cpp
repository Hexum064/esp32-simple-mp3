

#include <config.h>
#include <I2SOutput.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <dirent.h>
#include "filenavi.h"
#include "esp_log.h"

#include "minimp3.h"

// SD Card
sdmmc_host_t host = SDSPI_HOST_DEFAULT();
sdmmc_card_t *card;
const char mount_point[] = MOUNT_POINT;
// END

// mp3 decoder state
mp3dec_t mp3d = {};
Output *output = NULL;
int64_t mp3_start_time;
int64_t mp3_run_time;

int64_t display_update_start_time;

// esp_timer_handle_t periodic_timer_handle;
// static xQueueHandle gpio_evt_queue = NULL;
// static void periodic_timer_callback(void *arg);

bool pause_toggle = true;
bool play_pressed = false;
bool next_pressed = false;
bool back_pressed = false;

int64_t play_button_debounce_start_time = 0;
int64_t next_button_debounce_start_time = 0;
int64_t back_button_debounce_start_time = 0;

extern "C"
{
	void app_main();
}

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
	uint32_t gpio_num = (uint32_t)arg;

	xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

// static void periodic_timer_callback(void *arg)
// {
// 	int64_t time_since_boot = esp_timer_get_time();
// 	ESP_LOGI("main", "Periodic timer called, time since boot: %lld us", time_since_boot);
// }

static void gpio_task_example(void *arg)
{
	uint32_t io_num;
	for (;;)
	{

		if (xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY))
		{
			int level = gpio_get_level((gpio_num_t)io_num);
			// printf("GPIO[%d] intr, val: %d\n", io_num, level);

			switch (io_num)
			{
			case PLAY_BUTTON:

				if (level == 0)
				{
					play_button_debounce_start_time = esp_timer_get_time();
				}

				if (level == 1 && (esp_timer_get_time() - play_button_debounce_start_time) > DEBOUNCE_US)
				{
					play_button_debounce_start_time = esp_timer_get_time();
					pause_toggle = !pause_toggle;
				}

				break;
			case NEXT_BUTTON:

				if (level == 0)
				{
					// printf("Button pressed. Start timing\n");
					next_button_debounce_start_time = esp_timer_get_time();
					next_pressed = false;
				}

				if (level == 1 && (esp_timer_get_time() - next_button_debounce_start_time) > DEBOUNCE_US)
				{
					// printf("Button pressed. End timing\n");
					next_button_debounce_start_time = esp_timer_get_time();
					next_pressed = true;
				}

				break;
			case BACK_BUTTON:

				if (level == 0)
				{
					back_button_debounce_start_time = esp_timer_get_time();
					back_pressed = false;
				}

				if (level == 1 && (esp_timer_get_time() - back_button_debounce_start_time) > DEBOUNCE_US)
				{
					back_button_debounce_start_time = esp_timer_get_time();
					back_pressed = true;
				}

				break;
			default:
				break;
			}
		}
	}
}

// void init_timer()
// {
// 	const esp_timer_create_args_t periodic_timer_args = {
// 		.callback = &periodic_timer_callback,
// 		/* name is optional, but may help identify the timer when debugging */
// 		.name = "periodic"};

// 	ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer_handle));
// 	/* The timer has been created but is not running yet */
// }

void init_inputs()
{
	gpio_config_t io_conf = {
		.pin_bit_mask = ((1ULL << PLAY_BUTTON) | (1ULL << NEXT_BUTTON) | (1ULL << BACK_BUTTON)),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_ANYEDGE,
	};

	gpio_config(&io_conf);

	gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
	// start gpio task
	xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);

	// install gpio isr service
	gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);

	// hook isr handler for specific gpio pins
	gpio_isr_handler_add(PLAY_BUTTON, gpio_isr_handler, (void *)PLAY_BUTTON);
	gpio_isr_handler_add(NEXT_BUTTON, gpio_isr_handler, (void *)NEXT_BUTTON);
	gpio_isr_handler_add(BACK_BUTTON, gpio_isr_handler, (void *)BACK_BUTTON);
}

// void wait_for_play_button_push()
// {
// 	while (gpio_get_level(PLAY_BUTTON) == 1)
// 	{
// 		vTaskDelay(pdMS_TO_TICKS(100));
// 	}
// }

void wait_for_pause()
{
	while (pause_toggle)
	{
		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

bool mount_sd_card(void)
{

	esp_err_t ret;

	// Options for mounting the filesystem.
	// If format_if_mount_failed is set to true, SD card will be partitioned and
	// formatted in case when mounting fails.
	esp_vfs_fat_sdmmc_mount_config_t mount_config = {
		.format_if_mount_failed = false,
		.max_files = 5,
		.allocation_unit_size = 16 * 1024};

	ESP_LOGI("main", "Initializing SD card");

	// Use settings defined above to initialize SD card and mount FAT filesystem.
	// Note: esp_vfs_fat_sdmmc/sdspi_mount is all-in-one convenience functions.
	// Please check its source code and implement error recovery when developing
	// production applications.
	ESP_LOGI("main", "Using SPI peripheral");

	spi_bus_config_t bus_cfg = {
		.mosi_io_num = PIN_NUM_MOSI,
		.miso_io_num = PIN_NUM_MISO,
		.sclk_io_num = PIN_NUM_CLK,
		.quadwp_io_num = -1,
		.quadhd_io_num = -1,
		.max_transfer_sz = 4000,
	};
	ret = spi_bus_initialize(static_cast<spi_host_device_t>(host.slot), &bus_cfg, SPI_DMA_CHAN);
	if (ret != ESP_OK)
	{
		ESP_LOGE("main", "Failed to initialize bus.");
		return false;
	}

	// This initializes the slot without card detect (CD) and write protect (WP) signals.
	// Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
	sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
	slot_config.gpio_cs = PIN_NUM_CS;
	slot_config.host_id = static_cast<spi_host_device_t>(host.slot);

	ESP_LOGI("main", "Mounting filesystem");
	ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

	if (ret != ESP_OK)
	{
		if (ret == ESP_FAIL)
		{
			ESP_LOGE("main", "Failed to mount filesystem. "
							 "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
		}
		else
		{
			ESP_LOGE("main", "Failed to initialize the card (%s). "
							 "Make sure SD card lines have pull-up resistors in place.",
					 esp_err_to_name(ret));
		}
		return false;
	}
	ESP_LOGI("main", "Filesystem mounted");

	// Card has been initialized, print its properties
	sdmmc_card_print_info(stdout, card);

	return true;
}

void init_audio_out()
{
	output = new I2SOutput(I2S_NUM_0, i2s_speaker_pins);
	// setup the button to trigger playback - see config.h for settings
	// gpio_set_direction(START_BUTTON, GPIO_MODE_INPUT);
	// gpio_set_pull_mode(START_BUTTON, GPIO_PULLUP_ONLY);
}

void update_display()
{
	int totalSeconds = mp3_run_time / 1000000;
	int minutes = totalSeconds / 60;
	int seconds = totalSeconds % 60;

	printf("run time: %d:%02d\n", minutes, seconds);
}

void play_mp3(FILE *fp)
{

	ESP_LOGI("main", "Starting mp3. back_pressed=%d, next_pressed=%d\n", back_pressed, next_pressed);

	init_audio_out();
	short *pcm = (short *)malloc(sizeof(short) * MINIMP3_MAX_SAMPLES_PER_FRAME);
	uint8_t *input_buf = (uint8_t *)malloc(BUFFER_SIZE);
	if (!pcm)
	{
		ESP_LOGE("main", "Failed to allocate pcm memory");
		return;
	}
	if (!input_buf)
	{
		ESP_LOGE("main", "Failed to allocate input_buf memory");
		return;
	}
	ESP_LOGI("main", "memory allocated");
	// mp3 decoder state
	// mp3dec_t mp3d = {};
	// mp3dec_init(&mp3d);
	mp3dec_frame_info_t info = {};
	// keep track of how much data we have buffered, need to read and decoded
	int to_read = BUFFER_SIZE;
	int buffered = 0;
	int decoded = 0;
	int bytes_read = 0;
	bool is_output_started = false;
	bool pause_state = pause_toggle;

	mp3_start_time = esp_timer_get_time();
	mp3_run_time = 0;

	display_update_start_time = esp_timer_get_time();

	while (1)
	{

		if (esp_timer_get_time() - display_update_start_time > DISPLAY_UPDATE_INTERVAL_US)
		{
			update_display();
			display_update_start_time = esp_timer_get_time();
		}

		if (next_pressed || back_pressed)
		{
			ESP_LOGI("main", "breaking. back_pressed=%d, next_pressed=%d\n", back_pressed, next_pressed);

			break;
		}

		if (pause_state != pause_toggle)
		{
			pause_toggle ? printf("Paused\n") : printf("Playing\n");
			pause_state = pause_toggle;
		}

		if (!pause_toggle)
		{
			mp3_run_time = esp_timer_get_time() - mp3_start_time;

#ifdef VOLUME_CONTROL
			auto adc_value = float(adc1_get_raw(VOLUME_CONTROL)) / 4096.0f;
			// make the actual volume match how people hear
			// https://ux.stackexchange.com/questions/79672/why-dont-commercial-products-use-logarithmic-volume-controls
			output->set_volume(adc_value * adc_value);
#endif

			// read in the data that is needed to top up the buffer
			size_t n = fread(input_buf + buffered, 1, to_read, fp);
			// feed the watchdog
			vTaskDelay(pdMS_TO_TICKS(1));

			buffered += n;
			bytes_read += n;
			// ESP_LOGI("main", "Read %d bytes, total %d, size %d. buffered=%d", n, bytes_read, file_sz, buffered);
			if (buffered == 0)
			{
				// we've reached the end of the file and processed all the buffered data
				output->stop();
				is_output_started = false;
				break;
			}
			// decode the next frame
			int samples = mp3dec_decode_frame(&mp3d, input_buf, buffered, pcm, &info);
			// ESP_LOGI("main", "decoded frame\n");
			//  we've processed this may bytes from teh buffered data
			buffered -= info.frame_bytes;
			// ESP_LOGI("main", "buffered after decode: %d (frame_bytes=%d)\n", buffered, info.frame_bytes);
			// shift the remaining data to the front of the buffer
			memmove(input_buf, input_buf + info.frame_bytes, buffered);
			// we need to top up the buffer from the file
			to_read = info.frame_bytes;
			if (samples > 0)
			{
				// if we haven't started the output yet we can do it now as we now know the sample rate and number of channels
				if (!is_output_started)
				{
					ESP_LOGI("main", "starting output\n");
					output->start(info.hz);
					is_output_started = true;
					ESP_LOGI("main", "output started\n");
				}
				// if we've decoded a frame of mono samples convert it to stereo by duplicating the left channel
				// we can do this in place as our samples buffer has enough space
				if (info.channels == 1)
				{
					for (int i = samples - 1; i >= 0; i--)
					{
						pcm[i * 2] = pcm[i];
						pcm[i * 2 - 1] = pcm[i];
					}
				}
				// write the decoded samples to the I2S output
				output->write(pcm, samples);
				// ESP_LOGI("main", "output written\n");
				//  keep track of how many samples we've decoded
				decoded += samples;
			}
			// ESP_LOGI("main", "decoded %d samples\n", decoded);
		}
		else
		{
			// move the start time forward so the run_time doesn't change during pause
			mp3_start_time = esp_timer_get_time() - mp3_run_time;
			vTaskDelay(pdMS_TO_TICKS(100));
		}
	}

	if (pcm != NULL)
		free(pcm);
	if (input_buf != NULL)
		free(input_buf);
}

void play_task(void *param)
{

	// char *path;

	// setup for the mp3 decoded
	mp3dec_init(&mp3d);

	ESP_LOGI("main", "waiting for first play button push");
	// wait for the button to be pushed
	wait_for_play_button_push();
	ESP_LOGI("main", "locating first mp3");
	get_first_mp3();

	while (get_current_file() != NULL)
	{

		char file_name[1024];
		// path  = get_current_path(); //;MOUNT_POINT"/_MP3DO~2.MP3";
		strcpy(file_name, get_current_path());
		strcat(file_name, "/");
		strcat(file_name, get_current_file()->d_name);
		// this assumes that you have uploaded the mp3 file to the SPIFFS
		ESP_LOGI("main", "Opening file %s", file_name);
		FILE *fp = fopen(file_name, "r");
		if (!fp)
		{
			ESP_LOGE("main", "Failed to open file");
			continue;
		}

		// fseek(fp, 0L, SEEK_END);
		// file_sz = ftell(fp);
		// rewind(fp);

		play_mp3(fp);
		fclose(fp);
		ESP_LOGI("main", "Finished\n");
		if (back_pressed)
		{
			ESP_LOGI("main", "Navigate back\n");
			back_pressed = false;

			// only navigate backwards if the current song has only been playing for a short time
			if (mp3_run_time < RESTART_CURRENT_US)
				get_prev_mp3();
		}
		else // next is default op
		{
			ESP_LOGI("main", "Navigate next\n");
			get_next_mp3();
			next_pressed = false;
		}
	}

	// All done, unmount partition and disable SPI peripheral
	esp_vfs_fat_sdcard_unmount(mount_point, card);
	ESP_LOGI("main", "Card unmounted");

	// deinitialize the bus after all devices are removed
	spi_bus_free(static_cast<spi_host_device_t>(host.slot));
}

void app_main()
{
	init_inputs();
	// init_timer();

	// ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer_handle, 1000));

	if (mount_sd_card())
	{
		xTaskCreatePinnedToCore(play_task, "task", 32768, NULL, 1, NULL, 1);
	}
}