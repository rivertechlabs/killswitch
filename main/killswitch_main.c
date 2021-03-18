/* 
KILLSWITCH is a program that keeps time on a DS3231 and uses the alarm
line of the DS3231 and a P-CH MOSFET to cut/give power to an ESP32 and
save the recorded time and temperature onto an SD card.

Copyright (C) 2021 Jessica Droujko

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <sys/time.h>
#include <ds3231.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp32/ulp.h"
#include "driver/touch_pad.h"
#include "driver/adc.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include "soc/sens_periph.h"
#include "soc/rtc.h"
#include "sdkconfig.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

static RTC_DATA_ATTR struct timeval sleep_enter_time;

#ifdef CONFIG_IDF_TARGET_ESP32
#include "driver/sdmmc_host.h"
#endif

#if defined(CONFIG_IDF_TARGET_ESP8266)
#define SDA_GPIO 4
#define SCL_GPIO 5
#else
#define SDA_GPIO 21
#define SCL_GPIO 22
#endif

#define TAG "app"

#define MOUNT_POINT "/sdcard"

// This example can use SDMMC and SPI peripherals to communicate with SD card.
// By default, SDMMC peripheral is used.
// To enable SPI mode, uncomment the following line:

#define USE_SPI_MODE

// DMA channel to be used by the SPI peripheral
#ifndef SPI_DMA_CHAN
#define SPI_DMA_CHAN    1
#endif //SPI_DMA_CHAN

// Pin mapping when using SPI mode.
// With this mapping, SD card can be used both in SPI and 1-line SD mode.
// Note that a pull-up on CS line is required in SD mode.
#define PIN_NUM_MISO 2
#define PIN_NUM_MOSI 15
#define PIN_NUM_CLK  14
#define PIN_NUM_CS   13

enum {
    DS3231_SET = 0,
    DS3231_CLEAR,
    DS3231_REPLACE
};

esp_err_t ds3231_set_flag(i2c_dev_t *dev, uint8_t addr, uint8_t bits, uint8_t mode)
{
    uint8_t data;

    /* get status register */
    esp_err_t res = i2c_dev_read_reg(dev, addr, &data, 1);
    if (res != ESP_OK)
        return res;
    /* clear the flag */
    if (mode == DS3231_REPLACE)
        data = bits;
    else if (mode == DS3231_SET)
        data |= bits;
    else
        data &= ~bits;

    return i2c_dev_write_reg(dev, addr, &data, 1);
}

void app_main(void)
{

    vTaskDelay(2000 / portTICK_PERIOD_MS);

    ESP_ERROR_CHECK(i2cdev_init());

    // Initiate I2C bus
    i2c_dev_t dev;
    memset(&dev, 0, sizeof(i2c_dev_t));

    ESP_ERROR_CHECK(ds3231_init_desc(&dev, 0, SDA_GPIO, SCL_GPIO));

    struct tm time;
    float temp;

    vTaskDelay(250 / portTICK_PERIOD_MS);

    if (ds3231_get_temp_float(&dev, &temp) != ESP_OK)
    {
        printf("Could not get temperature\n");
    }

    if (ds3231_get_time(&dev, &time) != ESP_OK)
    {
        printf("Could not get time\n");
    }

    printf("%04d-%02d-%02d %02d:%02d:%02d, %.2f deg Cel\n", time.tm_year + 1900, time.tm_mon + 1, time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec, temp);

// Enable BBSQW so that DS3231 is able to set off the alarm from the coin cell
    if (ds3231_set_flag(&dev, 0x0e, 0x40,DS3231_SET) != ESP_OK)
    {
        printf("Couldn't write BBSQW register.\n");
    }

    struct timeval now;
    gettimeofday(&now, NULL);
    int sleep_time_ms = (now.tv_sec - sleep_enter_time.tv_sec) * 1000 + (now.tv_usec - sleep_enter_time.tv_usec) / 1000;

    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_TIMER: {
            printf("Wake up from timer. Time spent in deep sleep: %dms\n", sleep_time_ms);
            break;
        }
        case ESP_SLEEP_WAKEUP_UNDEFINED:
        default:
            printf("Not a deep sleep reset\n");
    }

    esp_err_t ret;
    // Options for mounting the filesystem.
    // If format_if_mount_failed is set to true, SD card will be partitioned and
    // formatted in case when mounting fails.
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
#ifdef CONFIG_EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .format_if_mount_failed = true,
#else
        .format_if_mount_failed = false,
