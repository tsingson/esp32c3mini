// src/wifi.c
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_vfs_dev.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_interface.h"
#include <stdio.h>
#include <string.h>

// static const char *TAG = "WIFI_DRIVER";
static bool wifi_connected = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_connected = false;
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    wifi_connected = true;
  }
}

static bool wifi_init_driver(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
  return true;
}

static bool wifi_connect_driver(const char *target, const char *password) {
  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, target, sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, password,
          sizeof(wifi_config.sta.password));

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  wifi_connected = false;
  if (esp_wifi_start() != ESP_OK)
    return false;

  for (int i = 0; i < 20; i++) { // 阻塞等待 10s
    if (wifi_connected)
      return true;
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  return false;
}

static void wifi_disconnect_driver(void) {
  esp_wifi_disconnect();
  esp_wifi_stop();
  wifi_connected = false;
}

static bool wifi_is_connected_driver(void) { return wifi_connected; }

// 导出驱动实例
const network_driver_t wifi_driver = {.name = "ESP32_STA",
                                      .init = wifi_init_driver,
                                      .connect = wifi_connect_driver,
                                      .disconnect = wifi_disconnect_driver,
                                      .is_connected = wifi_is_connected_driver};
