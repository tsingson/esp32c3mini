#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_idf_version.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/uart_vfs.h"

static const char *TAG = "C3_SUPERMINI_PM";

// ================= 【硬件与睡眠控制核心配置】 =================
#define BOOT_BUTTON_PIN       GPIO_NUM_9          // ESP32-C3 SuperMini 板载 BOOT 键固定在 GPIO9
#define UART_BUF_SIZE         256

typedef enum {
    SLEEP_MODE_LIGHT = 0,                         // 浅度睡眠测试模式
    SLEEP_MODE_DEEP  = 1                          // 深度睡眠户外生产模式
} custom_sleep_mode_t;

// 核心控制策略配置（支持 RTC 跨周期保持）
static RTC_DATA_ATTR custom_sleep_mode_t current_sleep_strategy = SLEEP_MODE_LIGHT;
static RTC_DATA_ATTR uint32_t config_sleep_time_sec = 180;                         // 默认休眠 3 分钟
static uint32_t config_work_window_ms = 30000;                                     // 默认工作窗口 30 秒

static EventGroupHandle_t xWorkEventGroup = NULL;
#define WORK_FINISHED_BIT    (1 << 0)

static RTC_DATA_ATTR uint32_t s_wakeup_count = 0;

// ================= 【功能1：C3 SuperMini 硬件指标诊断报告】 =================
static void print_chip_diagnostics(void) {
    esp_chip_info_t chip_info;
    uint32_t flash_size;

    esp_chip_info(&chip_info);

    printf("\n======================================================\n");
    printf("         ESP32-C3 SuperMini 硬件指标诊断报告          \n");
    printf("======================================================\n");
    printf("  [软件] ESP-IDF SDK 版本 : %s\n", esp_get_idf_version());
    printf("  [芯片] 核心微架构类型   : ");
    if (chip_info.model == CHIP_ESP32C3) {
        printf("RISC-V 32-bit (ESP32-C3 正品硅片)\n");
    } else {
        printf("未知架构 (代码: %d)\n", chip_info.model);
    }
    printf("  [芯片] 物理 CPU 核心数  : %d 核\n", chip_info.cores);
    printf("  [芯片] 硅片硬件版本     : v%" PRIu16 "\n", chip_info.revision);
    printf("  [外设] 内置功能支持     : ");
    printf("%s ", (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "Wi-Fi" : "");
    printf("%s ", (chip_info.features & CHIP_FEATURE_BLE) ? "| BLE" : "");
    printf("%s\n", (chip_info.features & CHIP_FEATURE_IEEE802154) ? "| Zigbee" : "");

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        printf("  [闪存] 外部 SPI Flash   : %lu MB (%lu 字节)\n", flash_size / (1024 * 1024), flash_size);
    }
    printf("  [封装] Flash 物理拓扑   : %s 闪存\n",
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "片上集成(Embedded)" : "外部引脚挂载(External)");
    printf("======================================================\n\n");
}

static void init_hardware_gpio(void) {
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL, // 唤醒源低电平触发
    };
    gpio_config(&btn_conf);
}