#endif // EXAMPLE_FORMAT_IF_MOUNT_FAILED
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t* card;
    const char mount_point[] = MOUNT_POINT;
    ESP_LOGI(TAG, "Initializing SD card");

    ESP_LOGI(TAG, "Using SPI peripheral");

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = PIN_NUM_MOSI,
        .miso_io_num = PIN_NUM_MISO,
        .sclk_io_num = PIN_NUM_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ret = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CHAN);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return;
    }

// This initializes the slot without card detect (CD) and write protect (WP) signals.
// Modify slot_config.gpio_cd and slot_config.gpio_wp if your board has these signals.
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = PIN_NUM_CS;
    slot_config.host_id = host.slot;

    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. "
                "If you want the card to be formatted, set the EXAMPLE_FORMAT_IF_MOUNT_FAILED menuconfig option.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). "
                "Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return;
    }

// Card has been initialized, print its properties
    sdmmc_card_print_info(stdout, card);

    // Print measurements to SD card    
    // Use POSIX and C standard library functions to work with files.
    // First create a file.
    
    printf("Adding date, time, and temperature to file: %04d-%02d-%02d %02d:%02d:%02d, %.2f deg Cel\n", time.tm_year + 1900, time.tm_mon + 1, time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec, temp);

    FILE* f;
    char dataToAppend[1000];

        // Open and append file if it exists, create file if it doesn't exist
        f = fopen(MOUNT_POINT"/turb.txt", "a");
        if (f == NULL){
            ESP_LOGE(TAG, "Failed to open file for writing");
            return;
        }
        ESP_LOGI(TAG, "Opening file again");

        snprintf(dataToAppend, 1000, "%04d-%02d-%02d %02d:%02d:%02d, %.2f deg Cel\n", time.tm_year + 1900, time.tm_mon + 1, time.tm_mday, time.tm_hour, time.tm_min, time.tm_sec, temp);
        // writes character string into buffer
        printf("Writing %s to SD card.\n", dataToAppend);
        fputs(dataToAppend, f);
        fclose(f);
        ESP_LOGI(TAG, "File written again");

    // All done, unmount partition and disable SPI peripheral
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    ESP_LOGI(TAG, "Card unmounted");
    //deinitialize the bus after all devices are removed
    spi_bus_free(host.slot);

    // close all files and allow at least one second for the SD card to power down before pulling the plug
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    const int wakeup_time_sec = 90;
    printf("Enabling timer wakeup, %ds\n", wakeup_time_sec);
    esp_sleep_enable_timer_wakeup(wakeup_time_sec * 1000000);

#if CONFIG_IDF_TARGET_ESP32
    // Isolate GPIO12 pin from external circuits. This is needed for modules
    // which have an external pull-up resistor on GPIO12 (such as ESP32-WROVER)
    // to minimize current consumption.
    rtc_gpio_isolate(GPIO_NUM_12);
#endif

vTaskDelay(1000 / portTICK_PERIOD_MS);

    printf("Entering deep sleep\n");
    gettimeofday(&sleep_enter_time, NULL);

    struct tm time1 = {
        .tm_min = 00,
        .tm_sec = 00
    };

    if (ds3231_clear_alarm_flags(&dev,DS3231_ALARM_2) != ESP_OK)
    {
        printf("Could not clear flags\n");
    }

    if (ds3231_set_alarm(&dev,DS3231_ALARM_2,0,0,&time1,DS3231_ALARM2_EVERY_MIN) != ESP_OK)
    {
        printf("Could not set alarm\n");
    }

    if (ds3231_enable_alarm_ints(&dev,DS3231_ALARM_2) != ESP_OK)
    {
        printf("Could not set interrupts\n");
    }

// ESP32 should already be off by now
    printf("Should theoretically be sleeping (unless you're using a USB and plugged into your PC\n");
    esp_deep_sleep_start();

}
