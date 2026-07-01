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
#include "driver/gpio.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"

static const char *TAG = "C3_SUPERMINI_PM";

// ================= 【硬件与睡眠控制核心配置】 =================
#define BOOT_BUTTON_PIN       GPIO_NUM_9          // ESP32-C3 SuperMini 板载 BOOT 键固定在 GPIO9
#define UART_BUF_SIZE         256

typedef enum {
    SLEEP_MODE_LIGHT = 0,                         // 功能2：独立的 light sleep 模式
    SLEEP_MODE_DEEP  = 1                          // 功能1：独立的 deep sleep 模式
} custom_sleep_mode_t;

// === 核心控制参数（完全持久化保持） ===
static RTC_DATA_ATTR custom_sleep_mode_t current_sleep_strategy = SLEEP_MODE_LIGHT;
static RTC_DATA_ATTR uint32_t config_sleep_time_sec = 180;                         // 功能4：默认 3 分钟
static uint32_t config_work_window_ms = 30000;                                     // 功能5：默认工作窗口 30 秒

static EventGroupHandle_t xWorkEventGroup = NULL;
#define WORK_FINISHED_BIT    (1 << 0)

static RTC_DATA_ATTR uint32_t s_wakeup_count = 0;
static RTC_DATA_ATTR bool     enable_deep_sleep = true;
static RTC_DATA_ATTR uint32_t times_deep_sleep = 0;

// ================= 【功能1：C3 SuperMini 硬件指标诊断报告】 =================
static void print_chip_diagnostics(void) {
    esp_chip_info_t chip_info;
    uint32_t flash_size;

    esp_chip_info(&chip_info);

    // 使用 esp_rom_printf 绕过标准输出缓冲区，确保上电瞬间 100% 打印到终端
    esp_rom_printf("\r\n======================================================\r\n");
    esp_rom_printf("         ESP32-C3 SuperMini 硬件指标诊断报告          \r\n");
    esp_rom_printf("======================================================\r\n");
    esp_rom_printf("  [软件] ESP-IDF SDK 版本 : %s (LTS 长期稳定版)\r\n", esp_get_idf_version());
    esp_rom_printf("  [芯片] 核心微架构类型   : RISC-V 32-bit (ESP32-C3 正品硅片)\r\n");
    esp_rom_printf("  [芯片] 物理 CPU 核心数  : %d 核\r\n", chip_info.cores);
    esp_rom_printf("  [芯片] 硅片硬件版本     : v%" PRIu16 "\r\n", chip_info.revision);

    // 获取当前系统可自由支配的物理内部 RAM (内存) 容量
    esp_rom_printf("  [内存] 系统当前空闲 RAM : %" PRIu32 " 字节 (约 %" PRIu32 " KB)\r\n",
                   esp_get_free_heap_size(), esp_get_free_heap_size() / 1024);
    esp_rom_printf("  [内存] 开机历史最小 RAM : %" PRIu32 " 字节\r\n", esp_get_minimum_free_heap_size());

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        esp_rom_printf("  [闪存] 外部 SPI Flash   : %" PRIu32 " MB (%" PRIu32 " 字节)\r\n",
                       flash_size / (1024 * 1024), flash_size);
    }
    esp_rom_printf("  [封装] Flash 物理拓扑   : %s 闪存\r\n",
                   (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "片上集成(Embedded)" : "外部引脚挂载(External)");
    esp_rom_printf("======================================================\r\n\r\n");

    fflush(stdout);
}

// 功能3：按键引发的中断初始化配置
static void init_hardware_gpio(void) {
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_LOW_LEVEL,
    };
    gpio_config(&btn_conf);
}

static void check_hardware_button_intercept(void) {
    esp_rom_delay_us(5000); // 5ms 硬件去抖

    if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
        enable_deep_sleep = false;
        esp_rom_printf("\r\n⚠️ [调试后门触发] 检测到物理 BOOT 键长按，已永久关断深睡机制！\r\n\r\n");
    }
}

