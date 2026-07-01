#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "nvs_flash.h" // 引入 NVS 永久存储头文件
#include "nvs.h"

#define BOOT_BUTTON_PIN 9
#define ESP_INTR_FLAG_DEFAULT 0

// 3分钟定时换算为微秒 (3ULL * 60ULL * 1000ULL * 1000ULL)
#define SLEEP_DURATION_US (3ULL * 60ULL * 1000ULL * 1000ULL)

static const char *TAG = "BOOT_NVS_DEMO";

// 用于接收中断信号的任务句柄
static TaskHandle_t sleep_task_handle = NULL;

// 存储在临时 RAM 中的变量，每次开机后会去 NVS(Flash) 中读取真实值覆盖它
static uint32_t boot_isr_seq = 0;

// 需求 3：BOOT 按下的硬件外部中断服务程序（ISR）
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    // ISR 内部：用效率最高的高级任务通知，瞬间唤醒专门的存储任务
    xTaskNotifyFromISR(sleep_task_handle, 0, eNoAction, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// 专门用来处理 Flash 永久保存和瞬间睡眠的“高优闪击任务”
static void nvs_and_sleep_task(void* arg)
{
    while(1) {
        // 无限阻塞，直到 ISR 按下按键时强行唤醒它
        xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);

        // 瞬间确认电平，确保不是抖动
        if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
            boot_isr_seq++; // 内存计数自增

            // 写入 NVS (Flash 永久存储)
            nvs_handle_t my_handle;
            esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
            if (err == ESP_OK) {
                // 将最新的序号写入 Flash，绑定键名 "boot_seq"
                nvs_set_u32(my_handle, "boot_seq", boot_isr_seq);
                nvs_commit(my_handle); // 强制提交刷新到 Flash 物理颗粒
                nvs_close(my_handle);
            }

            // 配置 3 分钟自动唤醒定时器
            esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);

            // 锁定引脚状态，防止悬空
            gpio_hold_en(BOOT_BUTTON_PIN);

            // 闪击进入 Deep-sleep，瞬间切断电源！
            esp_deep_sleep_start();
        }
    }
}

void app_main(void)
{
    // 初始化 NVS 闪存驱动（必须先初始化，否则无法使用永久存储）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 需求 1 & 5：上电开机，首先去 NVS 中读取曾经保存的自增序号
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &my_handle);
    if (err == ESP_OK) {
        // 如果能读到，就把数据放进 boot_isr_seq 变量中
        nvs_get_u32(my_handle, "boot_seq", &boot_isr_seq);
        nvs_close(my_handle);
    } else {
        // 需求 1：如果读不到（第一次冷开机闪存是空的），它初始化就是 0
        boot_isr_seq = 0;
    }

    // 状态判定：如果是定时器正常唤醒，串口需要缓冲重连
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        vTaskDelay(pdMS_TO_TICKS(2500));
        gpio_hold_dis(BOOT_BUTTON_PIN); // 解冻按键

        printf("\n");
        ESP_LOGE(TAG, "==================================================");
        ESP_LOGE(TAG, "【⏰ 定时器自动唤醒】3分钟休眠结束，程序重新跑。");
        ESP_LOGE(TAG, "成功从非易失 Flash 中加载到最新的序号！");
        ESP_LOGE(TAG, "==================================================");
        vTaskDelay(pdMS_TO_TICKS(2500));
    } else {
        // 外部拔线插电 或者 点按了物理 RST 按钮重启
        vTaskDelay(pdMS_TO_TICKS(1500));
        ESP_LOGI(TAG, "==================================================");
        ESP_LOGI(TAG, "【🔌 物理冷上电 / RST 键复位】");
        ESP_LOGI(TAG, "由于使用了 NVS 永久存储，上电读取到的序号为: %ld", boot_isr_seq);
        ESP_LOGI(TAG, "==================================================");
        vTaskDelay(pdMS_TO_TICKS(2500));
    }

    // 创建“高优闪击任务”，赋予其极高的调度优先级 (configMAX_PRIORITIES - 1)
    xTaskCreate(nvs_and_sleep_task, "flash_sleep_task", 4096, NULL, configMAX_PRIORITIES - 1, &sleep_task_handle);

    // 初始化 BOOT 键配置
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE       // 严格监听下降沿
    };
    gpio_config(&io_conf);

    // 挂载中断服务
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(BOOT_BUTTON_PIN, gpio_isr_handler, (void*) BOOT_BUTTON_PIN);

    // 需求 2：一直打印自增序号..... 直到 boot 按下
    while (1) {
        ESP_LOGI(TAG, "【🔥 永久自增序号 (Flash 级) 🔥】= %ld", boot_isr_seq);
        vTaskDelay(pdMS_TO_TICKS(3000)); // 严格每 3 秒打印一次
    }
}
