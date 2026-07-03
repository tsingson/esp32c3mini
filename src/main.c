#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_vfs_dev.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_client.h"
#include "network_interface.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SAR_PROTOTYPE";

#define BUF_SIZE 512
#define CMD_READ_SSID 0x10
#define CMD_WRITE_SSID 0x11
#define CMD_WRITE_PASS 0x12
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64

// 🎯 需求变更：硬编码默认硬后备网络配置信息
#define DEFAULT_WIFI_SSID "MUSIC"
#define DEFAULT_WIFI_PASS "22676263"

char wifi_ssid[WIFI_SSID_MAX_LEN] = {0};
char wifi_pass[WIFI_PASS_MAX_LEN] = {0};

char temp_ssid[WIFI_SSID_MAX_LEN] = {0};
int temp_ssid_idx = 0;
char temp_pass[WIFI_PASS_MAX_LEN] = {0};
int temp_pass_idx = 0;
static SemaphoreHandle_t wifi_cfg_mutex = NULL;

uint8_t httpd_enable = 0;
volatile bool g_usb_driver_installed = false;

/* ------------------------------------------------------------------
 * 1. NVS 持久化存储与加载逻辑 (支持出厂默认注入)
 * ------------------------------------------------------------------ */
void init_and_load_nvs_config(void) {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  nvs_handle_t my_handle;
  err = nvs_open("storage", NVS_READWRITE, &my_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
    return;
  }

  err = nvs_get_u8(my_handle, "httpd_enble", &httpd_enable);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    httpd_enable = 0;
    ESP_ERROR_CHECK(nvs_set_u8(my_handle, "httpd_enble", httpd_enable));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
  } else if (err != ESP_OK) {
    ESP_LOGW(TAG, "nvs_get_u8(httpd_enble) failed: %s", esp_err_to_name(err));
  }

  // 🎯 需求变更：如果 Flash 为空读取不到 SSID，直接注入并持久化默认账密
  size_t size = sizeof(wifi_ssid);
  err = nvs_get_str(my_handle, "ssid", wifi_ssid, &size);
  if (err != ESP_OK) {
    strlcpy(wifi_ssid, DEFAULT_WIFI_SSID, sizeof(wifi_ssid));
    ESP_ERROR_CHECK(nvs_set_str(my_handle, "ssid", wifi_ssid));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
  }

  size = sizeof(wifi_pass);
  err = nvs_get_str(my_handle, "pass", wifi_pass, &size);
  if (err != ESP_OK) {
    strlcpy(wifi_pass, DEFAULT_WIFI_PASS, sizeof(wifi_pass));
    ESP_ERROR_CHECK(nvs_set_str(my_handle, "pass", wifi_pass));
    ESP_ERROR_CHECK(nvs_commit(my_handle));
  }
  nvs_close(my_handle);
}

esp_err_t save_wifi_string(const char *key, const char *value) {
  nvs_handle_t my_handle;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open failed in save_wifi_string(%s): %s", key,
             esp_err_to_name(err));
    return err;
  }
  err = nvs_set_str(my_handle, key, value);
  if (err == ESP_OK) {
    err = nvs_commit(my_handle);
  }
  nvs_close(my_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "save_wifi_string(%s) failed: %s", key, esp_err_to_name(err));
  }
  return err;
}

void send_packet(uint8_t cmd, uint8_t status, uint8_t val) {
  uint8_t tx_buf[5] = {0xAA, 0xBB, cmd, status, val};
  tx_buf[4] = (tx_buf[0] + tx_buf[1] + tx_buf[2] + tx_buf[3]) & 0xFF;

  if (g_usb_driver_installed) {
    fwrite(tx_buf, 1, 5, stdout);
    fflush(stdout);
  }
}

/* ------------------------------------------------------------------
 * 2. Wi-Fi 连接与 HTTP 请求 (支持自愈降级动态覆盖)
 * ------------------------------------------------------------------ */
esp_err_t http_event_handler(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA) {
    if (g_usb_driver_installed) {
      fwrite(evt->data, 1, evt->data_len, stdout);
      fflush(stdout);
    }
  }
  return ESP_OK;
}

bool fetch_httpbin(void) {
  esp_http_client_config_t config = {
      .url = "http://httpbin.org",
      .event_handler = http_event_handler,
      .timeout_ms = 5000,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "esp_http_client_init failed");
    return false;
  }
  esp_err_t err = esp_http_client_perform(client);
  bool success = (err == ESP_OK);
  esp_http_client_cleanup(client);
  return success;
}

/* ------------------------------------------------------------------
 * 4. 串口全双工配置交互任务
 * ------------------------------------------------------------------ */
