#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define BOOT_BUTTON_PIN 9
#define ESP_INTR_FLAG_DEFAULT 0

static const char *TAG = "BOOT_INTR";
static QueueHandle_t gpio_evt_queue = NULL;

// 1. 中断服务程序（ISR）：运行在中断上下文中，代码应当尽可能精简、运行迅速
static void IRAM_ATTR gpio_isr_handler(void* arg)
{
  uint32_t gpio_num = (uint32_t) arg;
  // 将触发中断的 GPIO 编号发送到队列中，通知用户任务处理
  xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

// 2. 按钮事件处理任务：负责从队列读取数据，运行在普通任务上下文中，可以执行耗时操作
static void button_task(void* arg)
{
  uint32_t io_num;
  for(;;) {
    // 阻塞等待队列消息
    if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)) {
      // 读取当前电平进行简单的状态确认（由于是下降沿触发，通常为 0）
      int level = gpio_get_level(io_num);
      ESP_LOGI(TAG, "GPIO[%ld] 中断触发! 当前电平: %d (按键被按下)", io_num, level);
    }
  }
}

void app_main(void)
{
  // 配置 GPIO 参数
  gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,       // 启用内部上拉
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_NEGEDGE         // 设置为下降沿触发（按键按下时电平从高变低）
};
  gpio_config(&io_conf);

  // 创建一个队列用于从 ISR 传递按键事件
  gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));

  // 启动处理按键事件的任务（优先级设为 10）
  xTaskCreate(button_task, "button_task", 2048, NULL, 10, NULL);

  // 安装 GPIO 中断服务
  gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);

  // 为特定的 GPIO 引脚挂载中断处理函数
  gpio_isr_handler_add(BOOT_BUTTON_PIN, gpio_isr_handler, (void*) BOOT_BUTTON_PIN);

  ESP_LOGI(TAG, "BOOT 按钮中断监听配置完成。");

  // 主任务可以去执行其他逻辑或休眠
  while (1) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
