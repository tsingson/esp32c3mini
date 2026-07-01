#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#define BOOT_BUTTON_PIN 9

void app_main(void)
{
  // 配置 GPIO 参数
  gpio_config_t io_conf = {
    .pin_bit_mask = (1ULL << BOOT_BUTTON_PIN),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE, // 启用内部上拉
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE     // 如有需要也可改为中断模式
};
  gpio_config(&io_conf);

  while (1) {
    // 读取按键电平，按下为 0
    if (gpio_get_level(BOOT_BUTTON_PIN) == 0) {
      printf("BOOT Button Pressed (ESP-IDF)!\n");
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}
