#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"

#define BOOT_BUTTON_PIN 9
#define ESP_INTR_FLAG_DEFAULT 0

static const char *TAG = "BOOT_SLEEP";
static QueueHandle_t gpio_evt_queue = NULL;

// 使用 RTC_DATA_ATTR 将变量保存在 RTC 内存中，这样在深睡眠期间数据不会丢失
static RTC_DATA_ATTR uint32_t press_count = 0;

// 中断服务程序（ISR）
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

// 按钮事件与睡眠处理任务
static void button_task(void* arg)
{
    uint32_t io_num;
    for(;;) {
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
            // 简单的软件消抖：按下后等待 50ms 确认状态
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(io_num) == 0) {

                press_count++; // 计数器自增
                ESP_LOGI(TAG, "BOOT 按键被按下！当前总次数: %ld", press_count);

                // 奇数次按下：进入 Deep-sleep
                if (press_count % 2 != 0) {
                    ESP_LOGW(TAG, "检测到奇数次按下，即将进入 Deep-sleep 模式...");

                    // 配置 GPIO9（BOOT键）作为低电平唤醒源（因为按键按下时为低电平）
                    // 这样当下一次（偶数次）按下按键时，芯片就会被唤醒
                    esp_deep_sleep_enable_gpio_wakeup(1ULL << BOOT_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);

                    // 稍作延时确保串口日志输出完毕，然后切断电源进入深睡眠
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_deep_sleep_start();
                }
                // 正常运行下的偶数次按下（通常唤醒后直接在 app_main 处理，此处做备用逻辑）
                else {
                    ESP_LOGI(TAG, "检测到偶数次按下，系统处于正常运行状态。");
                }
            }
        }
    }
}

void app_main(void)
{
    // 检查芯片本次启动是否是从深睡眠中唤醒的
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
        // 如果是 GPIO（BOOT键）唤醒的，说明这是“偶数次”按下
        press_count++; // 补充由于唤醒动作带来的计数
        ESP_LOGW(TAG, "=== ESP32-C3 已唤醒 ===");
        ESP_LOGI(TAG, "唤醒原因: 按钮触发唤醒（偶数次按下，当前总次数: %ld）", press_count);
    } else {
        // 冷启动或复位启动
        ESP_LOGI(TAG, "系统正常启动（非深睡眠唤醒）");
    }

    // 常规 GPIO 中断配置
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE         // 下降沿触发
    };
    gpio_config(&io_conf);

    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    xTaskCreate(button_task, "button_task", 3072, NULL, 10, NULL);

    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(BOOT_BUTTON_PIN, gpio_isr_handler, (void*) BOOT_BUTTON_PIN);

    ESP_LOGI(TAG, "系统就绪，请按下 BOOT 键测试。");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
