#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "esp_vfs_dev.h"
#include "driver/usb_serial_jtag.h"

#define BUF_SIZE        1024
#define CMD_BIDI_STREAM 0x05
#define STREAM_DATA     0x00
#define STREAM_EOF      0x01
#define STREAM_CANCEL   0x02

// 线程安全互斥锁
static SemaphoreHandle_t g_bidi_mutex = NULL;

volatile bool is_bidi_active = false;
volatile uint8_t target_value = 0;
volatile uint8_t current_status = 0;

static TaskHandle_t bidi_tx_task_handle = NULL;

// 稳定性优化：带有缓冲和超时控制的二进制安全发送
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

    // 严谨写入标准输出，采用 fflush 强行排空
    size_t written = fwrite(tx_buf, 1, 5, stdout);
    if (written == 5) {
        fflush(stdout);
    }
}

// 任务 1：高频上行喷吐流 (互斥锁保护 + 逼近算法)
void bidi_tx_task(void *pvParameters) {
    while (1) {
        // 安全锁保护
        if (xSemaphoreTake(g_bidi_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (!is_bidi_active) {
                xSemaphoreGive(g_bidi_mutex);
                break; // 收到退出信号，安全解锁并跳出
            }

            // 逼近算法
            if (current_status < target_value) {
                current_status += 2;
                if (current_status > target_value) current_status = target_value;
            } else if (current_status > target_value) {
                current_status -= 2;
                if (current_status < target_value) current_status = target_value;
            }

            uint8_t snap_status = current_status;
            xSemaphoreGive(g_bidi_mutex);

            // 发送数据（发送动作移出锁区，防止阻塞接收）
            send_binary_packet(CMD_BIDI_STREAM, STREAM_DATA, snap_status);
        }

        vTaskDelay(pdMS_TO_TICKS(500)); // 严格 0.5s 步进
    }

    bidi_tx_task_handle = NULL;
    vTaskDelete(NULL);
}

// 任务 2：下行接收流状态机 (工业级自愈滑动窗口)
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

            // 🚀 可靠性核心：帧头滑动对齐校检 🚀
            if (packet_idx == 1 && data_buf[0] != 0xAA) { packet_idx = 0; continue; }
            if (packet_idx == 2 && data_buf[1] != 0xBB) {
                if (data_buf[1] == 0xAA) { // 处理连续 0xAA 0xAA 0xBB 的边缘情况
                    data_buf[0] = 0xAA; packet_idx = 1;
                } else {
                    packet_idx = 0;
                }
                continue;
            }

            if (packet_idx == 5) {
                uint8_t cmd = data_buf[2];
                uint8_t status_or_flag = data_buf[3];
                uint8_t val = data_buf[4];

                uint8_t calc_sum = 0;
                for(int i = 0; i < 4; i++) calc_sum += data_buf[i];

                if (val == calc_sum) {
                    if (cmd == CMD_BIDI_STREAM) {
                        if (status_or_flag == STREAM_DATA) {
                            if (xSemaphoreTake(g_bidi_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                                target_value = val;
                                if (!is_bidi_active && bidi_tx_task_handle == NULL) {
                                    is_bidi_active = true;
                                    xTaskCreate(bidi_tx_task, "bidi_tx", 3072, NULL, 5, &bidi_tx_task_handle);
                                }
                                xSemaphoreGive(g_bidi_mutex);
                            }
                        }
                        else if (status_or_flag == STREAM_CANCEL || status_or_flag == STREAM_EOF) {
                            if (xSemaphoreTake(g_bidi_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                                if (is_bidi_active) {
                                    is_bidi_active = false; // 触发发送线程安全退出
                                    send_binary_packet(CMD_BIDI_STREAM, STREAM_EOF, 0x00);
                                }
                                xSemaphoreGive(g_bidi_mutex);
                            }
                        }
                    }
                }
                packet_idx = 0; // 清空状态机
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 创建互斥锁
    g_bidi_mutex = xSemaphoreCreateMutex();
    if (g_bidi_mutex != NULL) {
        xTaskCreate(usb_rx_task, "usb_rx_task", 4096, NULL, 10, NULL);
    }
}
