#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    // 等待 2 秒，防止错开 USB 串口初始化时的前几条打印
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        // 方法 1：使用标准格式化输出
        printf("hello gemini (printf)\n");

        // 方法 2：使用 ESP-IDF 推荐的带标签日志输出
        ESP_LOGI(TAG, "hello gemini (ESP_LOGI)");

        // 每隔 1 秒循环输出一次
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
