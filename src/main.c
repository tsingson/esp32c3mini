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

// 需求：3 分钟定时自动唤醒 (3分钟 * 60秒 * 1,000,000微秒)
// 💡 提示：测试阶段强烈建议将其改为 (5ULL * 1000ULL * 1000ULL) 也就是 5 秒，以便极速验证连续多轮循环
#define SLEEP_DURATION_US (1ULL * 60ULL * 1000ULL * 1000ULL)

// 需求 2：Worker 任务的超时时间为 30 秒
#define WORKER_TIMEOUT_MS (10 * 1000)

static const char *TAG = "BOOT_NVS_DEMO";
static TaskHandle_t sleep_task_handle = NULL;
static uint32_t boot_isr_seq = 0;

// 1. BOOT 按下的底层外部中断服务（ISR）
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    // 瞬间通过轻量级任务通知解除目标任务的阻塞状态，保证微秒级的突发响应能力
    xTaskNotifyFromISR(sleep_task_handle, 0, eNoAction, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

// 统一的自增保存并睡眠流水线：A/B 两条分支在逻辑、闪存落盘和硬件配置上完全对等
static void save_sequence_and_enter_deep_sleep(const char* log_prefix)
{
    // 核心动作：在永久写入前，先对内存中的自增序号加 1
    boot_isr_seq++;

    // 核心动作：永久写入非易失性闪存(Flash)，防止任何非预期断电或物理拔线
    nvs_handle_t my_handle;
    if (nvs_open("storage", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_u32(my_handle, "boot_seq", boot_isr_seq);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }

    ESP_LOGW(TAG, ">>> %s [NVS 写入成功] 最新持久化序号已变更为: %ld", log_prefix, boot_isr_seq);
    ESP_LOGW(TAG, ">>> 正在剥离引脚并锁闭总线，芯片即将进入深睡眠...");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(200)); // 预留物理窗口期，确保内置 USB-CDC 芯片发完最后一批数据

    // 安全防御：在深睡前注销中断绑定，杜绝物理松手弹跳或环境悬空电磁波误触
    gpio_isr_handler_remove(BOOT_BUTTON_PIN);

    // 强制清理非预期唤醒源，并将内部低功耗计时器设为唯一允许的唤醒来源
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);

    // 彻底切断主 CPU 电源进入深度休眠
    esp_deep_sleep_start();
}

// 专门用来闪击 NVS 并将芯片送入深睡的任务（按键触发结局 A）
static void nvs_and_sleep_task(void* arg)
{
    while(1) {
        // 挂起自身，零消耗等待来自 ISR 的硬件同步信号
        xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);

        vTaskDelay(pdMS_TO_TICKS(40)); // 软件防抖
        if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {

            // 严格的松手死循环检测，确保用户手指脱离开发板
            while(gpio_get_level(BOOT_BUTTON_PIN) == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            vTaskDelay(pdMS_TO_TICKS(40)); // 释放弹跳消抖

            // 触发结局 A 流程
            save_sequence_and_enter_deep_sleep("【分叉 结局 A: BOOT 键按下】");
        }
    }
}

// 需求 1 & 2：独立的 Worker Task
static void worker_task(void* arg)
{
    uint32_t elapsed_ms = 0;
    const uint32_t interval_ms = 1000; // 每 3 秒执行一次打印动作

    ESP_LOGI("WORKER", "Worker 任务已就绪，正在开启 30 秒生命生存倒计时...");
    vTaskDelay(pdMS_TO_TICKS(100));
    while (1) {
        // 需求 1：这个 worker task 只做一个简单的任务, 那就是 打印自增序号
        ESP_LOGI("WORKER", "【工作正常】持久化序号 = %ld (当前运行进度: %ld / 30 秒)", boot_isr_seq, elapsed_ms / 1000);

        // 挂起 3 秒
        vTaskDelay(pdMS_TO_TICKS(interval_ms));
        elapsed_ms += interval_ms;

        // 需求 2：worker task 超过30秒时, 则调用 deep sleep 函数
        if (elapsed_ms >= WORKER_TIMEOUT_MS  ) {
            // 触发结局 B 流程
            save_sequence_and_enter_deep_sleep("【分叉 结局 B: 满 30 秒超时】");
        }
    }
}

void init_boot_pin(void) {
  gpio_config_t io_conf = {
      .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE, // 正常运行时依靠内部上拉维持绝对高电平
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_NEGEDGE // 锁定下降沿事件
  };
  gpio_config(&io_conf);
}
void app_main(void)
{
    // A. 开头强制等待 3 秒。非常核心，留足时间给电脑的串口监视器自动重新识别 USB-CDC。
    vTaskDelay(pdMS_TO_TICKS(3000));

    // B. 安全初始化非易失存储闪存库
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // C. 冷启动或唤醒后，第一时间深入闪存颗粒读取持久化变量
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        err = nvs_get_u32(my_handle, "boot_seq", &boot_isr_seq);
        if (err != ESP_OK) {
            // 如果是刚出厂/首次烧录，闪存中找不到此键，赋安全默认值 0
            boot_isr_seq = 0;
            nvs_set_u32(my_handle, "boot_seq", boot_isr_seq);
            nvs_commit(my_handle);
        }
        nvs_close(my_handle);
    } else {
        boot_isr_seq = 0;
    }

    // D. 判定本次开机是由时间自动叫醒，还是外部机械重置
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

    printf("\n\n");
    if (wakeup_reason == ESP_SLEEP_WAKEUP_TIMER) {
        // 需求 3：esp32 唤醒后, 进入上面的要求 1 并由 worker 重新接管打印
        ESP_LOGE(TAG, "==================================================");
        ESP_LOGE(TAG, "【⏰ 时间自动唤醒成功】休眠阶段圆满结束！");
        ESP_LOGE(TAG, "成功从非易失 Flash 中加载到最新的序号：%ld", boot_isr_seq);
        ESP_LOGE(TAG, "==================================================");
        vTaskDelay(pdMS_TO_TICKS(200));
    } else {
        ESP_LOGI(TAG, "==================================================");
        ESP_LOGI(TAG, "【🔌 物理冷上电 / RST 键手动复位成功】");
        ESP_LOGI(TAG, "当前从持久化闪存中安全加载的序号为: %ld", boot_isr_seq);
        ESP_LOGI(TAG, "==================================================");
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    printf("\n");

    // 创建最高优先级的保存处理任务，直接占领内核最高运行级别，防止被任何应用级业务挂起
    xTaskCreate(nvs_and_sleep_task, "flash_sleep_task", 4096, NULL, configMAX_PRIORITIES - 1, &sleep_task_handle);

    // 需求 1 & 3：创建 worker 任务使其开始/重新开始独立工作
    xTaskCreate(worker_task, "worker_task", 3072, NULL, 5, NULL);

    // E. 重新拉起并初始化 BOOT 键的外围数字控制层
    init_boot_pin();

    // 正式激活外部 GPIO 中断路由服务并绑定对应的处理句柄
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    gpio_isr_handler_add(BOOT_BUTTON_PIN, gpio_isr_handler, (void*) BOOT_BUTTON_PIN);
}
