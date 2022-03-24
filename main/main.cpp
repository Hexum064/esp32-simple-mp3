

#include <config.h>
#include <I2SOutput.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include <dirent.h>

#include "esp_log.h"


#include "minimp3.h"



sdmmc_host_t host = SDSPI_HOST_DEFAULT();
sdmmc_card_t *card;
const char mount_point[] = MOUNT_POINT;
DIR *dr = NULL;
struct dirent *de;  // Pointer for directory entry
long int cur_pos = 0;
long int target_pos = 0;
Output *output = NULL;
char path[1024];
uint32_t io_num;
static xQueueHandle gpio_evt_queue = NULL;

extern "C"
{
  void app_main();
}

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    io_num = gpio_num;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

static void gpio_task_example(void* arg)
{
    uint32_t io_num;
    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            printf("GPIO[%d] intr, val: %d\n", io_num, gpio_get_level((gpio_num_t)io_num));
        }
    }
}

void init_inputs()
{
  gpio_config_t io_conf = {
    .pin_bit_mask = ((1ULL<<PLAY_BUTTON) | (1ULL<<NEXT_BUTTON) | (1ULL<<BACK_BUTTON)),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_NEGEDGE,
  };

  gpio_config(&io_conf);

  gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
  //start gpio task
  xTaskCreate(gpio_task_example, "gpio_task_example", 2048, NULL, 10, NULL);

    //install gpio isr service
  gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
  
  //hook isr handler for specific gpio pins    
  gpio_isr_handler_add(PLAY_BUTTON, gpio_isr_handler, (void*) PLAY_BUTTON);    
  gpio_isr_handler_add(NEXT_BUTTON, gpio_isr_handler, (void*) NEXT_BUTTON);
  gpio_isr_handler_add(BACK_BUTTON, gpio_isr_handler, (void*) BACK_BUTTON);

}

void revert_path()
{
    int i = 0;
    int lastPos = -1;

    for (; i<1024 || path[i] == NULL; i++)
    {
        //the next i will always be greater than whatever is stored in lastPos
        if (path[i] == '/')
        {
            lastPos = i;
        }            
    }

    if (lastPos < 0 || path[lastPos] == NULL)
        return;

    path[lastPos] = NULL;

}

void wait_for_play_button_push()
{
  while (gpio_get_level(PLAY_BUTTON) == 1)
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
        .format_if_mount_failed = false,
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

void open_dir(char* name)
{
    
    if (dr != NULL)
        closedir(dr);    

    dr = opendir(name);
}

void navigate_to_target_pos_from_curr_dir() 
{
    long int cur_dir = 0;

    if (target_pos < 1)
        return;

    //first look through the files then recurse through the dirs
    while ((de = readdir(dr)) != NULL)
    {
        if (de->d_type == DT_REG)
        {
            cur_pos++;
#ifdef DEBUG
            printf("Looing at file %s in %s. Pos: %ld, Target: %ld\n", de->d_name, path, cur_pos, target_pos);
#endif        
            if (cur_pos == target_pos)
            {
#ifdef DEBUG                
                printf("File found at pos: %ld\n", cur_pos);
#endif        
                return;
            }
            
#ifdef DEBUG 
            printf("Inc cur_pos to %ld\n", cur_pos);
#endif
        }
    }

    rewinddir(dr);

    //DIRs now
    while ((de = readdir(dr)) != NULL)
    {
        if (de->d_type == DT_DIR)
        {
            cur_dir = telldir(dr);

            strcat(path, "/");
            strcat(path, de->d_name);
            
            open_dir(path);     
#ifdef DEBUG            
            printf("Navigating to path %s\n", path);
#endif            
            navigate_to_target_pos_from_curr_dir();

            if (cur_pos == target_pos)
            {
#ifdef DEBUG                
                printf("File found at pos: %ld\n", cur_pos);
#endif                
                return;
            }

            revert_path();
#ifdef DEBUG            
            printf("Returning to %s\n", path);
#endif            
            open_dir(path);   
            seekdir(dr, cur_dir + 1);
        }
    }  
#ifdef DEBUG
    printf("No more files or dirs in %s\n", path);
#endif    
}

void navigate_to_pos() 
{
    de = NULL;
    strcpy(path, MOUNT_POINT);
    open_dir(path); 
    cur_pos = 0;
    navigate_to_target_pos_from_curr_dir();
}


void get_next_file()
{
    target_pos++;
    navigate_to_pos();
}


void get_prev_file()
{
    if (target_pos > 0)
    {
        target_pos--;
        navigate_to_pos();
    }
}

bool is_mp3(char * name)
{
  int size = strlen(name);

  if (size < 4) //should at least be '.mp3'
    return false;
  
  size--; //for 0 offset

  //returns true if the file ends in '.mp3', case-insensitive 
  return (name[size] == '3'
    && (name[size - 1] == 'p' || name[size - 1] == 'P')
    && (name[size - 2] == 'm' || name[size - 2] == 'M')
    && name[size - 3] == '.');

}

void get_next_mp3()
{
  long int curr = target_pos;
  
  do
  {
    get_next_file();
  } while (de != NULL && !is_mp3(de->d_name));
  
  if (de == NULL)  
  {
    //try to go back, we probably ran out of files
    target_pos = curr;
    navigate_to_pos();
    return;
  }
}

void get_prev_mp3()
{
  long int curr = target_pos;
  
  do
  {
    get_prev_file();
  } while (de != NULL && !is_mp3(de->d_name));
  
  if (de == NULL)  
  {
    //try to go back, we probably ran out of files
    target_pos = curr;
    navigate_to_pos();
    return;
  }
}

void init_audio_out()
{
    output = new I2SOutput(I2S_NUM_0, i2s_speaker_pins);
    // setup the button to trigger playback - see config.h for settings
    // gpio_set_direction(START_BUTTON, GPIO_MODE_INPUT);
    // gpio_set_pull_mode(START_BUTTON, GPIO_PULLUP_ONLY);
}

void play_mp3(FILE *fp)
{

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

  if (pcm != NULL)
    free(pcm);
  if (input_buf != NULL)
    free(input_buf);
}

void play_task(void *param)
{

  init_audio_out();

  // setup for the mp3 decoded


  ESP_LOGI("main", "memory allocated");
  
  ESP_LOGI("main", "waiting for first play button push");
  // wait for the button to be pushed
  wait_for_play_button_push();

  while (true)
  {



    char *file_name = path; //;MOUNT_POINT"/_MP3DO~2.MP3";

///TESTING////
    get_next_file();
    get_next_file();
    strcat(file_name, "/");
    strcat(file_name, de->d_name);
    // this assumes that you have uploaded the mp3 file to the SPIFFS
    ESP_LOGI("main", "Opening file %s", file_name);
    FILE *fp = fopen(file_name, "r");
    if (!fp)
    {
      ESP_LOGE("main", "Failed to open file");
      continue;
    }

    fseek(fp, 0L, SEEK_END);
    //file_sz = ftell(fp);
    rewind(fp);

    play_mp3(fp);

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
  init_inputs();
  if (mount_sd_card())
  {
    xTaskCreatePinnedToCore(play_task, "task", 32768, NULL, 1, NULL, 1);
  }
  

}