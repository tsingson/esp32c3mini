#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_vfs_dev.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// 引入网络抽象层
#include "network_interface.h"

// 引入标准网络套接字库
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

static const char *TAG = "SAR_PROTOTYPE";

#define BUF_SIZE 512
#define CMD_READ_SSID 0x10
#define CMD_WRITE_SSID 0x11
#define CMD_WRITE_PASS 0x12

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
static bool wifi_connected_flag = false;

/* ------------------------------------------------------------------
 * 1. NVS 存储管理
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
    nvs_get_u8(my_handle, "httpd_enble", &httpd_enable);

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
 * 2. 具体的 Wi-Fi 驱动实现层 (实现了 network_driver_t 接口)
 * ------------------------------------------------------------------ */
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_connected_flag = false;
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    wifi_connected_flag = true;
  }
}

bool wifi_driver_init(void) {
  static bool basic_netif_inited = false;
  if (!basic_netif_inited) {
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));
    basic_netif_inited = true;
  }
  return true;
}

bool wifi_driver_connect(const char *target, const char *password) {
  if (strlen(target) == 0)
    return false;

  wifi_config_t wifi_config = {0};
  strncpy((char *)wifi_config.sta.ssid, target, sizeof(wifi_config.sta.ssid));
  strncpy((char *)wifi_config.sta.password, password,
          sizeof(wifi_config.sta.password));

  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

  wifi_connected_flag = false;
  if (esp_wifi_start() != ESP_OK)
    return false;

  int retry = 0;
  while (!wifi_connected_flag && retry < 20) {
    vTaskDelay(pdMS_TO_TICKS(500));
    retry++;
  }
  return wifi_connected_flag;
}

void wifi_driver_disconnect(void) {
  esp_wifi_disconnect();
  esp_wifi_stop();
  wifi_connected_flag = false;
}

bool wifi_driver_is_connected(void) { return wifi_connected_flag; }

// 🎯 实例化：将 Wi-Fi 驱动绑定到抽象接口上
const network_driver_t wifi_driver = {.name = "Wi-Fi_BuiltIn",
                                      .init = wifi_driver_init,
                                      .connect = wifi_driver_connect,
                                      .disconnect = wifi_driver_disconnect,
                                      .is_connected = wifi_driver_is_connected};

/* ------------------------------------------------------------------
 * 3. 通用标准 TCP 套接字传输引擎 (将来支持 4G/蓝牙网络)
 * ------------------------------------------------------------------ */
#define WEB_SERVER "httpbin.org"
#define WEB_PORT "80"

bool fetch_httpbin_via_generic_tcp(void) {
  const struct addrinfo hints = {
      .ai_family = AF_INET,
      .ai_socktype = SOCK_STREAM,
  };
  struct addrinfo *res;
  int s = -1;

  // 1. DNS 解析 (该方法由 lwIP 提供，通过底层任意物理网卡发送)
  int err = getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);
  if (err != 0 || res == NULL) {
    return false;
  }

  // 2. 分配标准的 TCP Socket
  s = socket(res->ai_family, res->ai_socktype, 0);
  if (s < 0) {
    freeaddrinfo(res);
    return false;
  }

  // 3. 建立 TCP 握手
  if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
    close(s);
    freeaddrinfo(res);
    return false;
  }
  freeaddrinfo(res);

  // 4. 组装标准 HTTP 1.1 文本请求
  const char *req = "GET /get HTTP/1.1\r\nHost: httpbin.org\r\nUser-Agent: "
                    "ESP32\r\nConnection: close\r\n\r\n";
  if (write(s, req, strlen(req)) < 0) {
    close(s);
    return false;
  }

  // 5. 持续接收流数据并输出到串口
  char recv_buf[128];
  int r;
  do {
    bzero(recv_buf, sizeof(recv_buf));
    r = read(s, recv_buf, sizeof(recv_buf) - 1);
    if (r > 0 && g_usb_driver_installed) {
      // 直接将网络 Payload 原始喷吐到串口
      fwrite(recv_buf, 1, r, stdout);
      fflush(stdout);
    }
  } while (r > 0);

  close(s);
  return true;
}

/* ------------------------------------------------------------------
 * 4. 搜救主业务任务循环 (完全基于抽象接口的 NAL 状态机)
 * ------------------------------------------------------------------ */
void sar_mission_task(void *pvParameters) {
  // 🎯 核心重构：将当前网络驱动指针绑定到 Wi-Fi
  // 未来如果换成 4G 模块，只需将其指向 &cat_4g_driver 即可
  const network_driver_t *net = &wifi_driver;
  net->init();

  while (1) {
    int retry_count = 0;
    bool transmission_success = false;

    // 优先尝试自定义/存储的参数
    if (net->connect(wifi_ssid, wifi_pass)) {
      while (retry_count < 3) {
        // 调用通用 TCP 套接字接收网络流
        if (fetch_httpbin_via_generic_tcp()) {
          transmission_success = true;
          break;
        }
        retry_count++;
        vTaskDelay(pdMS_TO_TICKS(2000));
      }
      net->disconnect();
    }

    // 如果失败，自愈降级使用默认安全线路
    if (!transmission_success) {
      if (g_usb_driver_installed) {
        printf("\n⚠️ [NAL自愈] 参数连接失败，尝试降级后备网络: %s ...\n",
               DEFAULT_WIFI_SSID);
      }

      if (net->connect(DEFAULT_WIFI_SSID, DEFAULT_WIFI_PASS)) {
        retry_count = 0;
        while (retry_count < 3) {
          if (fetch_httpbin_via_generic_tcp()) {
            break;
          }
          retry_count++;
          vTaskDelay(pdMS_TO_TICKS(2000));
        }
        net->disconnect();
      }
    }

    // 无休眠挂起测试，3 分钟后步进
    vTaskDelay(pdMS_TO_TICKS(3 * 60 * 1000));
  }
}

/* ------------------------------------------------------------------
 * 5. 串口配置交互任务 (保持兼容)
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
