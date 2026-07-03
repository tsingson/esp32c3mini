#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_vfs_dev.h"
#include "driver/usb_serial_jtag.h"

static const char *TAG = "usb_combined_test";

#define QUEUE_SIZE 16
#define BUF_SIZE   1024

// 全局功能参数 (加回 httpd_enable 逻辑)
uint8_t httpd_enable = 0;

// 16位通用数据队列数组
uint8_t stream_queue[QUEUE_SIZE] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};

/* ------------------------------------------------------------------
 * 1. NVS 数据持久化逻辑 (加回核心功能)
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
            ESP_LOGI(TAG, "httpd_enble not found. Set default to 0");
        } else {
            ESP_LOGI(TAG, "Loaded httpd_enble from NVS: %d", httpd_enable);
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
        ESP_LOGI(TAG, "Synced httpd_enble value %d to NVS", val);
    }
}

/* ------------------------------------------------------------------
 * 2. 5 字节二进制协议打包发送函数
 * ------------------------------------------------------------------ */
void send_binary_packet(uint8_t cmd, uint8_t index_or_status, uint8_t val) {
    uint8_t tx_buf[5];
    tx_buf[0] = 0xAA;
    tx_buf[1] = 0xBB;
    tx_buf[2] = cmd;
    tx_buf[3] = index_or_status;
    tx_buf[4] = val;

    // 动态计算前 4 字节的累加校验和
    uint8_t sum = 0;
    for(int i = 0; i < 4; i++) {
        sum += tx_buf[i];
    }
    tx_buf[4] = sum;

    // 通过 stdout 直接推入 USB 虚拟端点
    fwrite(tx_buf, 1, 5, stdout);
    fflush(stdout);
}

/* ------------------------------------------------------------------
 * 3. 状态机调度的流式数据周期性（0.5s）喷吐输出函数
 * ------------------------------------------------------------------ */
void handle_streaming_output(void) {
    for (int i = 0; i < QUEUE_SIZE; i++) {
        // cmd=0x03: index_or_status域携带索引，val域携带该位置的数据
        send_binary_packet(0x03, (uint8_t)i, stream_queue[i]);
        vTaskDelay(pdMS_TO_TICKS(3000)); // 严格阻塞半秒，实现流式跨度步进
    }
}

/* ------------------------------------------------------------------
 * 4. 虚拟串口流多指令通用解析状态机 (融合所有控制业务)
 * ------------------------------------------------------------------ */
void usb_rx_task(void *pvParameters) {
    usb_serial_jtag_driver_config_t usb_config = {
        .rx_buffer_size = 512,
        .tx_buffer_size = 512
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb_config));

    // 锁定对接专属于 ESP-IDF v5.x 的 VFS 映射接口
    esp_vfs_usb_serial_jtag_use_driver();

    uint8_t data_buf[BUF_SIZE];
    int packet_idx = 0;

    while (1) {
        uint8_t ch;
        int len = usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(10));

        if (len > 0) {
            if (packet_idx >= BUF_SIZE) {
                packet_idx = 0;
            }

            data_buf[packet_idx++] = ch;

            // 帧头强制对齐检测
            if (packet_idx == 1 && data_buf[0] != 0xAA) { packet_idx = 0; continue; }
            if (packet_idx == 2 && data_buf[1] != 0xBB) { packet_idx = 0; continue; }

            // 成功攒满一整包（5 字节）
            if (packet_idx == 5) {
                uint8_t cmd = data_buf[2];
                uint8_t status_or_idx = data_buf[3];
                uint8_t val = data_buf[4];

                // 校验和验证
                uint8_t calc_sum = 0;
                for(int i = 0; i < 4; i++) {
                    calc_sum += data_buf[i];
                }

                if (val == calc_sum) {
                    // --- 核心业务分支分配 ---
                    if (cmd == 0x01) {
                        // 【指令 0x01 融合】：主机读取旧有的 httpd_enable 值
                        send_binary_packet(0x01, 0x00, httpd_enable);
                    }
                    else if (cmd == 0x02) {
                        // 【指令 0x02 融合】：主机修改旧有的 httpd_enable 值并存入 NVS
                        if (status_or_idx == 0 || status_or_idx == 1) {
                            save_nvs_config(status_or_idx);
                            send_binary_packet(0x02, 0x01, 0x01); // 回复写入成功 ACK
                        } else {
                            send_binary_packet(0x02, 0x00, 0x00); // 非法状态拒签
                        }
                    }
                    else if (cmd == 0x03) {
                        // 【指令 0x03 新增】：触发 16位队列流式吐出服务 (每隔0.5s喷发)
                        handle_streaming_output();
                    }
                    else if (cmd == 0x04) {
                        // 【指令 0x04 新增】：接收 Go 间隔 0.5s 上报的流式队列写入请求
                        if (status_or_idx < QUEUE_SIZE) {
                            stream_queue[status_or_idx] = val;
                            send_binary_packet(0x04, status_or_idx, 0x01); // 反馈 ACK 成功确认
                        } else {
                            send_binary_packet(0x04, status_or_idx, 0x00); // 越界拒签
                        }
                    }
                }
                packet_idx = 0; // 重置状态机，准备接收下一帧
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

void app_main(void) {
    // 初始等待，规避拔插或打开串口时的电涌杂讯
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 初始化并加载旧的 NVS 参数
    init_and_load_nvs_config();

    // 注册统一的交互与流式多任务引擎
    xTaskCreate(usb_rx_task, "usb_rx_task", 4096, NULL, 10, NULL);
}