void usb_rx_task(void *pvParameters) {
  uint8_t data_buf[BUF_SIZE];
  int packet_idx = 0;

  while (1) {
    uint8_t ch;
    int len = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(10));
    if (len > 0) {
      if (packet_idx >= BUF_SIZE)
        packet_idx = 0;
      data_buf[packet_idx++] = ch;

      if (packet_idx == 1 && data_buf[0] != 0xAA) {
        packet_idx = 0;
        continue;
      }
      if (packet_idx == 2 && data_buf[1] != 0xBB) {
        packet_idx = 0;
        continue;
      }

      if (packet_idx == 5) {
        uint8_t cmd = data_buf[2];
        uint8_t flag = data_buf[3];
        uint8_t val = data_buf[4];
        uint8_t calc_sum = (data_buf[0] + data_buf[1] + cmd + flag) & 0xFF;

        if (val == calc_sum) {
          if (cmd == CMD_READ_SSID) {
            if (wifi_cfg_mutex != NULL) {
              xSemaphoreTake(wifi_cfg_mutex, portMAX_DELAY);
            }
            printf("\n[CURRENT_WIFI] SSID:%s\n", wifi_ssid);
            if (wifi_cfg_mutex != NULL) {
              xSemaphoreGive(wifi_cfg_mutex);
            }
            send_packet(CMD_READ_SSID, 0x01, 0x01);
          } else if (cmd == CMD_WRITE_SSID) {
            if (flag == 0x00) {
              temp_ssid[temp_ssid_idx] = '\0';
              if (wifi_cfg_mutex != NULL) {
                xSemaphoreTake(wifi_cfg_mutex, portMAX_DELAY);
              }
              strlcpy(wifi_ssid, temp_ssid, sizeof(wifi_ssid));
              if (wifi_cfg_mutex != NULL) {
                xSemaphoreGive(wifi_cfg_mutex);
              }
              save_wifi_string("ssid", wifi_ssid);
              temp_ssid_idx = 0;
              send_packet(CMD_WRITE_SSID, 0x01, 0x01);
            } else {
              if (temp_ssid_idx < (int)(sizeof(temp_ssid) - 1))
                temp_ssid[temp_ssid_idx++] = flag;
            }
          } else if (cmd == CMD_WRITE_PASS) {
            if (flag == 0x00) {
              temp_pass[temp_pass_idx] = '\0';
              if (wifi_cfg_mutex != NULL) {
                xSemaphoreTake(wifi_cfg_mutex, portMAX_DELAY);
              }
              strlcpy(wifi_pass, temp_pass, sizeof(wifi_pass));
              if (wifi_cfg_mutex != NULL) {
                xSemaphoreGive(wifi_cfg_mutex);
              }
              save_wifi_string("pass", wifi_pass);
              temp_pass_idx = 0;
              send_packet(CMD_WRITE_PASS, 0x01, 0x01);
            } else {
              if (temp_pass_idx < (int)(sizeof(temp_pass) - 1))
                temp_pass[temp_pass_idx++] = flag;
            }
          }
        }
        packet_idx = 0;
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
}

/////////

extern const network_driver_t wifi_driver; // 引用外部驱动

void sar_mission_task(void *pvParameters) {
  // 统一初始化
  wifi_driver.init();

  while (1) {
    // 使用驱动进行业务逻辑
    bool success = false;
    char active_ssid[WIFI_SSID_MAX_LEN];
    char active_pass[WIFI_PASS_MAX_LEN];

    if (wifi_cfg_mutex != NULL) {
      xSemaphoreTake(wifi_cfg_mutex, portMAX_DELAY);
    }
    strlcpy(active_ssid, wifi_ssid, sizeof(active_ssid));
    strlcpy(active_pass, wifi_pass, sizeof(active_pass));
    if (wifi_cfg_mutex != NULL) {
      xSemaphoreGive(wifi_cfg_mutex);
    }

    // 尝试主配置
    // if (wifi_driver.connect(wifi_ssid, wifi_pass)) {
    //   success = fetch_httpbin(); // fetch 函数保持在 main 中或移至单独的
    //   http.c wifi_driver.disconnect();
    // }
    if (wifi_driver.connect(active_ssid, active_pass)) {
      // 调用我们刚写的 TCP 版本
      success = fetch_httpbin_via_tcp();
      wifi_driver.disconnect();
    }

    // 尝试后备配置
    if (!success) {
      if (wifi_driver.connect(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS)) {
        fetch_httpbin();
        wifi_driver.disconnect();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(3 * 60 * 1000));
  }
}
//
// void app_main(void) {
//   // ... 初始化 NVS, USB 等 ...
//   xTaskCreate(sar_mission_task, "sar_mission", 5120, NULL, 5, NULL);
// }

void app_main(void) {

  vTaskDelay(pdMS_TO_TICKS(1000));
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  init_and_load_nvs_config();
  wifi_cfg_mutex = xSemaphoreCreateMutex();
  ESP_ERROR_CHECK(wifi_cfg_mutex != NULL ? ESP_OK : ESP_FAIL);

  usb_serial_jtag_driver_config_t usb_config = {.rx_buffer_size = 512,
                                                .tx_buffer_size = 512};

  ESP_LOGD(TAG, "This is a debug message.");


  if (!g_usb_driver_installed) {
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
    usb_serial_jtag_vfs_use_driver();
    g_usb_driver_installed = true;
  }
  xTaskCreate(usb_rx_task, "usb_rx_task", 3072, NULL, 10, NULL);
  xTaskCreate(sar_mission_task, "sar_mission", 5120, NULL, 5, NULL);
}
