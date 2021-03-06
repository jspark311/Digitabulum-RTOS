/*
File:   main.cpp
Author: J. Ian Lindsay
Date:   2017.02.09

Copyright 2016 Manuvr, Inc

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

    .__       ,    .     .
    |  \* _ *-+- _.|_ . .|. .._ _
    |__/|(_]| | (_][_)(_||(_|[ | )
         ._|

Intended target is an WROOM32 SoC module.
*/

/* Manuvr includes */
#include <Platform/Platform.h>
#include <Platform/Peripherals/I2C/I2CAdapter.h>
#include <XenoSession/Console/ManuvrConsole.h>
#include <Transports/ManuvrSocket/ManuvrTCP.h>
#include "Digitabulum/Digitabulum.h"
#include "Digitabulum/DigitabulumPMU/DigitabulumPMU-r2.h"

#ifdef __cplusplus
  extern "C" {
#endif

/* ESP32 includes */
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_task_wdt.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/apps/sntp.h"


// These pins are special on the ESP32. They should not be assigned as inputs
//   since the levels can't be assured at startup:   [0, 2, 5, 12, 15]
#define ESP32_LED_PIN             15  // This is an LED.

/*******************************************************************************
* The confs below are for the current-version WROOM32U board.
* Pin defs given here assume a WROOM32 module.
*******************************************************************************/
/* Pins assigned to CPLD */
const CPLDPins cpld_pins(
  2,   // Reset
  18,  // Transfer request
  32,  // CPLD's IRQ_WAKEUP pin
  16,  // CPLD clock input
  4,   // CPLD OE pin
  255, // CPLD GPIO is not assigned to an MCU pin. It ties to PMU's voltage select line.
  17,  // SPI1 CS
  13,  // SPI1 CLK
  14,  // SPI1 MOSI
  25,  // SPI1 MISO
  35,  // SPI2 CS
  34,  // SPI2 CLK
  33   // SPI2 MOSI
);

/* Pins assigned to LED driver */
const ADP8866Pins adp_opts(
  5,   // Reset
  19,  // IRQ
  ADP8866_OPT_IRQ_PU  // We use the internal pullup.
);

const I2CAdapterOptions i2c_opts(
  0,      // Device number
  26,     // sda
  27,     // scl
  0,      // We don't need the internal pullups.
  100000  // 100kHz
);

const LTC294xOpts gas_gauge_opts(
  21,     // Alert pin
  LTC294X_OPT_ADC_AUTO | LTC294X_OPT_INTEG_SENSE
);

const PowerPlantOpts powerplant_opts(
  255, // 2.5v select pin is driven by the CPLD.
  12,  // Aux regulator enable pin.
  DIGITAB_PMU_FLAG_ENABLED  // Regulator enabled @3.3v
);

const BQ24155Opts charger_opts(
  68,  // Sense resistor is 68 mOhm.
  255, // N/A (STAT)
  23,  // ISEL
  BQ24155USBCurrent::LIMIT_800,  // Hardware limits (if any) on source draw..
  BQ24155_FLAG_ISEL_HIGH  // We want to start the ISEL pin high.
);

const BatteryOpts battery_opts (
  1400,    // Battery capacity (in mAh)
  3.60f,   // Battery dead (in volts)
  3.70f,   // Battery weak (in volts)
  4.15f,   // Battery float (in volts)
  4.2f     // Battery max (in volts)
);

/* Wrapped config for the sensor front-end */
const DigitabulumOpts digitabulum_opts(
  &cpld_pins,
  &adp_opts,
  1
);


#define EXAMPLE_WIFI_SSID "WaddleNest_Quarantine"
#define EXAMPLE_WIFI_PASS "idistrustmydevice"

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = BIT0;

static const char *TAG = "main-cpp";

static void obtain_time(void);
static void initialize_sntp(void);
static void initialise_wifi(void);
static esp_err_t event_handler(void *ctx, system_event_t *event);


static void obtain_time() {
  ESP_ERROR_CHECK( nvs_flash_init() );
  initialise_wifi();
  xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
  initialize_sntp();

  // wait for time to be set
  time_t now = 0;
  struct tm timeinfo = { 0 };
  int retry = 0;
  const int retry_count = 10;
  while(timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
    ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
    vTaskDelay(2000 / portTICK_PERIOD_MS);
    time(&now);
    localtime_r(&now, &timeinfo);
  }
  //ESP_ERROR_CHECK( esp_wifi_stop() );
}


static void initialize_sntp() {
  ESP_LOGI(TAG, "Initializing SNTP");
  sntp_setoperatingmode(SNTP_OPMODE_POLL);
  sntp_setservername(0, "pool.ntp.org");
  sntp_init();
}


static void initialise_wifi() {
  wifi_event_group = xEventGroupCreate();
  ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
  ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
  wifi_config_t wifi_config = {
    .sta = {
      EXAMPLE_WIFI_SSID,
      EXAMPLE_WIFI_PASS,
      WIFI_FAST_SCAN
    }
  };
  ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
  ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
  ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
  ESP_ERROR_CHECK( esp_wifi_start() );
}


static esp_err_t event_handler(void* ctx, system_event_t* event) {
  switch(event->event_id) {
    case SYSTEM_EVENT_SCAN_DONE:
      break;
    case SYSTEM_EVENT_STA_START:
      esp_wifi_connect();
      break;
    case SYSTEM_EVENT_STA_CONNECTED:
      break;
    case SYSTEM_EVENT_STA_STOP:
      break;
    case SYSTEM_EVENT_STA_GOT_IP:
      xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
      break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
      /* This is a workaround as ESP32 WiFi libs don't currently
         auto-reassociate. */
      esp_wifi_connect();
      xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
      break;
    default:
      break;
  }
  return ESP_OK;
}


int8_t send_serialized_frame(ManuLegendPipe* pipe) {
  ESP_LOGI(TAG, "send_serialized_frame()");
  return 0;
}


void manuvr_task(void* pvParameter) {
  esp_task_wdt_add(nullptr);
  Kernel* kernel = platform.kernel();
  unsigned long ms_0 = millis();
  unsigned long ms_1 = ms_0;
  bool odd_even = false;

  I2CAdapter i2c(&i2c_opts);
  kernel->subscribe(&i2c);

  PMU pmu(&i2c, &charger_opts, &gas_gauge_opts, &powerplant_opts, &battery_opts);
  kernel->subscribe((EventReceiver*) &pmu);

  Digitabulum digitabulum(&i2c, &pmu, &digitabulum_opts);
  kernel->subscribe(&digitabulum);

  Digitabulum::frame_cb = send_serialized_frame;

  platform.bootstrap();

  while (1) {
    kernel->procIdleFlags();
    ms_1 = millis();
    kernel->advanceScheduler(ms_1 - ms_0);
    ms_0 = ms_1;
    setPin(ESP32_LED_PIN, odd_even);
    odd_even = !odd_even;
  }
}



/*******************************************************************************
* Main function                                                                *
*******************************************************************************/
void app_main() {
  /*
  * The platform object is created on the stack, but takes no action upon
  *   construction. The first thing that should be done is to call the preinit
  *   function to setup the defaults of the platform.
  */
  platform.platformPreInit();
  gpioDefine(ESP32_LED_PIN, GPIOMode::OUTPUT);

  // The entire front-end driver apparatus lives on the stack.
  xTaskCreate(manuvr_task, "_manuvr", 48000, NULL, (tskIDLE_PRIORITY + 2), NULL);

  // TODO: Ultimately generalize this... Taken from ESP32 examples....
  // https://github.com/espressif/esp-idf/blob/master/examples/protocols/sntp/main/sntp_example_main.c
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  // Is time set? If not, tm_year will be (1970 - 1900).
  if (timeinfo.tm_year < (2016 - 1900)) {
    ESP_LOGI(TAG, "Time is not set yet. Connecting to WiFi and getting time over NTP.");
    obtain_time();
    // update 'now' variable with current time
    time(&now);
  }
  char strftime_buf[64];

  // Set timezone to Eastern Standard Time and print local time
  setenv("TZ", "MST7MDT,M3.2.0/2,M11.1.0", 1);
  tzset();
  localtime_r(&now, &timeinfo);
  strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
  ESP_LOGI(TAG, "The current date/time in CO is: %s", strftime_buf);
  // TODO: End generalize block.
}


#ifdef __cplusplus
}
#endif
