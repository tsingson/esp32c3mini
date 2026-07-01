#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#define BOOT_BUTTON_PIN 9
#define ESP_INTR_FLAG_DEFAULT 0

// 需求 2 & 4：3 分钟定时自动唤醒 (3分钟 * 60秒 * 1000000微秒)
// 💡 提示：测试时强烈建议将其改为 (5ULL * 1000ULL * 1000ULL) 也就是 5 秒，以便连续测试十几轮
#define SLEEP_DURATION_US (3ULL * 60ULL * 1000ULL * 1000ULL)

static const char *TAG = "BOOT_NVS_DEMO";
static TaskHandle_t sleep_task_handle = NULL;
static uint32_t boot_isr_seq = 0;

// 1. BOOT 按下的底层外部中断服务（ISR）
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xTaskNotifyFromISR(sleep_task_handle, 0, eNoAction, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// 专门用来闪击 NVS 并将芯片送入深睡的任务
static void nvs_and_sleep_task(void* arg)
{
    while(1) {
        xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);

        // 确认按下
        vTaskDelay(pdMS_TO_TICKS(40));
        if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {

            // 严格等待松手：防止手按在上面时就进睡眠导致松手时提前拉醒
            while(gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            vTaskDelay(pdMS_TO_TICKS(40)); // 释放后的弹跳消抖

            // 写入自增序号到非易失性闪存(Flash)
            boot_isr_seq++;
            nvs_handle_t my_handle;
            if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
                nvs_set_u32(my_handle, "boot_seq", boot_isr_seq);
                nvs_commit(my_handle);
                nvs_close(my_handle);
            }

            ESP_LOGW(TAG, ">>> [NVS 写入成功] 当前最新序号: %ld，正在进入深睡...", boot_isr_seq);
            fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(200)); // 留给内置 USB 发送日志的时间

            // ⚡【修复核心】：移除冲突的 gpio_reset_pin 和 gpio_hold_en，防止硬件计数器被冲刷
            // 只做中断的注销，保持引脚配置结构稳定，避免电源管理单元误判
            gpio_isr_handler_remove(BOOT_BUTTON_PIN);

            // 配置纯粹的时间唤醒源
            esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
            esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);

            // 彻底闭眼进入深睡眠，此时 100% 只能由时间定时器触发唤醒
            esp_deep_sleep_start();
        }
    }
}

void app_main(void)
{
    // A. 强制等待 3 秒。因为内置 USB-CDC 在重新上电或唤醒时需要物理时间让电脑识别串口
    vTaskDelay(pdMS_TO_TICKS(3000));

    // B. 安全初始化和恢复 NVS 存储引擎
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // C. 满足要求 1 & 5：开机首先读取自增序号，读不到（比如第一次冷启动）默认就是 0
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        err = nvs_get_u32(my_handle, "boot_seq", &boot_isr_seq);
        if (err != ESP_OK) {
            boot_isr_seq = 0;
            nvs_set_u32(my_handle, "boot_seq", boot_isr_seq);
            nvs_commit(my_handle);
        }
        nvs_close(my_handle);
    } else {
        boot_isr_seq = 0;
    }

    // D. 判定判定唤醒源并呈现到串口
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    printf("\n\n");
    if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        // 验证分支：完全符合要求 4 & 5，时间唤醒成功
        ESP_LOGE(TAG, "==================================================");
        ESP_LOGE(TAG, "【⏰ 时间自动唤醒成功】3分钟休眠已正常到期！");
        ESP_LOGE(TAG, "成功读取永久闪存中的最新序号：%ld", boot_isr_seq);
        ESP_LOGE(TAG, "==================================================");
    } else {
        // 验证分支：第一次接通电源（冷开机）或者戳了 RST 物理重置键
        ESP_LOGI(TAG, "==================================================");
        ESP_LOGI(TAG, "【🔌 物理冷上电 / RST 复位成功】返回到要求 1");
        ESP_LOGI(TAG, "当前读取到的持久化序号为: %ld", boot_isr_seq);
        ESP_LOGI(TAG, "==================================================");
    }
    printf("\n");

    // 创建最高优先级的保存任务
    xTaskCreate(nvs_and_sleep_task, "flash_sleep_task", 4096, NULL, configMAX_PRIORITIES - 1, &sleep_task_handle);

    // E. 唤醒后，重新为 BOOT 按键初始化常规输入及中断矩阵
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,     // 正常运行时启用上拉电阻维持高电平
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE       // 下降沿触发（按下动作瞬间）
    };
    gpio_config(&io_conf);

    // 安装中断服务并绑定处理函数
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(BOOT_BUTTON_PIN, gpio_isr_handler, (void*) BOOT_BUTTON_PIN);

    // F. 满足要求 2：一直打印自增序号..... 直到下一次 boot 按下
    while (1) {
        ESP_LOGI(TAG, "【当前自增序号】= %ld", boot_isr_seq);
        vTaskDelay(pdMS_TO_TICKS(3000)); // 每3秒输出一次
    }
}
