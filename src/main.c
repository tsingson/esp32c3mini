#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_vfs_dev.h"
#include "driver/usb_serial_jtag.h"

#define QUEUE_SIZE 16
#define BUF_SIZE   1024

// 流控命令及状态机定义
#define STREAM_DATA   0x00
#define STREAM_EOF    0x01
#define STREAM_CANCEL 0x02

uint8_t httpd_enable = 0;
uint8_t stream_queue[QUEUE_SIZE] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160};

// 用于管理流式喷吐任务的 FreeRTOS 句柄
static TaskHandle_t stream_tx_task_handle = NULL;

void send_binary_packet(uint8_t cmd, uint8_t index_or_status, uint8_t val) {
    uint8_t tx_buf[5];
    tx_buf[0] = 0xAA;
    tx_buf[1] = 0xBB;
    tx_buf[2] = cmd;
    tx_buf[3] = index_or_status;
    tx_buf[4] = val;

    uint8_t sum = 0;
    for(int i = 0; i < 4; i++) sum += tx_buf[i];
    tx_buf[4] = sum;

    fwrite(tx_buf, 1, 5, stdout);
    fflush(stdout);
}

// 独立的流喷吐服务子线程
void stream_tx_task(void *pvParameters) {
    for (int i = 0; i < QUEUE_SIZE; i++) {
        send_binary_packet(0x03, STREAM_DATA, stream_queue[i]);

        // 将 3 秒的长延时拆解为 30 次 100ms 的短切片，从而能够毫秒级响应外部的中止退出事件
        for (int t = 0; t < 30; t++) {
            uint32_t notify_val;
            // 非阻塞检查是否有外部（主串口任务）下发的杀进程信号
            if (xTaskNotifyWait(0, 0, &notify_val, pdMS_TO_TICKS(100)) == pdTRUE) {
                // 收到强行终止指令，发送中止确认帧后优雅自毁
                send_binary_packet(0x03, STREAM_CANCEL, 0xFF);
                stream_tx_task_handle = NULL;
                vTaskDelete(NULL);
                return;
            }
        }
    }

    // 16位数据顺利吐完，向 Go 发送明确的 Close 信号
    send_binary_packet(0x03, STREAM_EOF, 0x00);
    stream_tx_task_handle = NULL;
    vTaskDelete(NULL);
}

void usb_rx_task(void *pvParameters) {
    usb_serial_jtag_driver_config_t usb_config = { .rx_buffer_size = 512, .tx_buffer_size = 512 };
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
                    else if (cmd == 0x03) {
                        if (status_or_idx == STREAM_DATA) {
                            // 启动流：防止重复拉起多个任务实例
                            if (stream_tx_task_handle == NULL) {
                                xTaskCreate(stream_tx_task, "stream_tx", 3072, NULL, 5, &stream_tx_task_handle);
                            }
                        }
                        else if (status_or_idx == STREAM_CANCEL) {
                            // 🚀【核心中止动作】：如果流正在运行，直接通过任务通知强行唤醒并终止它
                            if (stream_tx_task_handle != NULL) {
                                xTaskNotifyGive(stream_tx_task_handle);
                            }
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
    vTaskDelay(pdMS_TO_TICKS(2000));
    xTaskCreate(usb_rx_task, "usb_rx_task", 4096, NULL, 10, NULL);
}
