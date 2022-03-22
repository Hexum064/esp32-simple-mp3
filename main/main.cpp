
// #include <freertos/FreeRTOS.h>
// #include <freertos/task.h>
#include <config.h>
#include <I2SOutput.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <dirent.h>

#include "esp_log.h"
// #include "esp_spiffs.h"
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_STDIO
#include "minimp3.h"

#define MOUNT_POINT "/sdcard"
#define PIN_NUM_MISO GPIO_NUM_12
#define PIN_NUM_MOSI GPIO_NUM_13
#define PIN_NUM_CLK  GPIO_NUM_14
#define PIN_NUM_CS   GPIO_NUM_15
#define SPI_DMA_CHAN    1

sdmmc_host_t host = SDSPI_HOST_DEFAULT();
sdmmc_card_t *card;
const char mount_point[] = MOUNT_POINT;

extern "C"
{
  void app_main();
}

void wait_for_button_push()
{
  while (gpio_get_level(GPIO_BUTTON) == 1)
  {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

const int BUFFER_SIZE = 1024;

bool mount_sd_card(void)
{

   esp_err_t ret;

    // Options for mounting the filesystem.
    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case when mounting fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
// #ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
//         .format_if_mount_failed = true,
// #else
        .format_if_mount_failed = false,
// #endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    
    
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
    if (ret != ESP_OK) {
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

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE("main", "Failed to mount filesystem. "
                     "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE("main", "Failed to initialize the card (%s). "
                     "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return false;
    }
    ESP_LOGI("main", "Filesystem mounted");

    // Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);

    return true;
}

void list_files(void)
{
        struct dirent *de;  // Pointer for directory entry
  
    // opendir() returns a pointer of DIR type. 
    DIR *dr = opendir(MOUNT_POINT);
  
    if (dr == NULL)  // opendir returns NULL if couldn't open directory
    {
        printf("Could not open current directory" );
        return;
    }
  
    // Refer http://pubs.opengroup.org/onlinepubs/7990989775/xsh/readdir.html
    // for readdir()
        while ((de = readdir(dr)) != NULL)
            printf("%s\n", de->d_name);
  
    closedir(dr);    
    return;
}


void play_task(void *param)
{

  list_files();
// #ifdef VOLUME_CONTROL
//   // set up the ADC for reading the volume control
//   adc1_config_width(ADC_WIDTH_12Bit);
//   adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_DB_11);
// #endif
  // create the output - see config.h for settings
// #ifdef USE_I2S
  Output *output = new I2SOutput(I2S_NUM_0, i2s_speaker_pins);
// #else
//   // Output *output = new DACOutput();
// #endif
#ifdef I2S_SPEAKDER_SD_PIN
  // if you I2S amp has a SD pin, you'll need to turn it on
  gpio_set_direction(I2S_SPEAKDER_SD_PIN, GPIO_MODE_OUTPUT);
  gpio_set_level(I2S_SPEAKDER_SD_PIN, 1);
#endif
  // setup the button to trigger playback - see config.h for settings
  gpio_set_direction(GPIO_BUTTON, GPIO_MODE_INPUT);
  gpio_set_pull_mode(GPIO_BUTTON, GPIO_PULLUP_ONLY);
  // create the file system
  ///////////SPIFFS spiffs("/fs");

  //   esp_vfs_spiffs_conf_t conf = {
  //     .base_path = "/spiffs",
  //     .partition_label = NULL,
  //     .max_files = 5,
  //     .format_if_mount_failed = false
  //   };

  //   // Use settings defined above to initialize and mount SPIFFS filesystem.
  //   // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
  //   esp_err_t ret = esp_vfs_spiffs_register(&conf);

  //   if (ret != ESP_OK) {
  //       if (ret == ESP_FAIL) {
  //           ESP_LOGE("main", "Failed to mount or format filesystem");
  //       } else if (ret == ESP_ERR_NOT_FOUND) {
  //           ESP_LOGE("main", "Failed to find SPIFFS partition");
  //       } else {
  //           ESP_LOGE("main", "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
  //       }
  //        while (true);
  //       //return;
  //   }

  // ESP_LOGI("main", "SPIFFS Mounted");



  // setup for the mp3 decoded
  short *pcm = (short *)malloc(sizeof(short) * MINIMP3_MAX_SAMPLES_PER_FRAME);
  uint8_t *input_buf = (uint8_t *)malloc(BUFFER_SIZE);
  if (!pcm)
  {
    ESP_LOGE("main", "Failed to allocate pcm memory");
  }
  if (!input_buf)
  {
    ESP_LOGE("main", "Failed to allocate input_buf memory");
  }

  ESP_LOGI("main", "memory allocated");
  
  while (true)
  {
    ESP_LOGI("main", "waiting for button");
    // wait for the button to be pushed
    wait_for_button_push();
    // mp3 decoder state
    mp3dec_t mp3d = {};
    mp3dec_init(&mp3d);
    mp3dec_frame_info_t info = {};
    // keep track of how much data we have buffered, need to read and decoded
    int to_read = BUFFER_SIZE;
    int buffered = 0;
    int decoded = 0;
    int bytes_read = 0;
    int file_sz = 0;
    bool is_output_started = false;
    const char *file_name = MOUNT_POINT"/_MP3DO~2.MP3";
    // this assumes that you have uploaded the mp3 file to the SPIFFS
    ESP_LOGI("main", "Opening file %s", file_name);
    FILE *fp = fopen(file_name, "r");
    if (!fp)
    {
        ESP_LOGE("main", "Failed to open file");
      continue;
    }

    fseek(fp, 0L, SEEK_END);
    file_sz = ftell(fp);
    rewind(fp);

    while (1)
    {
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
      //ESP_LOGI("main", "decoded frame\n");
      // we've processed this may bytes from teh buffered data
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
        //ESP_LOGI("main", "output written\n");
        // keep track of how many samples we've decoded
        decoded += samples;
      }
      // ESP_LOGI("main", "decoded %d samples\n", decoded);
      }
    ESP_LOGI("main", "Finished\n");
    fclose(fp);
  }

      // All done, unmount partition and disable SPI peripheral
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    ESP_LOGI("main", "Card unmounted");

    //deinitialize the bus after all devices are removed
    spi_bus_free(static_cast<spi_host_device_t>(host.slot));
}

void app_main()
{
  if (mount_sd_card())
  {
    xTaskCreatePinnedToCore(play_task, "task", 32768, NULL, 1, NULL, 1);
  }
  

}