// ================= 【功能6：工作时间内接收串口字符串并动态修改】 =================
static void vUartReceiverTask(void *pvParameters) {
    // 彻底修复内存指针越界隐患，采用正规字节数组缓冲区
    uint8_t rx_buffer[UART_BUF_SIZE];
    ESP_LOGI(TAG, "串口交互监视器已在工作期拉起...");

    while (1) {
        int len = uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, rx_buffer, sizeof(rx_buffer) - 1, pdMS_TO_TICKS(10));
        if (len > 0) {
            rx_buffer[len] = '\0';
            char *input_str = (char *)rx_buffer;

            // 剪除末尾换行符
            input_str[strcspn(input_str, "\r\n")] = 0;
            ESP_LOGI(TAG, "收到串口指令: \"%s\"", input_str);

            if (strncmp(input_str, "set_sleep=", 10) == 0) {
                uint32_t parsed_time = atoi(input_str + 10);
                if (parsed_time > 0) {
                    config_sleep_time_sec = parsed_time;
                    ESP_LOGW(TAG, "🎯 成功更新下一次休眠时间为: %lu 秒!", config_sleep_time_sec);
                }
            }
            else if (strcmp(input_str, "finish") == 0) {
                ESP_LOGW(TAG, "收到手工提前结束工作指令！");
                xEventGroupSetBits(xWorkEventGroup, WORK_FINISHED_BIT);
            }
            else if (strcmp(input_str, "toggle_mode") == 0) {
                current_sleep_strategy = (current_sleep_strategy == SLEEP_MODE_LIGHT) ? SLEEP_MODE_DEEP : SLEEP_MODE_LIGHT;
                ESP_LOGW(TAG, "🔄 睡眠模式已切换！下一次将使用: %s",
                         (current_sleep_strategy == SLEEP_MODE_DEEP) ? "DEEP SLEEP (深睡)" : "LIGHT SLEEP (浅睡)");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// 模拟传感器业务核心处理任务
static void vDummyWorkExecution(void *pvParameters) {
    ESP_LOGI(TAG, "--- 核心业务工作开始执行 ---");

    // 【可配置工作时间测试项】：10 秒即可干完活，完美触发提前休眠
    const int work_duration_test_sec = 10;

    for (int i = 1; i <= work_duration_test_sec; i++) {
        ESP_LOGI(TAG, " 🛠️ 正在执行核心传感器数据打包...进度 [%d/%d]", i, work_duration_test_sec);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "--- 核心业务工作顺利结束！宣告释放休眠锁 ---");
    xEventGroupSetBits(xWorkEventGroup, WORK_FINISHED_BIT);
    vTaskDelete(NULL); // 彻底释放自身句柄，完美避免 Light Sleep 内存堆泄漏
}

void app_main(void) {
    s_wakeup_count++;
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    // 初始化通用 UART0 控制台
    uart_config_t uart_config = { .baud_rate = 115200, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE };
    uart_param_config(CONFIG_ESP_CONSOLE_UART_NUM, &uart_config);
    uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0);

    init_hardware_gpio();
    xWorkEventGroup = xEventGroupCreate();

    // 逻辑判定：只有冷启动才完整打印大报告，如果是定时器或按键唤醒则打印紧凑小日志
    if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "⏰ 唤醒状态: 由 RTC 定时器自动唤醒。唤醒序号: %lu", s_wakeup_count);
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGW(TAG, "🖱️ 唤醒状态: 检测到物理 BOOT 按键触发唤醒！唤醒序号: %lu", s_wakeup_count);
    } else {
        s_wakeup_count = 1;
        print_chip_diagnostics(); // 冷启动：强效输出 C3 SuperMini 全套指标查询数据
    }

    printf("  [当前配置] 设定的休眠周期 : %lu 秒 (默认3分钟)\n", config_sleep_time_sec);
    printf("  [当前配置] 使用的睡眠模式 : %s\n", (current_sleep_strategy == SLEEP_MODE_DEEP) ? "DEEP SLEEP (深睡)" : "LIGHT SLEEP (浅睡)");
    printf("------------------------------------------------------\n\n");

    // 开启异步工作期并发任务
    TaskHandle_t uart_task_handle = NULL;
    xTaskCreate(vUartReceiverTask, "uart_task", 3072, NULL, 10, &uart_task_handle);
    xTaskCreate(vDummyWorkExecution, "work_task", 3072, NULL, 5, NULL);

    // ================= 【功能5：可配置的工作时间控制状态机】 =================
    TickType_t xStartTicks = xTaskGetTickCount();

    // 等待核心工作标志位（动态卡死 30 秒限制）
    EventBits_t bits = xEventGroupWaitBits(
        xWorkEventGroup,
        WORK_FINISHED_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(config_work_window_ms)
    );

    if (bits & WORK_FINISHED_BIT) {
        uint32_t actual_cost_ms = pdTICKS_TO_MS(xTaskGetTickCount() - xStartTicks);
        ESP_LOGI(TAG, "👍 满足【提前休眠条件】：工作在 %lu 毫秒内提早结束（少于30秒），系统将立即转入休眠。", actual_cost_ms);
    } else {
        ESP_LOGW(TAG, "⏳ 触发【工作超时拉伸】：已满 30 秒但外部工作尚未完成！主核将强制保持常开，死等工作结束...");
        xEventGroupWaitBits(xWorkEventGroup, WORK_FINISHED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        ESP_LOGI(TAG, "🏁 业务数据完整性获得保护！超时工作最终完成，获准进入休眠。");
    }

    // 安全回收串口多线程资源，防止锁死
    if (uart_task_handle != NULL) {
        vTaskDelete(uart_task_handle);
    }

    // ================= 【功能4：可配置的 sleep 时间设定（默认3分钟）】 =================
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(config_sleep_time_sec * 1000000ULL));

    // ================= 【功能1/2/3：独立的深睡/浅睡及引脚中断唤醒控制】 =================
    if (current_sleep_strategy == SLEEP_MODE_DEEP) {
        // --- 深度睡眠（Deep Sleep）下的 GPIO9 中断绑定 ---
        ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(1ULL << BOOT_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW));

        ESP_LOGI(TAG, "🌙 芯片即将切入断电的 [DEEP SLEEP]... %lu 秒后自动复位或随时按 BOOT 键唤醒。", config_sleep_time_sec);
        uart_wait_tx_idle_polling(CONFIG_ESP_CONSOLE_UART_NUM);
        esp_deep_sleep_start();
    }
    else {
        // --- 浅度睡眠（Light Sleep）下的 GPIO9 中断绑定 ---
        ESP_ERROR_CHECK(gpio_wakeup_enable(BOOT_BUTTON_PIN, GPIO_INTR_LOW_LEVEL));
        ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());

        ESP_LOGI(TAG, "💤 芯片即将切入挂起时钟的 [LIGHT SLEEP]... %lu 秒后恢复或随时按 BOOT 键唤醒。", config_sleep_time_sec);
        uart_wait_tx_idle_polling(CONFIG_ESP_CONSOLE_UART_NUM);
        esp_light_sleep_start();

        // 浅睡醒来恢复现场，并手动调用重启，使浅睡和深睡具有完全一致的“重新上电状态机初始化”常识逻辑
        esp_restart();
    }
}
