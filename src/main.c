#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "lwip/sys.h"
#include "lwip/dns.h"
#include "lwip/sockets.h"

static const char *TAG = "wifi_ap_admin";

#ifdef ARDUINO_ESP32C3_DEV
// 软热点配置修改
#define ESP_WIFI_SSID      "esp32cfgc3` "
#elif
#define ESP_WIFI_SSID      "esp32cfg"
#endif

//
#define ESP_WIFI_PASS      "12345678"
#define ESP_WIFI_CHANNEL   1
#define MAX_STA_CONN       4
#define DNS_PORT           53

// 全局变量，定义为字符数组缓冲区
char device_name[32] = "Default-Beetle";

/* ------------------------------------------------------------------
 * 1. HTML 网页模板
 * ------------------------------------------------------------------ */
const char* html_page =
    "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
    "<title>ESP32 参数配置后台</title>"
    "<style>body{font-family:Arial,sans-serif;margin:20px;background:#f0f2f5;}"
    ".card{background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);max-width:400px;margin:auto;}"
    "input[type=text]{width:100%%;padding:10px;margin:10px 0;box-sizing:border-box;border:1px solid #ccc;border-radius:4px;}"
    "input[type=submit]{width:100%%;background:#007bff;color:white;padding:10px;border:none;border-radius:4px;cursor:pointer;}"
    "input[type=submit]:hover{background:#0056b3;}</style></head>"
    "<body><div class=\"card\"><h2>设备参数设置</h2>"
    "<p>当前设备名称: <strong>%s</strong></p>"
    "<form action=\"/admin\" method=\"POST\">"
    "新设备名称:<br><input type=\"text\" name=\"devname\" placeholder=\"请输入新名称\">"
    "<input type=\"submit\" value=\"保存并保存到NVS\">"
    "</form></div></body></html>";

/* ------------------------------------------------------------------
 * 2. HTTP Server 路由与处理函数
 * ------------------------------------------------------------------ */

esp_err_t admin_get_handler(httpd_req_t *req) {
    char response[1024];
    snprintf(response, sizeof(response), html_page, device_name);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

esp_err_t admin_post_handler(httpd_req_t *req) {
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char value[32] = {0};
    if (httpd_query_key_value(buf, "devname", value, sizeof(value)) == ESP_OK) {
        strncpy(device_name, value, sizeof(device_name) - 1);
        device_name[sizeof(device_name) - 1] = '\0';
        ESP_LOGI(TAG, "收到新设备名称: %s", device_name);

        nvs_handle_t my_handle;
        if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
            nvs_set_str(my_handle, "dev_name", device_name);
            nvs_commit(my_handle);
            nvs_close(my_handle);
            ESP_LOGI(TAG, "成功保存到 NVS");
        }
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/admin");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t captive_portal_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "拦截到非法 URI 请求: %s, 正在重定向...", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.8");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}



httpd_handle_t start_webserver(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.max_uri_handlers = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;

    // --- FIX FOR "HTTP HEADER TOO LONG" BUG ---
    // Increase memory allocation for large mobile browser headers
    config.max_resp_headers = 16;       // Default is 8
    config.stack_size = 8192;           // Default is 4096 (prevents stack overflow)
    // Note: In ESP-IDF v5.x, the internal header buffer scales automatically
    // up to the socket receive buffer limit when you increase stack and header count.
    // ------------------------------------------

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t admin_get = { .uri = "/admin", .method = HTTP_GET, .handler = admin_get_handler };
        httpd_register_uri_handler(server, &admin_get);

        httpd_uri_t admin_post = { .uri = "/admin", .method = HTTP_POST, .handler = admin_post_handler };
        httpd_register_uri_handler(server, &admin_post);

        // Catch-all wildcard redirect for Captive Portal
        httpd_uri_t captive_redirect = { .uri = "/*", .method = HTTP_GET, .handler = captive_portal_handler };
        httpd_register_uri_handler(server, &captive_redirect);
    }
    return server;
}


/* ------------------------------------------------------------------
 * 3. DNS 拦截服务器
 * ------------------------------------------------------------------ */
void dns_server_task(void *pvParameters) {
    char rx_buffer[512];
    struct sockaddr_in server_addr;

    int listen_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (listen_sock < 0) {
        vTaskDelete(NULL);
        return;
    }

    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(DNS_PORT);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    while (1) {
        struct sockaddr_storage source_addr;
        socklen_t socklen = sizeof(source_addr);
        int len = recvfrom(listen_sock, rx_buffer, sizeof(rx_buffer), 0, (struct sockaddr *)&source_addr, &socklen);

        if (len > 12) {
            rx_buffer[2] |= 0x80;
            rx_buffer[6] = 0; rx_buffer[7] = 1;

            int idx = len;
            rx_buffer[idx++] = 0xc0; rx_buffer[idx++] = 0x0c;
            rx_buffer[idx++] = 0x00; rx_buffer[idx++] = 0x01;
            rx_buffer[idx++] = 0x00; rx_buffer[idx++] = 0x01;
            rx_buffer[idx++] = 0x00; rx_buffer[idx++] = 0x00; rx_buffer[idx++] = 0x00; rx_buffer[idx++] = 0x3c;
            rx_buffer[idx++] = 0x00; rx_buffer[idx++] = 0x04;
            rx_buffer[idx++] = 192;  rx_buffer[idx++] = 168;  rx_buffer[idx++] = 8; rx_buffer[idx++] = 1;

            sendto(listen_sock, rx_buffer, idx, 0, (struct sockaddr *)&source_addr, socklen);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ------------------------------------------------------------------
 * 4. Wi-Fi AP 初始化 (包含安全验证策略修改)
 * ------------------------------------------------------------------ */
void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    esp_netif_ip_info_t ip_info;
    IP4_ADDR(&ip_info.ip, 192, 168, 8, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 8, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);

    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .channel = ESP_WIFI_CHANNEL,
            .password = ESP_WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            // 启用 WPA2 / WPA3 混合加密模式
            .authmode = WIFI_AUTH_WPA2_WPA3_PSK,
            // 如果密码长度 >= 8 字节，强烈建议同时开启 PMF (保护管理帧)
            .pmf_cfg = {
                .required = false, // 兼容不支持 PMF 的老设备
            },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // 必须在 esp_wifi_start() 成功之后调用
    // WIFI_POWER_8_5dBm 对应 34 (单位是 0.25dBm，即 34 * 0.25 = 8.5)
    esp_wifi_set_max_tx_power(34);
    ESP_LOGI(TAG, "已强制将 Wi-Fi 发射功率限制为 8.5dBm 规避硬件干扰");



    ESP_LOGI(TAG, "Wi-Fi AP 已成功就绪。SSID: esp32cfg 密码: %s", ESP_WIFI_PASS);
}

/* ------------------------------------------------------------------
 * 5. 主入口函数
 * ------------------------------------------------------------------ */
void app_main(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READONLY, &my_handle) == ESP_OK) {
        size_t required_size = sizeof(device_name);
        nvs_get_str(my_handle, "dev_name", device_name, &required_size);
        nvs_close(my_handle);
        ESP_LOGI(TAG, "从 NVS 读取的历史配置名称: %s", device_name);
    }

    wifi_init_softap();
    start_webserver();

    xTaskCreate(dns_server_task, "dns_task", 3072, NULL, 5, NULL);
}
