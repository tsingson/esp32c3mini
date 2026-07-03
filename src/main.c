#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_vfs_dev.h"
#include "driver/usb_serial_jtag.h"

#define QUEUE_SIZE 16
#define BUF_SIZE   1024

uint8_t httpd_enable = 0;
uint8_t stream_queue[QUEUE_SIZE] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

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

// 严谨的二进制包打包函数
void send_binary_packet(uint8_t cmd, uint8_t index_or_status, uint8_t val) {
    uint8_t tx_buf[5];
    tx_buf[0] = 0xAA;
    tx_buf[1] = 0xBB;
    tx_buf[2] = cmd;
    tx_buf[3] = index_or_status;
    tx_buf[4] = val;

    uint8_t sum = 0;
    for(int i = 0; i < 4; i++) {
        sum += tx_buf[i];
    }
    tx_buf[4] = sum; // 覆盖第5字节为精确校验和

    fwrite(tx_buf, 1, 5, stdout);
    fflush(stdout);
}

// 【修订】：将流式喷吐的间隔强制延长至 3 秒
void handle_streaming_output(void) {
    for (int i = 0; i < QUEUE_SIZE; i++) {
        send_binary_packet(0x03, (uint8_t)i, stream_queue[i]);
        vTaskDelay(pdMS_TO_TICKS(3000)); // ⏳ 严格每隔 3 秒喷发一包
    }
}

void usb_rx_task(void *pvParameters) {
    usb_serial_jtag_driver_config_t usb_config = {
        .rx_buffer_size = 512,
        .tx_buffer_size = 512
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));
    esp_vfs_usb_serial_jtag_use_driver();

    uint8_t data_buf[BUF_SIZE];
    int packet_idx = 0;

    while (1) {
        uint8_t ch;
        int len = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(10));

        if (len > 0) {
            if (packet_idx >= BUF_SIZE) packet_idx = 0;
            data_buf[packet_idx++] = ch;

            // 状态机强行对齐帧头 [0xAA 0xBB]
            if (packet_idx == 1 && data_buf[0] != 0xAA) { packet_idx = 0; continue; }
            if (packet_idx == 2 && data_buf[1] != 0xBB) { packet_idx = 0; continue; }

            if (packet_idx == 5) {
                uint8_t cmd = data_buf[2];
                uint8_t status_or_idx = data_buf[3];
                uint8_t val = data_buf[4];

                uint8_t calc_sum = 0;
                for(int i = 0; i < 4; i++) calc_sum += data_buf[i];

                if (val == calc_sum) {
                    if (cmd == 0x01) {
                        send_binary_packet(0x01, 0x00, httpd_enable);
                    }
                    else if (cmd == 0x02) {
                        if (status_or_idx == 0 || status_or_idx == 1) {
                            save_nvs_config(status_or_idx);
                            send_binary_packet(0x02, 0x01, 0x01);
                        } else {
                            send_binary_packet(0x02, 0x00, 0x00);
                        }
                    }
                    else if (cmd == 0x03) {
                        handle_streaming_output(); // 进入 3 秒步进流式发送
                    }
                    else if (cmd == 0x04) {
                        if (status_or_idx < QUEUE_SIZE) {
                            stream_queue[status_or_idx] = val;
                            send_binary_packet(0x04, status_or_idx, 0x01);
                        } else {
                            send_binary_packet(0x04, status_or_idx, 0x00);
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
    // 上电死等 2 秒，让开发板和 PC 彻底完成 USB 初始化，错开所有硬件电涌
    vTaskDelay(pdMS_TO_TICKS(2000));
    init_and_load_nvs_config();
    xTaskCreate(usb_rx_task, "usb_rx_task", 4096, NULL, 10, NULL);
}
