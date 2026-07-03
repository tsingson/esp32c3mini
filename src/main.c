#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_vfs_dev.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "usb_nvs_test";
uint8_t httpd_enable = 0;

/* ------------------------------------------------------------------
 * 1. NVS 数据持久化逻辑 (保持兼容)
 * ------------------------------------------------------------------ */
void init_and_load_nvs_config(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
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
        nvs_close(my_handle);
    }
}

void save_nvs_config(uint8_t val) {
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_u8(my_handle, "httpd_enble", val);
        nvs_commit(my_handle);
        nvs_close(my_handle);
        httpd_enable = val;
    }
}

/* ------------------------------------------------------------------
 * 2. 严谨的 5 字节二进制协议反馈
 * ------------------------------------------------------------------ */
void send_binary_response(uint8_t cmd, uint8_t status) {
    uint8_t tx_buf[5];
    tx_buf[0] = 0xAA;
    tx_buf[1] = 0xBB;
    tx_buf[2] = cmd;
    tx_buf[3] = status;
    tx_buf[4] = (tx_buf[0] + tx_buf[1] + cmd + status) & 0xFF; // 显式索引校验和

    // 通过 stdout 绕过物理 UART 寄存器，将纯二进制数据直接推入 USB 虚拟端点
    fwrite(tx_buf, 1, 5, stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------
 * 3. 兼容 IDF v5.3.1 的 USB 数据接收守护任务
 * ------------------------------------------------------------------ */
void usb_rx_task(void *pvParameters) {
    // 显式初始化内置 USB_SERIAL_JTAG 驱动
    usb_serial_jtag_driver_config_t usb_config = {
        .rx_buffer_size = 512,
        .tx_buffer_size = 512
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));

    // 【针对 ESP-IDF v5.3.1 的校准核心】将标准的 VFS 文件流挂载到刚刚配置的底层驱动上
    esp_vfs_usb_serial_jtag_use_driver();

    uint8_t data_buf[5];
    int packet_idx = 0;

    while (1) {
        uint8_t ch;
        // 采用 10ms 非阻塞低消轮询，读取内置 USB FIFO 缓冲
        int len = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(10));

        if (len > 0) {
            data_buf[packet_idx++] = ch;

            // 状态机校验：检测流式输入，如果对应帧头对齐不合法，立即重置索引以防死锁
            if (packet_idx == 1 && data_buf[0] != 0xAA) { packet_idx = 0; continue; }
            if (packet_idx == 2 && data_buf[1] != 0xBB) { packet_idx = 0; continue; }

            // 攒满 5 字节，立即进入协议解析
            if (packet_idx == 5) {
                uint8_t cmd = data_buf[2];
                uint8_t val = data_buf[3];
                uint8_t checksum = data_buf[4];
                uint8_t calc_sum = (data_buf[0] + data_buf[1] + cmd + val) & 0xFF;

                if (checksum == calc_sum) {
                    if (cmd == 0x01) { // 动作：主机执行读取
                        send_binary_response(0x01, httpd_enable);
                    }
                    else if (cmd == 0x02) { // 动作：主机执行写入
                        if (val == 0 || val == 1) {
                            save_nvs_config(val);
                            send_binary_response(0x02, 0x01); // 回复成功码 0x01
                        } else {
                            send_binary_response(0x02, 0x00); // 拒绝非法值并回复错误码 0x00
                        }
                    }
                }
                packet_idx = 0; // 重置以便读取下一帧
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

void app_main(void) {
    init_and_load_nvs_config();
    // 挂载至任务
    xTaskCreate(usb_rx_task, "usb_rx_task", 4096, NULL, 10, NULL);
}
