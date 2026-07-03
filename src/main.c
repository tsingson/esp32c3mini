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
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "SAR_PROTOTYPE";

#define BUF_SIZE 512
#define CMD_READ_SSID 0x10
#define CMD_WRITE_SSID 0x11
#define CMD_WRITE_PASS 0x12

// 🎯 需求变更：硬编码默认硬后备网络配置信息
#define DEFAULT_WIFI_SSID "MUSIC"
#define DEFAULT_WIFI_PASS "22676263"

char wifi_ssid[32] = {0};
char wifi_pass[64] = {0};

char temp_ssid[32] = {0};
int temp_ssid_idx = 0;
char temp_pass[64] = {0};
int temp_pass_idx = 0;

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
  if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
    err = nvs_get_u8(my_handle, "httpd_enble", &httpd_enable);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      httpd_enable = 0;
      nvs_set_u8(my_handle, "httpd_enble", httpd_enable);
      nvs_commit(my_handle);
    }

    // 🎯 需求变更：如果 Flash 为空读取不到 SSID，直接注入并持久化默认账密
    size_t size = sizeof(wifi_ssid);
    if (nvs_get_str(my_handle, "ssid", wifi_ssid, &size) != ESP_OK) {
      strcpy(wifi_ssid, DEFAULT_WIFI_SSID);
      nvs_set_str(my_handle, "ssid", wifi_ssid);
      nvs_commit(my_handle);
    }

    size = sizeof(wifi_pass);
    if (nvs_get_str(my_handle, "pass", wifi_pass, &size) != ESP_OK) {
      strcpy(wifi_pass, DEFAULT_WIFI_PASS);
      nvs_set_str(my_handle, "pass", wifi_pass);
      nvs_commit(my_handle);
    }

    nvs_close(my_handle);
  }
}

void save_wifi_string(const char *key, const char *value) {
  nvs_handle_t my_handle;
  if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
    nvs_set_str(my_handle, key, value);
    nvs_commit(my_handle);
    nvs_close(my_handle);
  }
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

// 接收传入参数，支持传入自定义配置或硬编码默认配置
bool init_wifi_and_connect(const char *target_ssid, const char *target_pass) {
  if (strlen(target_ssid) == 0) {
    return false;
  }

  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, target_ssid,
          sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, target_pass,
          sizeof(wifi_config.sta.password));

  // 强行命令 Wi-Fi 驱动使用 RAM 配置，忽略并覆盖过往隐藏的乱连历史
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  wifi_connected = false;
  if (esp_wifi_start() != ESP_OK)
    return false;

  // 有限阻塞等待 10 秒
  int retry = 0;
  while (!wifi_connected && retry < 20) {
    vTaskDelay(pdMS_TO_TICKS(500));
    retry++;
  }
  return wifi_connected;
}

void stop_wifi(void) {
  esp_wifi_disconnect();
  esp_wifi_stop();
  wifi_connected = false;
}

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
  esp_err_t err = esp_http_client_perform(client);
  bool success = (err == ESP_OK);
  esp_http_client_cleanup(client);
  return success;
}

/* ------------------------------------------------------------------
 * 3. 搜救主业务任务循环 (支持异常自动降级自愈)
 * ------------------------------------------------------------------ */
void sar_mission_task(void *pvParameters) {
  ESP_ERROR_CHECK(esp_netif_init());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

  while (1) {
    int retry_count = 0;
    bool bidi_transmission_success = false;

    // 🚀 尝试 1：优先采用 Flash 存储的用户网络账密进行连接上报
    if (init_wifi_and_connect(wifi_ssid, wifi_pass)) {
      while (retry_count < 3) {
        if (fetch_httpbin()) {
          bidi_transmission_success = true;
          break;
        }
        retry_count++;
        vTaskDelay(pdMS_TO_TICKS(2000));
      }
      stop_wifi();
    }

    // 🚀 尝试 2（🎯 降级核心）：如果上面因为连接超时或访问 httpbin 失败 3 次
    if (!bidi_transmission_success) {
      if (g_usb_driver_installed) {
        printf("\n⚠️ [自愈警告] 用户配置连接失败，强制降级使用默认安全网络线路: "
               "%s ...\n",
               DEFAULT_WIFI_SSID);
      }

      // 强制启用硬编码后备账密进行兜底连接
      if (init_wifi_and_connect(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS)) {
        retry_count = 0;
        while (retry_count < 3) {
          if (fetch_httpbin()) {
            break;
          }
          retry_count++;
          vTaskDelay(pdMS_TO_TICKS(2000));
        }
        stop_wifi();
      }
    }

    // 完成本周期上报后，纯软件挂起 3 分钟进行下一次轮询
    vTaskDelay(pdMS_TO_TICKS(3 * 60 * 1000));
  }
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
            printf("\n[CURRENT_WIFI] SSID:%s\n", wifi_ssid);
            send_packet(CMD_READ_SSID, 0x01, 0x01);
          } else if (cmd == CMD_WRITE_SSID) {
            if (flag == 0x00) {
              temp_ssid[temp_ssid_idx] = '\0';
              strcpy(wifi_ssid, temp_ssid);
              save_wifi_string("ssid", wifi_ssid);
              temp_ssid_idx = 0;
              send_packet(CMD_WRITE_SSID, 0x01, 0x01);
            } else {
              if (temp_ssid_idx < 31)
                temp_ssid[temp_ssid_idx++] = flag;
            }
          } else if (cmd == CMD_WRITE_PASS) {
            if (flag == 0x00) {
              temp_pass[temp_pass_idx] = '\0';
              strcpy(wifi_pass, temp_pass);
              save_wifi_string("pass", wifi_pass);
              temp_pass_idx = 0;
              send_packet(CMD_WRITE_PASS, 0x01, 0x01);
            } else {
              if (temp_pass_idx < 63)
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

void app_main(void) {
  vTaskDelay(pdMS_TO_TICKS(1000));
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  init_and_load_nvs_config();

  usb_serial_jtag_driver_config_t usb_config = {.rx_buffer_size = 512,
                                                .tx_buffer_size = 512};
  if (!g_usb_driver_installed) {
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
    usb_serial_jtag_vfs_use_driver();
    g_usb_driver_installed = true;
  }
  xTaskCreate(usb_rx_task, "usb_rx_task", 3072, NULL, 10, NULL);
  xTaskCreate(sar_mission_task, "sar_mission", 5120, NULL, 5, NULL);
}