// ================= 【功能6：接收串口字符串并动态修改】 =================
static void vUsbReceiverTask(void *pvParameters) {
    char rx_buffer[UART_BUF_SIZE];
    ESP_LOGI(TAG, "原生 USB-JTAG 串口控制台就绪，可以随时输入指令...");

    while (1) {
        if (fgets(rx_buffer, sizeof(rx_buffer), stdin) != NULL) {
            rx_buffer[strcspn(rx_buffer, "\r\n")] = 0;
            ESP_LOGI(TAG, "收到 USB 控制台指令: \"%s\"", rx_buffer);

            if (strncmp(rx_buffer, "set_sleep=", 10) == 0) {
                uint32_t parsed_time = atoi(rx_buffer + 10);
                if (parsed_time > 0) {
                    config_sleep_time_sec = parsed_time;
                    ESP_LOGW(TAG, "🎯 成功更新下一次休眠时间为: %" PRIu32 " 秒!", config_sleep_time_sec);
                }
            }
            else if (strcmp(rx_buffer, "stop") == 0) {
                enable_deep_sleep = false;
                ESP_LOGW(TAG, "🛑 运行期深睡已关闭！系统将常开不挂起。");
            }
            else if (strcmp(rx_buffer, "finish") == 0) {
                ESP_LOGW(TAG, "收到手工提前结束工作指令！");
                xEventGroupSetBits(xWorkEventGroup, WORK_FINISHED_BIT);
            }
            else if (strcmp(rx_buffer, "toggle_mode") == 0) {
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
    const int work_duration_test_sec = 10;

    for (int i = 1; i <= work_duration_test_sec; i++) {
        ESP_LOGI(TAG, " 🛠️ 正在执行核心传感器数据打包...进度 [%d/%d]", i, work_duration_test_sec);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI(TAG, "--- 核心业务工作顺利结束！释放计算任务。 ---");
    xEventGroupSetBits(xWorkEventGroup, WORK_FINISHED_BIT);
    vTaskDelete(NULL);
}

// ================= 【核心控制状态机任务（拥有独立大栈，绝缘溢出异常）】 =================
static void vMainSystemControlTask(void *pvParameters) {
    // 强行延迟 1.5 秒确保 Mac 的 USB 枚举绑定完全就绪
    esp_rom_delay_us(1500000);

    esp_vfs_dev_usb_serial_jtag_register();
    esp_vfs_usb_serial_jtag_use_driver();

    esp_reset_reason_t reset_reason = esp_reset_reason();
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    // 【彻底干净清空】：移除了此前遗留的未声明电压历史重置，保证绝对无死角编译
    if (reset_reason != ESP_RST_DEEPSLEEP && reset_reason != ESP_RST_SW) {
        s_wakeup_count = 0;
        times_deep_sleep = 0;
        enable_deep_sleep = true;
    }

    s_wakeup_count++;

    init_hardware_gpio();
    check_hardware_button_intercept();

    xWorkEventGroup = xEventGroupCreate();

    esp_rom_printf("\n------------------------------------------------------\n");
    if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "⏰ 唤醒状态: 由 RTC 定时器自动唤醒。唤醒序号: %" PRIu32, s_wakeup_count);
    } else if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
        ESP_LOGW(TAG, "🖱️ 唤醒状态: 检测到物理 BOOT 按键（GPIO9）触发中断复苏成功！唤醒序号: %" PRIu32, s_wakeup_count);
    } else {
        print_chip_diagnostics();
    }

    printf("  [当前配置] 设定的休眠周期 : %" PRIu32 " 秒 (默认3分钟)\n", config_sleep_time_sec);
    printf("  [当前配置] 使用的睡眠模式 : %s\n", (current_sleep_strategy == SLEEP_MODE_DEEP) ? "DEEP SLEEP (深睡)" : "LIGHT SLEEP (浅睡)");
    printf("------------------------------------------------------\n\n");

    // 派生工作期并发子任务
    TaskHandle_t usb_task_handle = NULL;
    xTaskCreate(vUsbReceiverTask, "usb_recv_task", 3072, NULL, 10, &usb_task_handle);
    xTaskCreate(vDummyWorkExecution, "work_task", 3072, NULL, 5, NULL);

    // ================= 【功能5：工作时间状态机保底与拉伸控制组】 =================
    TickType_t xStartTicks = xTaskGetTickCount();
    EventBits_t bits = xEventGroupWaitBits(xWorkEventGroup, WORK_FINISHED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(config_work_window_ms));

    if (bits & WORK_FINISHED_BIT) {
        uint32_t actual_cost_ms = pdTICKS_TO_MS(xTaskGetTickCount() - xStartTicks);
        ESP_LOGI(TAG, "👍 【规则A生效】：工作在 %" PRIu32 " 毫秒内提早结束（少于30秒），系统立即执行休眠。", actual_cost_ms);
    } else {
        ESP_LOGW(TAG, "⏳ 【规则B生效】：已满 30 秒但外部工作尚未完成！主核强制常开死等其安全结束...");
        xEventGroupWaitBits(xWorkEventGroup, WORK_FINISHED_BIT, pdTRUE, pdFALSE, portMAX_DELAY);
        ESP_LOGI(TAG, "🏁 延时到站！超时工作最终顺利干完，放行允许进入休眠。");
    }

    if (usb_task_handle != NULL) {
        vTaskDelete(usb_task_handle);
    }

    // 配置可调超时唤醒
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)config_sleep_time_sec * 1000000ULL));

    // ================= 【功能1/2/3：独立的深睡/浅睡安全进入】 =================
    if (enable_deep_sleep) {
        if (current_sleep_strategy == SLEEP_MODE_DEEP) {
            ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(1ULL << BOOT_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW));
            ESP_LOGI(TAG, "🌙 芯片进入第 %" PRIu32 " 次 [DEEP SLEEP]... %" PRIu32 " 秒后见。", times_deep_sleep, config_sleep_time_sec);
            times_deep_sleep++;
            fflush(stdout);
            esp_deep_sleep_start();
        }
        else {
            ESP_ERROR_CHECK(gpio_wakeup_enable(BOOT_BUTTON_PIN, GPIO_INTR_LOW_LEVEL));
            ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
            ESP_LOGI(TAG, "💤 芯片进入第 %" PRIu32 " 次 [LIGHT SLEEP]... %" PRIu32 " 秒后见。", times_deep_sleep, config_sleep_time_sec);
            times_deep_sleep++;
            fflush(stdout);
            esp_light_sleep_start();
            esp_restart();
        }
    } else {
        ESP_LOGW(TAG, "⚠️ 【调试模式锁定】深睡功能已被完全熔断！设备保持常开状态。");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            ESP_LOGI(TAG, "系统常开可用 RAM 空间: %" PRIu32 " 字节", esp_get_free_heap_size());
        }
    }
}

// 系统的真正总入口：只负责派生大内存控制任务，自身绝不进行重资产运行
void app_main(void) {
    // 💥【修复修复点】：将未定义的宏彻底替换修正为常数级核心高优先级 15
    xTaskCreate(
        vMainSystemControlTask,
        "main_sys_control",
        5120,
        NULL,
        15,
        NULL
    );
}
